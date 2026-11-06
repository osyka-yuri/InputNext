//! Bounded child-process execution shared by CI commands.

#[cfg(test)]
mod tests;

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::io::{self, Read};
use std::path::PathBuf;
use std::process::{Command, ExitStatus, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail, ensure};
#[cfg(windows)]
use process_wrap::std::JobObject;
#[cfg(unix)]
use process_wrap::std::ProcessGroup;
use process_wrap::std::{ChildWrapper, CommandWrap};

const MAX_OUTPUT_LIMIT: usize = 64 * 1024 * 1024;
const POLL_INTERVAL: Duration = Duration::from_millis(10);

/// An absolute command invocation with bounded execution and capture.
#[derive(Clone, Debug)]
pub struct CommandSpec {
    pub program: PathBuf,
    pub args: Vec<OsString>,
    pub cwd: PathBuf,
    pub env: BTreeMap<OsString, OsString>,
    pub timeout: Duration,
    /// Maximum retained bytes per output stream.
    pub output_limit: usize,
}

/// The bounded output from a completed child process.
#[derive(Debug)]
pub struct CommandOutput {
    pub code: Option<i32>,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}

impl CommandOutput {
    pub(crate) fn diagnostics(&self) -> String {
        format!(
            "stdout:\n{}\nstderr:\n{}",
            diagnostic_tail(&self.stdout),
            diagnostic_tail(&self.stderr)
        )
    }
}

fn diagnostic_tail(bytes: &[u8]) -> String {
    const MAX_DIAGNOSTIC_BYTES: usize = 8 * 1024;
    let start = bytes.len().saturating_sub(MAX_DIAGNOSTIC_BYTES);
    let text = String::from_utf8_lossy(&bytes[start..]);
    if start == 0 {
        text.into_owned()
    } else {
        format!("[{start} earlier bytes omitted]\n{text}")
    }
}

/// Runs child processes, retaining nonzero exit statuses as command outcomes.
pub trait Runner {
    /// Runs a child process with bounded output and execution time.
    ///
    /// # Errors
    ///
    /// Returns an error when validation, process creation, output capture, or cleanup fails.
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput>;
}

/// The operating-system process runner.
#[derive(Debug, Default)]
pub struct SystemRunner;

impl Runner for SystemRunner {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        validate_spec(spec)?;
        let deadline = Instant::now()
            .checked_add(spec.timeout)
            .context("command timeout is too large")?;
        let mut command = Command::new(&spec.program);
        command
            .args(&spec.args)
            .current_dir(&spec.cwd)
            .envs(&spec.env)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut command = CommandWrap::from(command);
        configure_process_tree(&mut command);
        let child = command
            .spawn()
            .with_context(|| format!("could not start {}", spec.program.display()))?;
        let mut child = ChildGuard::new(child);
        let mut captures = Captures::start(&mut child, spec.output_limit)?;
        wait_for_output(&mut child, spec, deadline, &mut captures)
    }
}

fn wait_for_output(
    child: &mut ChildGuard,
    spec: &CommandSpec,
    deadline: Instant,
    captures: &mut Captures,
) -> Result<CommandOutput> {
    let mut status = None;
    loop {
        if captures.exceeded.load(Ordering::Relaxed) {
            captures
                .terminate_and_join(child)
                .context("could not clean up after child output exceeded its limit")?;
            bail!(
                "{} exceeded the {} byte output limit",
                spec.program.display(),
                spec.output_limit
            );
        }
        if status.is_none() {
            // Poll only the root process. On Windows, JobObjectChild::try_wait consumes the
            // completion-port notification that its later wait() needs to confirm tree exit.
            match child.child.inner_mut().try_wait() {
                Ok(exit_status) => status = exit_status,
                Err(error) => {
                    let original =
                        anyhow::Error::new(error).context("could not inspect child status");
                    return Err(captures.error_with_cleanup(child, original));
                }
            }
        }
        if let Err(error) = captures.collect_finished() {
            return Err(captures.error_with_cleanup(child, error));
        }
        if let Some(exit_status) = status.filter(|_| captures.is_complete()) {
            let (stdout, stderr) = captures.take_output()?;
            let final_status = child
                .terminate()
                .context("could not clean up the completed child process group")?;
            debug_assert_eq!(exit_status, final_status);
            return Ok(output(final_status, stdout, stderr));
        }
        if Instant::now() >= deadline {
            let status_description =
                status.map_or_else(|| "running".to_owned(), |status| format!("{status:?}"));
            let stdout_pending = captures.stdout_bytes.is_none();
            let stderr_pending = captures.stderr_bytes.is_none();
            captures
                .terminate_and_join(child)
                .context("could not clean up after command timeout")?;
            bail!(
                "{} timed out after {:?} (root status: {}; stdout pending: {}; stderr pending: {})",
                spec.program.display(),
                spec.timeout,
                status_description,
                stdout_pending,
                stderr_pending
            );
        }
        thread::sleep(POLL_INTERVAL);
    }
}

