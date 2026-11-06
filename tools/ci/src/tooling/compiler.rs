use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use clap::{Args, ValueEnum};
use regex::Regex;
use serde::Serialize;
use tempfile::TempDir;

use crate::process::{CommandOutput, CommandSpec, Runner, run_checked};

use super::download;
use super::lock::CompilerDefinition;
use super::{load_toolset, write_json_report};

const PROBE_TIMEOUT: Duration = Duration::from_secs(30);
const INSTALL_TIMEOUT: Duration = Duration::from_hours(3);
const OUTPUT_LIMIT: usize = 4 * 1024 * 1024;

#[derive(Debug, Args)]
pub struct CompilerArgs {
    #[arg(long)]
    lock: PathBuf,
    #[arg(long)]
    toolset: PathBuf,
    #[arg(long)]
    cache: PathBuf,
    #[arg(long)]
    destination: PathBuf,
    #[arg(long, value_enum)]
    family: CompilerFamily,
    #[arg(long)]
    report: PathBuf,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, ValueEnum)]
enum CompilerFamily {
    Production,
    Codeql,
}

impl CompilerFamily {
    fn lock_key(self) -> &'static str {
        match self {
            Self::Production => "production",
            Self::Codeql => "codeql",
        }
    }
}

#[derive(Debug, Serialize)]
#[serde(deny_unknown_fields)]
struct CompilerReport {
    schema_version: u32,
    family: String,
    installation_root: PathBuf,
    toolset_root: PathBuf,
    vcvars_version: String,
    cl_path: PathBuf,
    cl_version: String,
}

pub(super) fn run(args: &CompilerArgs, runner: &mut dyn Runner) -> Result<()> {
    ensure!(
        cfg!(windows),
        "Visual Studio compiler setup is available only on Windows"
    );
    let lock_path = dunce::canonicalize(&args.lock)
        .with_context(|| format!("could not resolve toolchain lock {}", args.lock.display()))?;
    let lock = super::lock::ToolchainLock::read(&lock_path)?;
    let lock_sha256 = download::hash_file(&lock_path)?;
    let definition = lock
        .compilers
        .get(args.family.lock_key())
        .with_context(|| {
            format!(
                "toolchain lock has no '{}' compiler",
                args.family.lock_key()
            )
        })?;
    let toolset = load_toolset(&args.toolset)?;
    ensure!(
        toolset.lock_sha256() == lock_sha256,
        "toolchain report does not match the current lock"
    );
    ensure!(
        toolset.platform() == "windows-x64",
        "Visual Studio setup requires a Windows x64 toolchain report"
    );
    let powershell = toolset.executable("powershell")?.to_path_buf();
    let destination = absolute_destination(&args.destination)?;

    let mut installation = find_installation(
        runner,
        &powershell,
        definition,
        &destination,
        args.family.lock_key(),
    )?;
    if installation.is_none() && args.family == CompilerFamily::Production {
        ensure_github_hosted_runner()?;
        install_production_compiler(runner, &powershell, definition, &args.cache, &destination)?;
        installation = find_installation(
            runner,
            &powershell,
            definition,
            &destination,
            args.family.lock_key(),
        )?;
    }

    let installation = installation.with_context(|| {
        format!(
            "the exact locked {} compiler {} is not installed",
            args.family.lock_key(),
            definition.toolset_version
        )
    })?;
    let output = CompilerReport {
        schema_version: 1,
        family: args.family.lock_key().to_owned(),
        installation_root: installation.root,
        toolset_root: installation.toolset_root,
        vcvars_version: definition.vcvars_version.clone(),
        cl_path: installation.cl_path,
        cl_version: installation.cl_version,
    };
    write_json_report(&args.report, &output)
}

#[derive(Debug)]
struct Installation {
    root: PathBuf,
    toolset_root: PathBuf,
    cl_path: PathBuf,
    cl_version: String,
}

fn find_installation(
    runner: &mut dyn Runner,
    powershell: &Path,
    compiler: &CompilerDefinition,
    side_by_side_root: &Path,
    family: &str,
) -> Result<Option<Installation>> {
    let mut roots = visual_studio_roots(runner)?;
    if side_by_side_root.is_dir() {
        roots.push(side_by_side_root.to_path_buf());
    }
    roots.sort();
    roots.dedup();

    let expected_version = Regex::new(&compiler.cl_file_version_regex)
        .context("compiler lock contains an invalid file version regex")?;
    for installation_root in roots {
        let Some(toolset_root) = find_toolset_root(&installation_root, &compiler.toolset_version)?
        else {
            continue;
        };
        let compiler_paths = [
            toolset_root.join("bin/Hostx64/x64/cl.exe"),
            toolset_root.join("bin/Hostx64/x86/cl.exe"),
        ];
        let mut versions = Vec::with_capacity(compiler_paths.len());
        let mut valid = true;
        for path in &compiler_paths {
            if !path.is_file() {
                valid = false;
                break;
            }
            let version = file_version(runner, powershell, path)?;
            if !expected_version.is_match(&version) {
                valid = false;
                break;
            }
            versions.push(version);
        }
        if !valid || versions.len() != compiler_paths.len() || versions[0] != versions[1] {
            continue;
        }

        let dev_shell = installation_root.join("Common7/Tools/Microsoft.VisualStudio.DevShell.dll");
        ensure!(
            dev_shell.is_file(),
            "Visual Studio DevShell module is missing from the locked {family} compiler"
        );
        return Ok(Some(Installation {
            root: installation_root,
            toolset_root,
            cl_path: compiler_paths[0].clone(),
            cl_version: versions[0].clone(),
        }));
    }
    Ok(None)
}

