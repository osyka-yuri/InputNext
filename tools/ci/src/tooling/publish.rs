//! Publish verified tool directories with bounded recovery from transient Windows file locks.

use std::fs;
use std::io;
use std::path::Path;
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use tempfile::TempDir;

const WINDOWS_RENAME_RETRY_DELAYS: [Duration; 5] = [
    Duration::from_millis(50),
    Duration::from_millis(100),
    Duration::from_millis(200),
    Duration::from_millis(400),
    Duration::from_millis(800),
];

pub(super) fn publish_verified_directory(
    staging: &TempDir,
    destination: &Path,
    install_root: &Path,
) -> Result<()> {
    let install_root =
        dunce::canonicalize(install_root).context("could not resolve tool install root")?;
    ensure!(
        destination.starts_with(&install_root),
        "tool destination escaped install root"
    );
    let parent = destination
        .parent()
        .context("tool destination has no parent")?;
    fs::create_dir_all(parent).with_context(|| {
        format!(
            "could not create tool destination parent '{}'",
            parent.display()
        )
    })?;
    let parent = dunce::canonicalize(parent).with_context(|| {
        format!(
            "could not resolve tool destination parent '{}'",
            parent.display()
        )
    })?;
    ensure!(
        parent.starts_with(&install_root),
        "tool destination parent escaped install root"
    );

    match fs::symlink_metadata(destination) {
        Ok(metadata) => {
            ensure!(
                !metadata.file_type().is_symlink(),
                "existing tool destination '{}' must not be a symbolic link",
                destination.display()
            );
            ensure!(
                metadata.is_dir(),
                "existing tool destination '{}' is not a directory",
                destination.display()
            );
            fs::remove_dir_all(destination).with_context(|| {
                format!(
                    "could not replace previous tool installation at '{}'",
                    destination.display()
                )
            })?;
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(error).with_context(|| {
                format!(
                    "could not inspect previous tool installation at '{}'",
                    destination.display()
                )
            });
        }
    }

    let retry_delays: &[Duration] = if cfg!(windows) {
        &WINDOWS_RENAME_RETRY_DELAYS
    } else {
        &[]
    };
    retry_permission_denied(
        || fs::rename(staging.path(), destination),
        retry_delays,
        thread::sleep,
    )
    .with_context(|| {
        format!(
            "could not publish verified tool installation from '{}' to '{}'",
            staging.path().display(),
            destination.display()
        )
    })?;
    Ok(())
}

fn retry_permission_denied<T>(
    mut operation: impl FnMut() -> io::Result<T>,
    retry_delays: &[Duration],
    mut wait: impl FnMut(Duration),
) -> io::Result<T> {
    for delay in retry_delays {
        match operation() {
            Ok(result) => return Ok(result),
            Err(error) if error.kind() == io::ErrorKind::PermissionDenied => wait(*delay),
            Err(error) => return Err(error),
        }
    }
    operation()
}

#[cfg(test)]
mod tests {
    use std::io;
    use std::time::Duration;

    use super::retry_permission_denied;

    const RETRIES: [Duration; 3] = [
        Duration::from_millis(10),
        Duration::from_millis(20),
        Duration::from_millis(30),
    ];

    #[test]
    fn retry_permission_denied_recovers_after_transient_error() {
        let mut attempts = 0;
        let mut waits = Vec::new();
        let result = retry_permission_denied(
            || {
                attempts += 1;
                if attempts < 3 {
                    Err(io::Error::from(io::ErrorKind::PermissionDenied))
                } else {
                    Ok("published")
                }
            },
            &RETRIES,
            |delay| waits.push(delay),
        );

        assert_eq!(result.ok(), Some("published"));
        assert_eq!(attempts, 3);
        assert_eq!(waits, RETRIES[..2]);
    }

    #[test]
    fn retry_permission_denied_stops_after_bounded_attempts() {
        let mut attempts = 0;
        let mut waits = Vec::new();
        let result = retry_permission_denied(
            || {
                attempts += 1;
                Err::<(), _>(io::Error::from(io::ErrorKind::PermissionDenied))
            },
            &RETRIES,
            |delay| waits.push(delay),
        );

        assert!(matches!(
            result,
            Err(error) if error.kind() == io::ErrorKind::PermissionDenied
        ));
        assert_eq!(attempts, RETRIES.len() + 1);
        assert_eq!(waits, RETRIES);
    }

    #[test]
    fn retry_permission_denied_does_not_retry_other_errors() {
        let mut attempts = 0;
        let mut waits = Vec::new();
        let result = retry_permission_denied(
            || {
                attempts += 1;
                Err::<(), _>(io::Error::from(io::ErrorKind::NotFound))
            },
            &RETRIES,
            |delay| waits.push(delay),
        );

        assert!(matches!(
            result,
            Err(error) if error.kind() == io::ErrorKind::NotFound
        ));
        assert_eq!(attempts, 1);
        assert_eq!(waits, Vec::<Duration>::new());
    }

    #[cfg(windows)]
    #[test]
    fn retries_real_directory_rename_after_staged_file_handle_is_released() -> io::Result<()> {
        use std::fs::{self, OpenOptions};
        use std::os::windows::fs::OpenOptionsExt;

        let root = tempfile::tempdir()?;
        let staging = tempfile::tempdir_in(root.path())?;
        let staged_file = staging.path().join("tool.exe");
        fs::write(&staged_file, b"verified tool bytes")?;
        let mut locked_file = Some(
            OpenOptions::new()
                .read(true)
                .share_mode(0)
                .open(&staged_file)?,
        );

        let destination = root.path().join("installed");
        let retry_delays = [Duration::from_millis(1)];
        let mut attempts = 0;
        let mut first_error = None;
        let mut waits = Vec::new();
        let result = retry_permission_denied(
            || {
                attempts += 1;
                let result = fs::rename(staging.path(), &destination);
                if let Err(error) = &result {
                    first_error = Some((error.kind(), error.raw_os_error()));
                }
                result
            },
            &retry_delays,
            |delay| {
                waits.push(delay);
                drop(locked_file.take());
            },
        );

        result?;
        assert_eq!(
            first_error,
            Some((io::ErrorKind::PermissionDenied, Some(5))),
            "the held staged file should reproduce Windows ERROR_ACCESS_DENIED"
        );
        assert_eq!(attempts, 2);
        assert_eq!(waits, retry_delays);
        assert!(!staging.path().exists(), "the staging path should be moved");
        assert_eq!(
            fs::read(destination.join("tool.exe"))?,
            b"verified tool bytes"
        );
        Ok(())
    }
}
