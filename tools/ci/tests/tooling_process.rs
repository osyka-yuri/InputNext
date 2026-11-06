use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io::{self, Write};
use std::process::{Child, Command};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use inputnext_ci::process::{CommandSpec, Runner, SystemRunner, run_checked};

const CHILD_MODE: &str = "INPUTNEXT_PROCESS_TEST_MODE";
const MARKER_PATH: &str = "INPUTNEXT_PROCESS_TEST_MARKER";

#[test]
#[ignore = "spawned by the process runner integration tests"]
fn process_child_probe() -> Result<()> {
    match std::env::var(CHILD_MODE).as_deref() {
        Ok("output") => {
            println!("child-stdout-marker");
            eprintln!("child-stderr-marker");
        }
        Ok("failure") => bail!("child-failure-marker"),
        Ok("sleep") => thread::sleep(Duration::from_secs(10)),
        Ok("overflow") => {
            let mut stdout = io::stdout().lock();
            let _ = stdout.write_all(&vec![b'x'; 8192]);
            let _ = stdout.flush();
        }
        Ok("descendant") => {
            let mut child = spawn_descendant()?;
            let _ = child.try_wait()?;
            thread::sleep(Duration::from_secs(10));
        }
        Ok("parent-exits") => {
            let mut child = spawn_descendant()?;
            let _ = child.try_wait()?;
        }
        _ => {}
    }
    Ok(())
}

#[test]
#[ignore = "spawned by the process runner integration tests"]
fn process_descendant_probe() -> Result<()> {
    thread::sleep(Duration::from_secs(10));
    let marker = std::env::var_os(MARKER_PATH).context("marker path is missing")?;
    fs::write(marker, "descendant survived")?;
    Ok(())
}

#[test]
fn captures_stdout_stderr_and_nonzero_status_without_rejecting_it() -> Result<()> {
    let mut runner = SystemRunner;
    let output = runner.run(&spec("output", Duration::from_secs(10), 4096, None)?)?;
    assert_eq!(output.code, Some(0));
    assert!(String::from_utf8_lossy(&output.stdout).contains("child-stdout-marker"));
    assert!(String::from_utf8_lossy(&output.stderr).contains("child-stderr-marker"));

    let output = runner.run(&spec("failure", Duration::from_secs(10), 4096, None)?)?;
    assert_ne!(output.code, Some(0));
    assert!(String::from_utf8_lossy(&output.stderr).contains("child-failure-marker"));
    Ok(())
}

#[test]
fn fast_children_finish_process_tree_cleanup() -> Result<()> {
    let mut runner = SystemRunner;
    for _ in 0..32 {
        let output = runner.run(&spec("fast", Duration::from_secs(2), 4096, None)?)?;
        assert_eq!(output.code, Some(0));
    }
    Ok(())
}

#[test]
fn run_checked_rejects_nonzero_status() -> Result<()> {
    let mut runner = SystemRunner;
    let error = failure(
        run_checked(
            &mut runner,
            &spec("failure", Duration::from_secs(10), 4096, None)?,
        ),
        "nonzero status",
    )?;
    assert!(error.to_string().contains("exited with status"));
    assert!(error.to_string().contains("child-failure-marker"));
    Ok(())
}

#[test]
fn timeout_and_capture_overflow_fail_with_bounded_output() -> Result<()> {
    let mut runner = SystemRunner;
    let timeout = failure(
        runner.run(&spec("sleep", Duration::from_millis(100), 4096, None)?),
        "timeout",
    )?;
    assert!(timeout.to_string().contains("timed out"));

    let overflow = failure(
        runner.run(&spec("overflow", Duration::from_secs(10), 256, None)?),
        "capture limit",
    )?;
    assert!(overflow.to_string().contains("output limit"));
    Ok(())
}

#[test]
fn timeout_kills_descendants_that_inherit_the_output_pipes() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let marker = directory.path().join("descendant-survived");
    let mut runner = SystemRunner;
    let timeout = failure(
        runner.run(&spec(
            "descendant",
            Duration::from_millis(150),
            4096,
            Some(marker.as_os_str().to_owned()),
        )?),
        "timeout",
    )?;
    assert!(timeout.to_string().contains("timed out"));
    thread::sleep(Duration::from_secs(1));
    assert!(!marker.exists(), "descendant process survived tree cleanup");
    Ok(())
}

#[test]
fn timeout_kills_descendant_when_root_exits_before_timeout() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let marker = directory.path().join("descendant-survived");
    let mut runner = SystemRunner;
    let started = Instant::now();
    let timeout = failure(
        runner.run(&spec(
            "parent-exits",
            Duration::from_millis(150),
            4096,
            Some(marker.as_os_str().to_owned()),
        )?),
        "timeout with inherited pipe",
    )?;
    assert!(timeout.to_string().contains("timed out"));
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "cleanup was not bounded"
    );
    thread::sleep(Duration::from_millis(100));
    assert!(!marker.exists(), "descendant process survived tree cleanup");
    Ok(())
}

fn failure<T>(result: Result<T>, description: &str) -> Result<anyhow::Error> {
    match result {
        Ok(_) => bail!("expected {description} failure"),
        Err(error) => Ok(error),
    }
}

fn spawn_descendant() -> Result<Child> {
    let executable = std::env::current_exe().context("could not resolve test executable")?;
    let marker = std::env::var_os(MARKER_PATH).context("marker path is missing")?;
    Command::new(executable)
        .args([
            OsStr::new("--exact"),
            OsStr::new("process_descendant_probe"),
            OsStr::new("--ignored"),
            OsStr::new("--nocapture"),
        ])
        .env(MARKER_PATH, marker)
        .spawn()
        .context("could not start descendant process")
}

fn spec(
    mode: &str,
    timeout: Duration,
    output_limit: usize,
    marker: Option<OsString>,
) -> Result<CommandSpec> {
    let mut env = BTreeMap::new();
    env.insert(CHILD_MODE.into(), mode.into());
    if let Some(marker) = marker {
        env.insert(MARKER_PATH.into(), marker);
    }
    Ok(CommandSpec {
        program: std::env::current_exe().context("could not resolve test executable")?,
        args: vec![
            "--exact".into(),
            "process_child_probe".into(),
            "--ignored".into(),
            "--nocapture".into(),
        ],
        cwd: std::env::current_dir()
            .context("could not read current directory")?
            .canonicalize()
            .context("could not resolve current directory")?,
        env,
        timeout,
        output_limit,
    })
}