fn visual_studio_roots(runner: &mut dyn Runner) -> Result<Vec<PathBuf>> {
    let mut roots = Vec::new();
    for environment_name in ["ProgramFiles(x86)", "ProgramFiles"] {
        if let Some(program_files) = env::var_os(environment_name) {
            let vswhere =
                PathBuf::from(program_files).join("Microsoft Visual Studio/Installer/vswhere.exe");
            if vswhere.is_file() {
                roots.extend(query_vswhere(runner, &vswhere)?);
                break;
            }
        }
    }
    Ok(roots)
}

fn query_vswhere(runner: &mut dyn Runner, executable: &Path) -> Result<Vec<PathBuf>> {
    let cwd = current_directory()?;
    let spec = CommandSpec {
        program: dunce::canonicalize(executable).context("could not resolve vswhere executable")?,
        args: [
            "-all",
            "-products",
            "*",
            "-property",
            "installationPath",
            "-utf8",
        ]
        .into_iter()
        .map(Into::into)
        .collect(),
        cwd,
        env: BTreeMap::new(),
        timeout: PROBE_TIMEOUT,
        output_limit: OUTPUT_LIMIT,
    };
    let output =
        run_checked(runner, &spec).context("could not enumerate Visual Studio installations")?;
    let text = String::from_utf8_lossy(&output.stdout);
    Ok(text
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(PathBuf::from)
        .filter(|path| path.is_absolute() && path.is_dir())
        .collect())
}

fn find_toolset_root(installation_root: &Path, expected: &str) -> Result<Option<PathBuf>> {
    let tools_root = installation_root.join("VC/Tools/MSVC");
    if !tools_root.is_dir() {
        return Ok(None);
    }
    let exact = tools_root.join(expected);
    if exact.is_dir() {
        return Ok(Some(exact));
    }
    let mut candidates = fs::read_dir(&tools_root)
        .with_context(|| format!("could not inspect {}", tools_root.display()))?
        .filter_map(std::result::Result::ok)
        .filter(|entry| entry.file_type().is_ok_and(|file_type| file_type.is_dir()))
        .filter(|entry| {
            entry
                .file_name()
                .to_str()
                .is_some_and(|name| name.starts_with(&format!("{expected}.")))
        })
        .map(|entry| entry.path())
        .collect::<Vec<_>>();
    candidates.sort();
    Ok((candidates.len() == 1).then(|| candidates.remove(0)))
}

fn file_version(runner: &mut dyn Runner, powershell: &Path, path: &Path) -> Result<String> {
    let script = r"
param([string] $FilePath)
$ErrorActionPreference = 'Stop'
(Get-Item -LiteralPath $FilePath).VersionInfo.FileVersion
";
    let temporary = TempDir::new().context("could not create compiler probe directory")?;
    let script_path = temporary.path().join("read-file-version.ps1");
    fs::write(&script_path, script).context("could not write compiler probe script")?;
    let spec = CommandSpec {
        program: powershell.to_path_buf(),
        args: vec![
            "-NoLogo".into(),
            "-NoProfile".into(),
            "-NonInteractive".into(),
            "-File".into(),
            script_path.into_os_string(),
            path.as_os_str().to_owned(),
        ],
        cwd: temporary.path().to_path_buf(),
        env: BTreeMap::new(),
        timeout: PROBE_TIMEOUT,
        output_limit: OUTPUT_LIMIT,
    };
    let output = run_checked(runner, &spec).context("could not probe cl.exe file version")?;
    output_text(&output).map(|text| text.trim().to_owned())
}

