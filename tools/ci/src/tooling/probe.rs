//! Version probes for executable and PowerShell module assets.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use regex::Regex;
use tempfile::TempDir;

use crate::process::{CommandSpec, Runner, run_checked};

use super::lock::ToolAsset;
use super::report::{ToolEntry, ToolKind};

const PROBE_TIMEOUT: Duration = Duration::from_secs(30);
const PROBE_OUTPUT_LIMIT: usize = 256 * 1024;

pub(super) fn executable(
    runner: &mut dyn Runner,
    name: &str,
    asset: &ToolAsset,
    executable: &Path,
    cwd: &Path,
) -> Result<()> {
    let expected = Regex::new(&asset.version_regex)
        .with_context(|| format!("tool '{name}' has an invalid version probe regex"))?;
    let spec = CommandSpec {
        program: executable.to_path_buf(),
        args: asset.probe_arguments.iter().map(Into::into).collect(),
        cwd: cwd.to_path_buf(),
        env: BTreeMap::new(),
        timeout: PROBE_TIMEOUT,
        output_limit: PROBE_OUTPUT_LIMIT,
    };
    let output = run_checked(runner, &spec)
        .with_context(|| format!("locked tool '{name}' version probe failed"))?;
    let text = normalized_probe_output(&output.stdout, &output.stderr);
    ensure!(
        expected.is_match(&text),
        "tool '{name}' version probe did not match the lock"
    );
    Ok(())
}

pub(super) fn module(
    runner: &mut dyn Runner,
    installed: &BTreeMap<String, ToolEntry>,
    asset: &ToolAsset,
    expected_version: &str,
    manifest_path: &Path,
) -> Result<()> {
    let powershell = installed
        .get("powershell")
        .context("PowerShell must be installed before PowerShell modules")?;
    ensure!(
        powershell.kind == ToolKind::Executable,
        "PowerShell installation is not executable"
    );
    let script = r"
param([string] $ManifestPath, [string] $ExpectedVersion)
$ErrorActionPreference = 'Stop'
$manifest = Test-ModuleManifest -Path $ManifestPath
if ($manifest.Version.ToString() -cne $ExpectedVersion) { throw 'Module version did not match the lock.' }
Write-Output $manifest.Version.ToString()
";
    let temporary = TempDir::new().context("could not create module probe directory")?;
    let script_path = temporary.path().join("verify-module.ps1");
    fs::write(&script_path, script).context("could not write module probe script")?;
    let spec = CommandSpec {
        program: powershell.path.clone(),
        args: vec![
            "-NoLogo".into(),
            "-NoProfile".into(),
            "-NonInteractive".into(),
            "-File".into(),
            script_path.into_os_string(),
            manifest_path.as_os_str().to_owned(),
            expected_version.into(),
        ],
        cwd: temporary.path().to_path_buf(),
        env: BTreeMap::new(),
        timeout: PROBE_TIMEOUT,
        output_limit: PROBE_OUTPUT_LIMIT,
    };
    let output =
        run_checked(runner, &spec).context("PSScriptAnalyzer module manifest probe failed")?;
    let text = normalized_probe_output(&output.stdout, &output.stderr);
    ensure!(
        Regex::new(&asset.version_regex)?.is_match(&text),
        "module version probe did not match the lock"
    );
    Ok(())
}

fn normalized_probe_output(stdout: &[u8], stderr: &[u8]) -> String {
    let stdout = normalized_probe_stream(stdout);
    let stderr = normalized_probe_stream(stderr);
    match (stdout.is_empty(), stderr.is_empty()) {
        (false, false) => format!("{stdout}\n{stderr}"),
        (false, true) => stdout,
        (true, false) => stderr,
        (true, true) => String::new(),
    }
}

fn normalized_probe_stream(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes)
        .replace("\r\n", "\n")
        .trim_end_matches('\n')
        .to_owned()
}

#[cfg(test)]
mod tests {
    use anyhow::Result;
    use regex::Regex;

    use super::normalized_probe_output;

    #[test]
    fn normalizes_crlf_for_locked_executable_and_module_probe_output() -> Result<()> {
        let powershell = Regex::new(r"(?m)^PowerShell (?<version>7\.6\.9)$")?;
        let psscriptanalyzer = Regex::new(r"1\.25\.0")?;
        let powershell_output = b"PowerShell 7.6.9\r\n";
        let module_output = b"1.25.0\r\n";

        assert!(!powershell.is_match(std::str::from_utf8(powershell_output)?));
        assert!(psscriptanalyzer.is_match(std::str::from_utf8(module_output)?));

        let normalized_powershell = normalized_probe_output(powershell_output, b"");
        let normalized_module = normalized_probe_output(module_output, b"");
        assert!(powershell.is_match(&normalized_powershell));
        assert!(psscriptanalyzer.is_match(&normalized_module));
        assert_eq!(normalized_powershell, "PowerShell 7.6.9");
        assert_eq!(normalized_module, "1.25.0");
        assert!(Regex::new(r"1\.25\.0$")?.is_match(&normalized_module));
        Ok(())
    }
}
