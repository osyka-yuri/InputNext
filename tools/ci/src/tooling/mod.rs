//! Pinned host tools, safe archive handling, and compiler discovery.

#[cfg(test)]
mod tests;

mod archive;
mod codeql;
pub mod compiler;
mod download;
mod lock;
mod probe;
mod report;

use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs;
use std::path::{Component, Path, PathBuf};

use anyhow::{Context, Result, ensure};
use clap::{Args, Subcommand, ValueEnum};
use semver::Version;
use tempfile::TempDir;

use crate::process::Runner;

use self::archive::ArchiveFormat;
use self::lock::{ToolAsset, ToolchainLock};
pub(crate) use self::report::write_json_report;
use self::report::{ToolEntry, ToolKind, ToolchainReport};
pub use self::report::{Toolset, load_toolset};

#[derive(Debug, Subcommand)]
pub enum ToolchainCommand {
    Install(InstallArgs),
    Compiler(compiler::CompilerArgs),
    Path(PathArgs),
}

impl ToolchainCommand {
    /// Executes one toolchain setup or report query.
    ///
    /// # Errors
    ///
    /// Returns an error if the requested operation cannot validate or use its inputs.
    pub fn run(self, runner: &mut dyn Runner) -> Result<()> {
        match self {
            Self::Install(args) => args.run(runner),
            Self::Compiler(args) => compiler::run(&args, runner),
            Self::Path(args) => args.run(),
        }
    }
}

#[derive(Debug, Args)]
pub struct PathArgs {
    #[arg(long)]
    report: PathBuf,
    #[arg(long)]
    name: String,
}

impl PathArgs {
    fn run(self) -> Result<()> {
        let toolset = load_toolset(&self.report)?;
        let path = toolset.executable(&self.name)?;
        println!("{}", path.display());
        Ok(())
    }
}

#[derive(Debug, Args)]
pub struct InstallArgs {
    #[arg(long)]
    lock: PathBuf,
    #[arg(long)]
    cache: PathBuf,
    #[arg(long)]
    destination: PathBuf,
    #[arg(long, value_enum)]
    profile: InstallProfile,
    #[arg(long)]
    report: PathBuf,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, ValueEnum)]
enum InstallProfile {
    Build,
    Quality,
    Release,
    Codeql,
    Lint,
}

impl InstallProfile {
    fn as_str(self) -> &'static str {
        match self {
            Self::Build => "build",
            Self::Quality => "quality",
            Self::Release => "release",
            Self::Codeql => "codeql",
            Self::Lint => "lint",
        }
    }
}

impl InstallArgs {
    fn run(self, runner: &mut dyn Runner) -> Result<()> {
        let lock_path = dunce::canonicalize(&self.lock)
            .with_context(|| format!("could not resolve toolchain lock {}", self.lock.display()))?;
        let lock = ToolchainLock::read(&lock_path)?;
        let lock_sha256 = download::hash_file(&lock_path)?;
        let platform = current_platform()?;
        let (tools, dependency_names) = profile_contents(self.profile, platform)?;
        validate_lock_selection(&lock, platform, &tools, &dependency_names)?;

        let cache_root = create_absolute_directory(&self.cache)?;
        let destination = create_absolute_directory(&self.destination)?;
        let (install_root, module_root) =
            prepare_install_roots(&destination, tools.contains("psscriptanalyzer"))?;
        let mut entries = install_tools(
            runner,
            &lock,
            platform,
            tools,
            &cache_root,
            &install_root,
            module_root.as_deref(),
        )?;
        let mut environment = tool_environment(&entries, module_root.as_deref())?;
        install_dependencies(
            runner,
            &lock,
            dependency_names,
            &cache_root,
            &destination,
            &mut entries,
            &mut environment,
        )?;

        let report = ToolchainReport {
            schema_version: 1,
            profile: self.profile.as_str().to_owned(),
            platform: platform.as_str().to_owned(),
            lock_sha256,
            tools: entries,
            environment,
        };
        write_json_report(&self.report, &report)?;
        Ok(())
    }
}