fn install_production_compiler(
    runner: &mut dyn Runner,
    powershell: &Path,
    compiler: &CompilerDefinition,
    cache: &Path,
    destination: &Path,
) -> Result<()> {
    let bootstrap = compiler
        .bootstrapper
        .as_ref()
        .context("production compiler lock has no bootstrapper")?;
    ensure!(
        !compiler.components.is_empty(),
        "production compiler lock has no installation components"
    );
    ensure!(
        compiler
            .components
            .iter()
            .all(|component| component.starts_with("Microsoft.VisualStudio.Component.")),
        "compiler lock contains an unsupported Visual Studio component"
    );
    for url in [
        &bootstrap.url,
        &bootstrap.channel_uri,
        &bootstrap.install_channel_uri,
    ] {
        ensure!(
            url.starts_with("https://"),
            "compiler bootstrap URLs must use HTTPS"
        );
    }

    let archive =
        download::verified_archive(runner, cache, &bootstrap.url, &bootstrap.sha256, "file")?;
    verify_microsoft_signature(runner, powershell, &archive)?;
    if let Ok(metadata) = fs::symlink_metadata(destination) {
        ensure!(
            !metadata.file_type().is_symlink(),
            "existing Visual Studio destination must not be a symbolic link"
        );
        ensure!(
            metadata.is_dir(),
            "existing Visual Studio destination is not a directory"
        );
        ensure!(
            fs::read_dir(destination)?.next().is_none(),
            "existing Visual Studio destination contains files and was preserved"
        );
    } else {
        fs::create_dir_all(
            destination
                .parent()
                .context("compiler destination has no parent")?,
        )
        .context("could not create compiler destination parent")?;
    }

    let mut args = vec![
        "--quiet".to_owned(),
        "--wait".to_owned(),
        "--norestart".to_owned(),
        "--nocache".to_owned(),
        "--installPath".to_owned(),
        destination.to_string_lossy().into_owned(),
        "--channelUri".to_owned(),
        bootstrap.channel_uri.clone(),
        "--installChannelUri".to_owned(),
        bootstrap.install_channel_uri.clone(),
    ];
    for component in &compiler.components {
        args.extend(["--add".to_owned(), component.clone()]);
    }
    args.push("--includeRecommended".to_owned());
    let spec = CommandSpec {
        program: archive,
        args: args.into_iter().map(Into::into).collect(),
        cwd: current_directory()?,
        env: BTreeMap::new(),
        timeout: INSTALL_TIMEOUT,
        output_limit: OUTPUT_LIMIT,
    };
    let output = runner
        .run(&spec)
        .context("could not launch the pinned Visual Studio bootstrapper")?;
    ensure!(
        matches!(output.code, Some(0 | 3010)),
        "pinned Visual Studio bootstrapper failed"
    );
    Ok(())
}

fn verify_microsoft_signature(
    runner: &mut dyn Runner,
    powershell: &Path,
    bootstrapper: &Path,
) -> Result<()> {
    let script = r"
param([string] $FilePath)
$ErrorActionPreference = 'Stop'
$signature = Get-AuthenticodeSignature -LiteralPath $FilePath
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $signature.SignerCertificate.Subject -notmatch '(^|,)\s*CN=Microsoft Corporation(?:,|$)') {
    throw 'The signed bootstrapper was not validly signed by Microsoft Corporation.'
}
";
    let temporary = TempDir::new().context("could not create signature verification directory")?;
    let script_path = temporary.path().join("verify-signature.ps1");
    fs::write(&script_path, script).context("could not write signature verification script")?;
    let spec = CommandSpec {
        program: powershell.to_path_buf(),
        args: vec![
            "-NoLogo".into(),
            "-NoProfile".into(),
            "-NonInteractive".into(),
            "-File".into(),
            script_path.into_os_string(),
            bootstrapper.as_os_str().to_owned(),
        ],
        cwd: temporary.path().to_path_buf(),
        env: BTreeMap::new(),
        timeout: PROBE_TIMEOUT,
        output_limit: OUTPUT_LIMIT,
    };
    run_checked(runner, &spec)
        .context("Visual Studio bootstrapper signature verification failed")?;
    Ok(())
}

fn ensure_github_hosted_runner() -> Result<()> {
    ensure!(
        env::var("GITHUB_ACTIONS").is_ok_and(|value| value == "true")
            && env::var("RUNNER_ENVIRONMENT").is_ok_and(|value| value == "github-hosted"),
        "Visual Studio installation is permitted only on a GitHub-hosted Actions runner"
    );
    Ok(())
}

fn absolute_destination(path: &Path) -> Result<PathBuf> {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        current_directory()?.join(path)
    };
    let parent = absolute
        .parent()
        .context("compiler destination has no parent")?;
    fs::create_dir_all(parent).context("could not create compiler destination parent")?;
    let parent =
        dunce::canonicalize(parent).context("could not resolve compiler destination parent")?;
    let name = absolute
        .file_name()
        .context("compiler destination has no final path component")?;
    Ok(parent.join(name))
}

fn current_directory() -> Result<PathBuf> {
    let current = env::current_dir().context("could not read current working directory")?;
    dunce::canonicalize(&current).context("could not resolve current working directory")
}

fn output_text(output: &CommandOutput) -> Result<String> {
    let text = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    ensure!(!text.is_empty(), "compiler probe returned no version");
    Ok(text)
}