struct Captures {
    exceeded: Arc<AtomicBool>,
    stdout_reader: Option<JoinHandle<io::Result<Vec<u8>>>>,
    stderr_reader: Option<JoinHandle<io::Result<Vec<u8>>>>,
    stdout_bytes: Option<Vec<u8>>,
    stderr_bytes: Option<Vec<u8>>,
}

impl Captures {
    fn start(child: &mut ChildGuard, limit: usize) -> Result<Self> {
        let stdout = child
            .child
            .stdout()
            .take()
            .context("child stdout pipe was not created")?;
        let stderr = child
            .child
            .stderr()
            .take()
            .context("child stderr pipe was not created")?;
        let exceeded = Arc::new(AtomicBool::new(false));
        let stdout_reader = Some(
            spawn_capture(stdout, limit, Arc::clone(&exceeded))
                .context("could not start stdout capture")?,
        );
        let stderr_reader = match spawn_capture(stderr, limit, Arc::clone(&exceeded)) {
            Ok(reader) => Some(reader),
            Err(error) => {
                child
                    .terminate()
                    .context("could not clean up after stderr capture setup failed")?;
                if let Some(reader) = stdout_reader {
                    let _ = join_capture(reader);
                }
                return Err(error).context("could not start stderr capture");
            }
        };
        Ok(Self {
            exceeded,
            stdout_reader,
            stderr_reader,
            stdout_bytes: None,
            stderr_bytes: None,
        })
    }

    fn collect_finished(&mut self) -> Result<()> {
        if self
            .stdout_reader
            .as_ref()
            .is_some_and(JoinHandle::is_finished)
        {
            self.stdout_bytes = Some(join_capture(
                self.stdout_reader
                    .take()
                    .context("stdout reader was already joined")?,
            )?);
        }
        if self
            .stderr_reader
            .as_ref()
            .is_some_and(JoinHandle::is_finished)
        {
            self.stderr_bytes = Some(join_capture(
                self.stderr_reader
                    .take()
                    .context("stderr reader was already joined")?,
            )?);
        }
        Ok(())
    }

    fn is_complete(&self) -> bool {
        self.stdout_bytes.is_some() && self.stderr_bytes.is_some()
    }

    fn take_output(&mut self) -> Result<(Vec<u8>, Vec<u8>)> {
        let stdout = self
            .stdout_bytes
            .take()
            .context("child stdout capture disappeared")?;
        let stderr = self
            .stderr_bytes
            .take()
            .context("child stderr capture disappeared")?;
        Ok((stdout, stderr))
    }

    fn terminate_and_join(&mut self, child: &mut ChildGuard) -> Result<()> {
        child.terminate()?;
        let mut first_error = None;
        for reader in [self.stdout_reader.take(), self.stderr_reader.take()]
            .into_iter()
            .flatten()
        {
            if let Err(error) = join_capture(reader) {
                first_error.get_or_insert(error);
            }
        }
        if let Some(error) = first_error {
            return Err(error).context("could not collect output after process cleanup");
        }
        Ok(())
    }

    fn error_with_cleanup(
        &mut self,
        child: &mut ChildGuard,
        error: anyhow::Error,
    ) -> anyhow::Error {
        match self.terminate_and_join(child) {
            Ok(()) => error,
            Err(cleanup) => error.context(format!("process cleanup also failed: {cleanup:#}")),
        }
    }
}