fn prepare_install_roots(
    destination: &Path,
    include_modules: bool,
) -> Result<(PathBuf, Option<PathBuf>)> {
    let install_root = destination.join("tools");
    fs::create_dir_all(&install_root).context("could not create tool installation root")?;
    let install_root = dunce::canonicalize(&install_root)?;
    let module_root = if include_modules {
        let path = destination.join("modules");
        fs::create_dir_all(&path).context("could not create module installation root")?;
        Some(dunce::canonicalize(&path)?)
    } else {
        None
    };
    Ok((install_root, module_root))
}

fn install_tools(
    runner: &mut dyn Runner,
    lock: &ToolchainLock,
    platform: HostPlatform,
    names: BTreeSet<String>,
    cache_root: &Path,
    install_root: &Path,
    module_root: Option<&Path>,
) -> Result<BTreeMap<String, ToolEntry>> {
    let mut entries = BTreeMap::new();
    let mut order = names.into_iter().collect::<Vec<_>>();
    order.sort_by_key(|name| (name != "powershell", name.clone()));
    for name in order {
        let definition = lock
            .tools
            .get(&name)
            .with_context(|| format!("toolchain lock has no '{name}' tool"))?;
        let asset = definition
            .platforms
            .get(platform.as_str())
            .with_context(|| format!("tool '{name}' has no {} archive", platform.as_str()))?;
        let kind = tool_kind(definition.kind.as_deref(), &asset.executable)?;
        let entry = install_tool(
            runner,
            &name,
            &definition.version,
            asset,
            kind,
            cache_root,
            install_root,
            module_root,
            &entries,
        )?;
        entries.insert(name, entry);
    }
    Ok(entries)
}

fn tool_environment(
    entries: &BTreeMap<String, ToolEntry>,
    module_root: Option<&Path>,
) -> Result<BTreeMap<String, String>> {
    let mut environment = BTreeMap::new();
    let directories = entries
        .values()
        .filter(|entry| entry.kind == ToolKind::Executable)
        .filter_map(|entry| entry.path.parent().map(Path::to_path_buf))
        .collect::<BTreeSet<_>>();
    let search_path =
        env::join_paths(directories).context("could not encode installed tool search paths")?;
    environment.insert(
        "PATH".to_owned(),
        search_path.to_string_lossy().into_owned(),
    );
    if let Some(module_root) = module_root {
        environment.insert(
            "PSModulePath".to_owned(),
            module_root.to_string_lossy().into_owned(),
        );
    }
    Ok(environment)
}

