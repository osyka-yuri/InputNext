//! Resolve the curl executable used for tool and metadata downloads.

use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};

pub(crate) fn executable() -> Result<PathBuf> {
    let path = env::var_os("PATH").context("PATH is not set; curl cannot be located")?;
    find_in_path(&path)
}

fn find_in_path(path: &OsStr) -> Result<PathBuf> {
    for directory in env::split_paths(path) {
        if directory.as_os_str().is_empty() {
            continue;
        }
        for name in executable_names() {
            let candidate = directory.join(name);
            if is_executable_file(&candidate) {
                return dunce::canonicalize(&candidate)
                    .context("could not resolve curl executable");
            }
        }
    }
    bail!("curl executable was not found on PATH")
}

#[cfg(windows)]
fn executable_names() -> &'static [&'static str] {
    &["curl.exe", "curl"]
}

#[cfg(not(windows))]
fn executable_names() -> &'static [&'static str] {
    &["curl"]
}

#[cfg(unix)]
fn is_executable_file(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;

    fs::metadata(path)
        .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

#[cfg(not(unix))]
fn is_executable_file(path: &Path) -> bool {
    fs::metadata(path).is_ok_and(|metadata| metadata.is_file())
}

#[cfg(test)]
mod tests {
    use super::{executable_names, find_in_path};
    use std::{env, fs};

    use anyhow::{Result, ensure};

    #[test]
    fn resolves_a_regular_executable_and_rejects_a_directory() -> Result<()> {
        let temporary = tempfile::tempdir()?;
        let path = env::join_paths([temporary.path()])?;
        let executable = temporary.path().join(executable_names()[0]);
        assert!(find_in_path(&path).is_err());
        fs::create_dir(&executable)?;
        assert!(find_in_path(&path).is_err());
        fs::remove_dir(&executable)?;
        fs::write(&executable, b"controlled curl fixture")?;

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;

            fs::set_permissions(&executable, fs::Permissions::from_mode(0o600))?;
            assert!(find_in_path(&path).is_err());
            fs::set_permissions(&executable, fs::Permissions::from_mode(0o700))?;
        }

        let resolved = find_in_path(&path)?;
        assert_eq!(resolved, dunce::canonicalize(executable)?);
        ensure!(resolved.is_absolute(), "curl path must be absolute");
        #[cfg(windows)]
        assert!(!resolved.to_string_lossy().starts_with(r"\\?\"));
        Ok(())
    }
}
