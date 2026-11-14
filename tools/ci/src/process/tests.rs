use std::collections::BTreeMap;
use std::io;
use std::mem::DropGuard;
use std::process::ExitStatus;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Context, Result};
use process_wrap::std::ChildWrapper;

use super::{
    ChildGuard, CommandOutput, CommandSpec, Runner, reap_child, run_checked, terminate_child,
};

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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CleanupCall {
    Kill,
    Wait,
}

#[derive(Debug)]
struct TestChild {
    calls: Arc<Mutex<Vec<CleanupCall>>>,
    fail_first_wait: bool,
}

impl ChildWrapper for TestChild {
    fn inner(&self) -> &dyn ChildWrapper {
        self
    }

    fn inner_mut(&mut self) -> &mut dyn ChildWrapper {
        self
    }

    fn into_inner(self: Box<Self>) -> Box<dyn ChildWrapper> {
        self
    }

    fn start_kill(&mut self) -> io::Result<()> {
        self.calls
            .lock()
            .map_err(|_| io::Error::other("cleanup log was poisoned"))?
            .push(CleanupCall::Kill);
        Ok(())
    }

    fn wait(&mut self) -> io::Result<ExitStatus> {
        self.calls
            .lock()
            .map_err(|_| io::Error::other("cleanup log was poisoned"))?
            .push(CleanupCall::Wait);
        if std::mem::take(&mut self.fail_first_wait) {
            return Err(io::Error::other("fixture wait failed"));
        }
        Ok(ExitStatus::default())
    }
}

#[test]
fn process_cleanup_dismisses_the_guard_only_after_success() -> Result<()> {
    use CleanupCall::{Kill, Wait};

    for fail_first_wait in [false, true] {
        let calls = Arc::new(Mutex::new(Vec::new()));
        let child: Box<dyn ChildWrapper> = Box::new(TestChild {
            calls: Arc::clone(&calls),
            fail_first_wait,
        });
        let child: ChildGuard = DropGuard::new(child, reap_child);

        let result = terminate_child(child);
        let calls = calls
            .lock()
            .map_err(|_| io::Error::other("cleanup log was poisoned"))?;
        if fail_first_wait {
            let error = result.err().context("expected process cleanup to fail")?;
            assert!(error.to_string().contains("could not wait"));
            assert_eq!(*calls, [Kill, Wait, Kill, Wait]);
        } else {
            assert!(result?.success());
            assert_eq!(*calls, [Kill, Wait]);
        }
    }
    Ok(())
}

#[cfg(unix)]
#[test]
fn missing_process_group_is_benign_but_other_kill_errors_are_retained() {
    use super::process_tree_is_absent;

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