fn install_dependencies(
    runner: &mut dyn Runner,
    lock: &ToolchainLock,
    names: BTreeSet<String>,
    cache_root: &Path,
    destination: &Path,
    entries: &mut BTreeMap<String, ToolEntry>,
    environment: &mut BTreeMap<String, String>,
) -> Result<()> {
    for name in names {
        let dependency = lock
            .dependencies
            .get(&name)
            .with_context(|| format!("toolchain lock has no '{name}' dependency"))?;
        ensure!(
            name == "codeqlBundle",
            "unsupported tool dependency '{name}'"
        );
        let path = codeql::install_bundle(runner, dependency, cache_root, destination)?;
        entries.insert(
            name,
            ToolEntry {
                version: dependency.version.clone(),
                path: path.clone(),
                kind: ToolKind::Archive,
                archive: Some(path.clone()),
                sha256: dependency.sha256.clone(),
            },
        );
        environment.insert(
            "INPUTNEXT_CODEQL_BUNDLE".to_owned(),
            path.to_string_lossy().into_owned(),
        );
        environment.insert(
            "INPUTNEXT_CODEQL_BUNDLE_VERSION".to_owned(),
            dependency.version.clone(),
        );
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum HostPlatform {
    WindowsX64,
    LinuxX64,
}

impl HostPlatform {
    fn as_str(self) -> &'static str {
        match self {
            Self::WindowsX64 => "windows-x64",
            Self::LinuxX64 => "linux-x64",
        }
    }
}

fn current_platform() -> Result<HostPlatform> {
    ensure!(
        env::consts::ARCH == "x86_64",
        "locked CI tools support x64 hosts only"
    );
    match env::consts::OS {
        "windows" => Ok(HostPlatform::WindowsX64),
        "linux" => Ok(HostPlatform::LinuxX64),
        other => anyhow::bail!("locked CI tools do not support host OS '{other}'"),
    }
}

fn profile_contents(
    profile: InstallProfile,
    platform: HostPlatform,
) -> Result<(BTreeSet<String>, BTreeSet<String>)> {
    let entries = match (platform, profile) {
        (HostPlatform::WindowsX64, InstallProfile::Build) => {
            (names(&["cmake", "ninja", "powershell"]), names(&[]))
        }
        (HostPlatform::WindowsX64, InstallProfile::Quality) => (
            names(&["cmake", "ninja", "powershell", "llvm", "psscriptanalyzer"]),
            names(&[]),
        ),
        (HostPlatform::WindowsX64, InstallProfile::Release) => (
            names(&["cmake", "ninja", "powershell", "gh", "git-cliff"]),
            names(&[]),
        ),
        (HostPlatform::WindowsX64, InstallProfile::Codeql) => (
            names(&["cmake", "ninja", "powershell"]),
            names(&["codeqlBundle"]),
        ),
        (HostPlatform::WindowsX64, InstallProfile::Lint) => (
            names(&[
                "powershell",
                "actionlint",
                "shellcheck",
                "git-cliff",
                "psscriptanalyzer",
            ]),
            names(&[]),
        ),
        (HostPlatform::LinuxX64, InstallProfile::Release) => (names(&["gh"]), names(&[])),
        (HostPlatform::LinuxX64, InstallProfile::Lint) => (
            names(&["actionlint", "shellcheck", "git-cliff"]),
            names(&[]),
        ),
        (HostPlatform::LinuxX64, _) => anyhow::bail!(
            "the '{}' toolchain profile requires Windows x64",
            profile.as_str()
        ),
    };
    Ok(entries)
}

fn names(values: &[&str]) -> BTreeSet<String> {
    values.iter().map(|value| (*value).to_owned()).collect()
}

fn validate_lock_selection(
    lock: &ToolchainLock,
    platform: HostPlatform,
    tools: &BTreeSet<String>,
    dependencies: &BTreeSet<String>,
) -> Result<()> {
    for name in tools {
        let definition = lock
            .tools
            .get(name)
            .with_context(|| format!("toolchain lock has no '{name}' tool"))?;
        ensure!(
            definition.platforms.contains_key(platform.as_str()),
            "tool '{name}' has no {} asset",
            platform.as_str()
        );
        validate_version(&definition.version)?;
    }
    for name in dependencies {
        let definition = lock
            .dependencies
            .get(name)
            .with_context(|| format!("toolchain lock has no '{name}' dependency"))?;
        validate_version(&definition.version)?;
        download::validate_sha256(&definition.sha256)?;
    }
    Ok(())
}

fn validate_version(version: &str) -> Result<()> {
    Version::parse(version).with_context(|| format!("locked version '{version}' is not SemVer"))?;
    Ok(())
}

fn tool_kind(kind: Option<&str>, executable: &str) -> Result<ToolKind> {
    match kind {
        Some("executable") => Ok(ToolKind::Executable),
        Some("module") => Ok(ToolKind::Module),
        Some(other) => anyhow::bail!("unsupported locked tool kind '{other}'"),
        None if Path::new(executable)
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("psd1")) =>
        {
            Ok(ToolKind::Module)
        }
        None => Ok(ToolKind::Executable),
    }
}

