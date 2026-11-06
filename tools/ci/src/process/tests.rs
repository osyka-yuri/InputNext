use std::collections::BTreeMap;
use std::time::Duration;

use anyhow::{Context, Result};

use super::{CommandOutput, CommandSpec, Runner, run_checked};

struct StubRunner(Option<CommandOutput>);

impl Runner for StubRunner {
    fn run(&mut self, _spec: &CommandSpec) -> Result<CommandOutput> {
        self.0.take().context("fixture output already consumed")
    }
}

fn spec() -> Result<CommandSpec> {
    Ok(CommandSpec {
        program: std::env::current_exe()?,
        args: vec!["argument-marker".into()],
        cwd: std::env::current_dir()?,
        env: BTreeMap::from([("TEST_VALUE".into(), "environment-marker".into())]),
        timeout: Duration::from_secs(1),
        output_limit: 32 * 1024,
    })
}

#[test]
fn rejected_statuses_retain_both_streams_without_arguments_or_environment() -> Result<()> {
    for code in [Some(7), None] {
        let mut runner = StubRunner(Some(CommandOutput {
            code,
            stdout: b"stdout-marker".to_vec(),
            stderr: b"stderr-marker".to_vec(),
        }));
        let error = run_checked(&mut runner, &spec()?)
            .err()
            .context("expected failure")?;
        let message = error.to_string();
        assert!(message.contains(if code.is_some() {
            "exited with status 7"
        } else {
            "did not return an exit status"
        }));
        assert!(message.contains("stdout:\nstdout-marker"));
        assert!(message.contains("stderr:\nstderr-marker"));
        assert!(!message.contains("argument-marker"));
        assert!(!message.contains("environment-marker"));
    }
    Ok(())
}

#[test]
fn failure_diagnostics_include_bounded_stdout_and_stderr_tails() {
    let output = CommandOutput {
        code: Some(1),
        stdout: [b"discarded-prefix".as_slice(), &vec![b'o'; 8192]].concat(),
        stderr: [b"discarded-prefix".as_slice(), &vec![b'e'; 8192]].concat(),
    };
    let message = output.diagnostics();
    assert_eq!(message.matches("[16 earlier bytes omitted]").count(), 2);
    assert!(!message.contains("discarded-prefix"));
    assert!(message.contains(&format!(
        "stdout:\n[16 earlier bytes omitted]\n{}",
        "o".repeat(8192)
    )));
    assert!(message.ends_with(&format!(
        "stderr:\n[16 earlier bytes omitted]\n{}",
        "e".repeat(8192)
    )));
    assert!(message.len() < 17 * 1024);
}

#[test]
fn success_preserves_output_and_invalid_utf8_is_displayable() -> Result<()> {
    let mut runner = StubRunner(Some(CommandOutput {
        code: Some(0),
        stdout: b"success".to_vec(),
        stderr: vec![b'a', 0xff, b'b'],
    }));
    let output = run_checked(&mut runner, &spec()?)?;
    assert_eq!(output.stdout, b"success");
    assert_eq!(output.stderr, [b'a', 0xff, b'b']);
    assert!(output.diagnostics().ends_with("a\u{fffd}b"));
    Ok(())
}

#[cfg(unix)]
#[test]
fn missing_process_group_is_benign_but_other_kill_errors_are_retained() {
    use super::process_tree_is_absent;
    use std::io;

    assert!(process_tree_is_absent(&io::Error::from_raw_os_error(
        libc::ESRCH
    )));
    assert!(!process_tree_is_absent(&io::Error::from_raw_os_error(
        libc::EPERM
    )));
    assert!(!process_tree_is_absent(&io::Error::from_raw_os_error(
        libc::EINVAL
    )));
}