/// Runs a command and rejects missing or unsuccessful exit statuses.
///
/// # Errors
///
/// Returns an error if the runner fails or the command exits unsuccessfully.
pub fn run_checked(runner: &mut dyn Runner, spec: &CommandSpec) -> Result<CommandOutput> {
    let output = runner.run(spec)?;
    match output.code {
        Some(0) => Ok(output),
        Some(code) => bail!(
            "{} exited with status {code}\n{}",
            spec.program.display(),
            output.diagnostics()
        ),
        None => bail!(
            "{} did not return an exit status\n{}",
            spec.program.display(),
            output.diagnostics()
        ),
    }
}

fn validate_spec(spec: &CommandSpec) -> Result<()> {
    ensure!(
        spec.program.is_absolute(),
        "command executable must be an absolute path"
    );
    ensure!(
        spec.cwd.is_absolute(),
        "command working directory must be absolute"
    );
    ensure!(
        spec.cwd.is_dir(),
        "command working directory does not exist"
    );
    ensure!(!spec.timeout.is_zero(), "command timeout must be positive");
    ensure!(
        spec.output_limit > 0 && spec.output_limit <= MAX_OUTPUT_LIMIT,
        "command output limit must be between 1 byte and {MAX_OUTPUT_LIMIT} bytes"
    );
    Ok(())
}

fn spawn_capture<R: Read + Send + 'static>(
    mut pipe: R,
    limit: usize,
    exceeded: Arc<AtomicBool>,
) -> io::Result<JoinHandle<io::Result<Vec<u8>>>> {
    thread::Builder::new().spawn(move || {
        let mut retained = Vec::with_capacity(limit.min(16 * 1024));
        let mut chunk = [0_u8; 8192];
        loop {
            let count = pipe.read(&mut chunk)?;
            if count == 0 {
                return Ok(retained);
            }

            let available = limit.saturating_sub(retained.len());
            let keep = available.min(count);
            retained.extend_from_slice(&chunk[..keep]);
            if keep < count {
                exceeded.store(true, Ordering::Relaxed);
            }
        }
    })
}

fn join_capture(reader: JoinHandle<io::Result<Vec<u8>>>) -> Result<Vec<u8>> {
    reader
        .join()
        .map_err(|_| anyhow::anyhow!("child output reader panicked"))?
        .context("could not read child output")
}

fn output(status: ExitStatus, stdout: Vec<u8>, stderr: Vec<u8>) -> CommandOutput {
    CommandOutput {
        code: status.code(),
        stdout,
        stderr,
    }
}

#[cfg(windows)]
fn configure_process_tree(command: &mut CommandWrap) {
    command.wrap(JobObject);
}

#[cfg(unix)]
fn configure_process_tree(command: &mut CommandWrap) {
    command.wrap(ProcessGroup::leader());
}

#[cfg(not(any(unix, windows)))]
fn configure_process_tree(_command: &mut CommandWrap) {}

struct ChildGuard {
    child: Box<dyn ChildWrapper>,
    finished: bool,
}

impl ChildGuard {
    fn new(child: Box<dyn ChildWrapper>) -> Self {
        Self {
            child,
            finished: false,
        }
    }

    fn terminate(&mut self) -> Result<ExitStatus> {
        match self.child.start_kill() {
            Ok(()) => {}
            Err(error) if process_tree_is_absent(&error) => {}
            Err(error) => return Err(error).context("could not terminate child process tree"),
        }
        let status = self
            .child
            .wait()
            .context("could not wait for child process tree cleanup")?;
        self.finished = true;
        Ok(status)
    }
}

fn process_tree_is_absent(error: &io::Error) -> bool {
    #[cfg(unix)]
    if error.raw_os_error() == Some(libc::ESRCH) {
        return true;
    }
    error.kind() == io::ErrorKind::NotFound
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        if !self.finished {
            let _ = self.child.start_kill();
            let _ = self.child.wait();
            self.finished = true;
        }
    }
}
