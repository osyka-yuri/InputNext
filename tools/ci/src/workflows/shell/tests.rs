use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

use super::check;
use crate::process::{CommandOutput, CommandSpec, Runner};

#[derive(Default)]
struct RecordingRunner {
    code: i32,
    scripts: Vec<(PathBuf, String)>,
}

impl Runner for RecordingRunner {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        assert_eq!(spec.args[..3], ["--shell=bash", "--format=gcc", "--"]);
        for path in &spec.args[3..] {
            self.scripts
                .push((PathBuf::from(path), fs::read_to_string(path)?));
        }
        Ok(CommandOutput {
            code: Some(self.code),
            stdout: Vec::new(),
            stderr: b"SC2086: Double quote to prevent globbing".to_vec(),
        })
    }
}

fn fixture() -> Result<tempfile::TempDir> {
    let directory = tempfile::tempdir()?;
    fs::create_dir_all(directory.path().join(".github/actions/example"))?;
    fs::create_dir(directory.path().join("tools"))?;
    Ok(directory)
}

#[test]
fn checks_standalone_and_composite_bash_without_interpreting_github_expressions() -> Result<()> {
    let directory = fixture()?;
    let standalone = directory.path().join("tools/example.sh");
    fs::write(&standalone, "#!/usr/bin/env bash\nprintf '%s' \"$VALUE\"\n")?;
    fs::write(
        directory.path().join(".github/actions/example/action.yml"),
        r"
runs:
  using: composite
  steps:
    - shell: bash --noprofile
      run: echo '${{ inputs.value }}'
    - shell: pwsh
      run: Write-Output $env:VALUE
",
    )?;
    let shellcheck = directory.path().join("shellcheck");
    let mut runner = RecordingRunner::default();
    check(directory.path(), &shellcheck, &mut runner)?;
    assert_eq!(runner.scripts.len(), 2);
    assert!(runner.scripts.iter().any(|(path, _)| *path == standalone));
    let extracted = runner
        .scripts
        .iter()
        .find(|(path, _)| *path != standalone)
        .context("composite script omitted")?;
    assert_eq!(extracted.1, "echo '__GITHUB_EXPRESSION__'");
    assert!(
        !extracted.0.exists(),
        "temporary composite script was retained"
    );
    assert!(standalone.exists(), "repository source was removed");
    Ok(())
}

#[test]
fn shellcheck_findings_fail_the_gate_and_retain_their_diagnostics() -> Result<()> {
    let directory = fixture()?;
    fs::write(directory.path().join("tools/bad.sh"), "echo $VALUE\n")?;
    let mut runner = RecordingRunner {
        code: 1,
        ..RecordingRunner::default()
    };
    let error = check(directory.path(), Path::new("shellcheck"), &mut runner)
        .err()
        .context("invalid Bash passed")?;
    let message = format!("{error:#}");
    assert!(message.contains("repository Bash checks failed"));
    assert!(message.contains("SC2086"));
    Ok(())
}