#[allow(
    clippy::too_many_arguments,
    reason = "the installer keeps archive, tool, and report ownership explicit"
)]
fn install_tool(
    runner: &mut dyn Runner,
    name: &str,
    version: &str,
    asset: &ToolAsset,
    kind: ToolKind,
    cache_root: &Path,
    install_root: &Path,
    module_root: Option<&Path>,
    installed: &BTreeMap<String, ToolEntry>,
) -> Result<ToolEntry> {
    validate_version(version)?;
    download::validate_sha256(&asset.sha256)?;
    let format = ArchiveFormat::parse(&asset.archive)?;
    validate_locked_relative_path(&asset.executable)?;
    let archive = download::verified_archive(
        runner,
        cache_root,
        &asset.url,
        &asset.sha256,
        &asset.archive,
    )?;

    let base = match kind {
        ToolKind::Executable => install_root,
        ToolKind::Module => module_root.with_context(|| {
            format!("tool '{name}' is a module but the module root is unavailable")
        })?,
        ToolKind::Archive => anyhow::bail!("tool '{name}' cannot declare archive kind"),
    };
    let version_directory = match kind {
        ToolKind::Module => name.to_owned(),
        _ => safe_component(name)?,
    };
    let destination = base.join(version_directory).join(version);
    ensure!(
        destination.starts_with(base),
        "tool destination escaped install root"
    );

    let staging =
        TempDir::new_in(base).context("could not create fresh tool extraction directory")?;
    archive::extract(&archive, format, staging.path())?;
    let staging_executable = staging.path().join(&asset.executable);
    ensure!(
        staging_executable.starts_with(staging.path()),
        "tool executable escaped extraction root"
    );
    let metadata = fs::symlink_metadata(&staging_executable)
        .with_context(|| format!("archive is missing required path '{}'", asset.executable))?;
    ensure!(metadata.is_file(), "locked tool path is not a regular file");

    match kind {
        ToolKind::Executable => {
            probe::executable(runner, name, asset, &staging_executable, staging.path())?;
        }
        ToolKind::Module => probe::module(runner, installed, asset, version, &staging_executable)?,
        ToolKind::Archive => anyhow::bail!("archive tools are not installed through tool archives"),
    }

    replace_install_directory(&staging, &destination, base)?;
    let path = destination.join(&asset.executable);
    ensure!(
        path.is_absolute() && path.is_file(),
        "installed tool path was not published"
    );
    Ok(ToolEntry {
        version: version.to_owned(),
        path,
        kind,
        archive: Some(archive),
        sha256: asset.sha256.clone(),
    })
}

fn replace_install_directory(staging: &TempDir, destination: &Path, base: &Path) -> Result<()> {
    let base = dunce::canonicalize(base).context("could not resolve tool install root")?;
    ensure!(
        destination.starts_with(&base),
        "tool destination escaped install root"
    );
    let parent = destination
        .parent()
        .context("tool destination has no parent")?;
    fs::create_dir_all(parent).context("could not create tool destination parent")?;
    let parent =
        dunce::canonicalize(parent).context("could not resolve tool destination parent")?;
    ensure!(
        parent.starts_with(&base),
        "tool destination parent escaped install root"
    );
    if let Ok(metadata) = fs::symlink_metadata(destination) {
        ensure!(
            !metadata.file_type().is_symlink(),
            "existing tool destination must not be a symbolic link"
        );
        ensure!(
            metadata.is_dir(),
            "existing tool destination is not a directory"
        );
        fs::remove_dir_all(destination).context("could not replace previous tool installation")?;
    }
    fs::rename(staging.path(), destination)
        .context("could not publish verified tool installation")?;
    Ok(())
}

fn validate_locked_relative_path(value: &str) -> Result<()> {
    let path = Path::new(value);
    ensure!(!value.is_empty(), "locked tool path must not be empty");
    ensure!(
        path.components()
            .all(|component| matches!(component, Component::Normal(_))),
        "locked tool path must be canonical and relative"
    );
    ensure!(
        !value.contains(['\\', ':', '\0']),
        "locked tool path has a forbidden character"
    );
    ensure!(
        value.split('/').all(|component| !component.is_empty()),
        "locked tool path is not canonical"
    );
    Ok(())
}

fn safe_component(value: &str) -> Result<String> {
    ensure!(
        !value.is_empty()
            && value.chars().all(
                |character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_')
            ),
        "tool name is not a safe path component"
    );
    Ok(value.to_owned())
}

fn create_absolute_directory(path: &Path) -> Result<PathBuf> {
    fs::create_dir_all(path).with_context(|| format!("could not create {}", path.display()))?;
    let resolved = dunce::canonicalize(path)
        .with_context(|| format!("could not resolve {}", path.display()))?;
    let metadata = fs::symlink_metadata(&resolved).context("could not inspect directory")?;
    ensure!(metadata.is_dir(), "path is not a directory");
    Ok(resolved)
}
