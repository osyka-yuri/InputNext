#![cfg(windows)]

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};

use super::probe;
use super::report::{ToolEntry, ToolKind, ToolchainReport};
use super::{ToolAsset, load_toolset, prepare_install_roots, tool_environment, write_json_report};
use crate::process::{CommandOutput, CommandSpec, Runner};

struct ModuleProbeRunner {
    manifest: PathBuf,
}

impl Runner for ModuleProbeRunner {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        let manifest_argument = spec
            .args
            .get(5)
            .context("PowerShell module probe omitted the manifest path")?;
        ensure!(
            Path::new(manifest_argument) == self.manifest,
            "PowerShell module probe received another manifest path"
        );
        ensure!(
            !manifest_argument.to_string_lossy().starts_with(r"\\?\"),
            "PowerShell module probe received a verbatim Win32 path"
        );
        Ok(CommandOutput {
            code: Some(0),
            stdout: b"1.25.0\r\n".to_vec(),
            stderr: Vec::new(),
        })
    }
}

#[test]
fn short_temp_tool_report_paths_work_with_module_probe() -> Result<()> {
    let temporary = tempfile::Builder::new().prefix("ci").tempdir()?;
    let destination = temporary.path().join("tools");
    let (install_root, module_root) = prepare_install_roots(&destination, true)?;
    let module_root = module_root.context("module install root was not created")?;
    ensure_regular_win32_path(&install_root)?;
    ensure_regular_win32_path(&module_root)?;

    let powershell_path = install_root.join("powershell/pwsh.exe");
    fs::create_dir_all(
        powershell_path
            .parent()
            .context("PowerShell executable has no parent")?,
    )?;
    fs::write(&powershell_path, b"powershell fixture")?;

    let manifest_path = module_root.join("psscriptanalyzer/1.25.0/PSScriptAnalyzer.psd1");
    fs::create_dir_all(
        manifest_path
            .parent()
            .context("module manifest has no parent")?,
    )?;
    fs::write(&manifest_path, b"module manifest fixture")?;

    let digest = "0".repeat(64);
    let mut tools = BTreeMap::new();
    tools.insert(
        "powershell".to_owned(),
        ToolEntry {
            version: "7.6.9".to_owned(),
            path: powershell_path,
            kind: ToolKind::Executable,
            archive: None,
            sha256: digest.clone(),
        },
    );
    tools.insert(
        "psscriptanalyzer".to_owned(),
        ToolEntry {
            version: "1.25.0".to_owned(),
            path: manifest_path.clone(),
            kind: ToolKind::Module,
            archive: None,
            sha256: digest.clone(),
        },
    );
    let environment = tool_environment(&tools, Some(&module_root))?;
    let report_path = temporary.path().join("toolset.json");
    write_json_report(
        &report_path,
        &ToolchainReport {
            schema_version: 1,
            profile: "quality".to_owned(),
            platform: "windows-x64".to_owned(),
            lock_sha256: digest,
            tools,
            environment,
        },
    )?;

    let toolset = load_toolset(&report_path)?;
    let reported_modules = toolset
        .environment()
        .get("PSModulePath")
        .context("tool report omitted PSModulePath")?;
    ensure!(
        Path::new(reported_modules) == module_root,
        "tool report changed the module path"
    );
    ensure_regular_win32_path(Path::new(reported_modules))?;

    let asset = ToolAsset {
        url: "https://example.invalid/module.zip".to_owned(),
        sha256: "0".repeat(64),
        archive: "zip".to_owned(),
        executable: "PSScriptAnalyzer.psd1".to_owned(),
        probe_arguments: Vec::new(),
        version_regex: r"1\.25\.0".to_owned(),
    };
    let mut runner = ModuleProbeRunner {
        manifest: manifest_path,
    };
    let probe_manifest = runner.manifest.clone();
    probe::module(
        &mut runner,
        &toolset.report.tools,
        &asset,
        "1.25.0",
        &probe_manifest,
    )?;
    Ok(())
}

fn ensure_regular_win32_path(path: &Path) -> Result<()> {
    ensure!(
        !path.to_string_lossy().starts_with(r"\\?\"),
        "path uses the verbatim Win32 prefix"
    );
    Ok(())
}
