use std::collections::BTreeMap;
use std::env;
use std::fs::{self, File};
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, bail, ensure};
use sha2::{Digest, Sha256};
use tempfile::NamedTempFile;

use crate::curl;
use crate::process::{CommandSpec, Runner, run_checked};

use super::archive::MAX_ARCHIVE_BYTES;

const DOWNLOAD_TIMEOUT: Duration = Duration::from_mins(30);
const DOWNLOAD_OUTPUT_LIMIT: usize = 1024 * 1024;

pub(super) fn verified_archive(
    runner: &mut dyn Runner,
    cache_root: &Path,
    url: &str,
    expected_sha256: &str,
    format: &str,
) -> Result<PathBuf> {
    validate_https_url(url)?;
    validate_sha256(expected_sha256)?;
    if format != "file" {
        super::archive::ArchiveFormat::parse(format)?;
    }

    let cache_root = prepare_directory(cache_root)?;
    let cached = cache_root.join(format!("{expected_sha256}.archive"));
    if let Some(path) = valid_cached_file(&cached, expected_sha256)? {
        return Ok(path);
    }

    let temporary = NamedTempFile::new_in(&cache_root)
        .context("could not create private archive download file")?;
    let temporary_path = temporary.into_temp_path();

    let curl = curl::executable()?;
    let current = env::current_dir().context("could not read current working directory")?;
    let cwd =
        dunce::canonicalize(&current).context("could not resolve current working directory")?;
    let spec = CommandSpec {
        program: curl,
        args: vec![
            "--fail".into(),
            "--location".into(),
            "--silent".into(),
            "--show-error".into(),
            "--max-redirs".into(),
            "5".into(),
            "--proto".into(),
            "=https".into(),
            "--proto-redir".into(),
            "=https".into(),
            "--max-filesize".into(),
            MAX_ARCHIVE_BYTES.to_string().into(),
            "--output".into(),
            temporary_path.as_os_str().to_owned(),
            url.into(),
        ],
        cwd,
        env: BTreeMap::new(),
        timeout: DOWNLOAD_TIMEOUT,
        output_limit: DOWNLOAD_OUTPUT_LIMIT,
    };

    if let Err(error) = run_checked(runner, &spec) {
        let _ = fs::remove_file(&temporary_path);
        return Err(error).context("locked tool download failed");
    }

    let metadata = fs::symlink_metadata(&temporary_path)
        .context("download did not produce a regular archive file")?;
    ensure!(
        metadata.is_file(),
        "download did not produce a regular archive file"
    );
    ensure!(
        metadata.len() <= MAX_ARCHIVE_BYTES,
        "downloaded archive exceeds the compressed size limit"
    );
    let actual_sha256 = hash_file(&temporary_path)?;
    ensure!(
        actual_sha256 == expected_sha256,
        "downloaded archive SHA-256 did not match the reviewed lock"
    );

    match temporary_path.persist_noclobber(&cached) {
        Ok(()) => {}
        Err(error) => {
            if let Some(path) = valid_cached_file(&cached, expected_sha256)? {
                drop(error);
                return Ok(path);
            }
            return Err(error.error).context("could not store verified archive in the cache");
        }
    }
    dunce::canonicalize(&cached).context("could not resolve cached archive path")
}

pub(super) fn hash_file(path: &Path) -> Result<String> {
    let file = File::open(path).with_context(|| format!("could not open {}", path.display()))?;
    let mut reader = BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let count = reader.read(&mut buffer).context("could not hash file")?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(encode_sha256(hasher.finalize().as_ref()))
}

fn encode_sha256(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

pub(super) fn validate_sha256(value: &str) -> Result<()> {
    ensure!(
        value.len() == 64,
        "locked SHA-256 must contain 64 hexadecimal characters"
    );
    ensure!(
        value.bytes().all(|byte| byte.is_ascii_hexdigit()),
        "locked SHA-256 contains a non-hexadecimal character"
    );
    Ok(())
}

fn valid_cached_file(path: &Path, expected_sha256: &str) -> Result<Option<PathBuf>> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error).context("could not inspect cached archive"),
    };
    ensure!(
        !metadata.file_type().is_symlink(),
        "cached archive must not be a symbolic link"
    );
    ensure!(
        metadata.is_file(),
        "cached archive path is not a regular file"
    );
    if hash_file(path)? == expected_sha256 {
        return Ok(Some(
            dunce::canonicalize(path).context("could not resolve cached archive path")?,
        ));
    }
    fs::remove_file(path).context("could not remove corrupted cached archive")?;
    Ok(None)
}

fn prepare_directory(path: &Path) -> Result<PathBuf> {
    fs::create_dir_all(path).with_context(|| format!("could not create {}", path.display()))?;
    let resolved = dunce::canonicalize(path)
        .with_context(|| format!("could not resolve {}", path.display()))?;
    let metadata = fs::symlink_metadata(&resolved).context("could not inspect directory")?;
    ensure!(metadata.is_dir(), "cache path is not a directory");
    Ok(resolved)
}

fn validate_https_url(url: &str) -> Result<()> {
    let Some(authority) = url.strip_prefix("https://") else {
        bail!("locked tool URL must use HTTPS");
    };
    let host = authority.split(['/', '?', '#']).next().unwrap_or_default();
    ensure!(!host.is_empty(), "locked HTTPS URL has no host");
    ensure!(
        !host.contains('@'),
        "locked HTTPS URL must not contain user information"
    );
    ensure!(
        !url.chars().any(char::is_whitespace),
        "locked HTTPS URL contains whitespace"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{hash_file, verified_archive};
    use crate::process::{CommandOutput, CommandSpec, Runner};
    use anyhow::{Context, Result};
    use std::fs;
    use std::path::Path;

    const ARCHIVE: &[u8] = b"reviewed archive fixture";

    struct Download {
        bytes: &'static [u8],
        calls: usize,
    }

    impl Runner for Download {
        fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
            let output = spec
                .args
                .windows(2)
                .find(|pair| pair[0] == "--output")
                .context("download command must select its output file")?;
            fs::write(Path::new(&output[1]), self.bytes)?;
            self.calls += 1;
            Ok(CommandOutput {
                code: Some(0),
                stdout: Vec::new(),
                stderr: Vec::new(),
            })
        }
    }

    #[test]
    fn cached_bytes_are_rehashed_and_corruption_is_replaced() -> Result<()> {
        let temporary = tempfile::tempdir()?;
        let source = temporary.path().join("source");
        fs::write(&source, ARCHIVE)?;
        let digest = hash_file(&source)?;
        let cache = temporary.path().join("cache");
        fs::create_dir(&cache)?;
        let cached = cache.join(format!("{digest}.archive"));
        fs::copy(&source, &cached)?;
        let mut runner = Download {
            bytes: ARCHIVE,
            calls: 0,
        };
        let admitted = verified_archive(
            &mut runner,
            &cache,
            "https://example.invalid/tool",
            &digest,
            "file",
        )?;
        assert_eq!(hash_file(&admitted)?, digest);
        assert_eq!(runner.calls, 0);

        fs::write(&cached, b"changed after cache admission")?;
        let repaired = verified_archive(
            &mut runner,
            &cache,
            "https://example.invalid/tool",
            &digest,
            "file",
        )?;
        assert_eq!(hash_file(&repaired)?, digest);
        assert_eq!(runner.calls, 1);
        Ok(())
    }

    #[test]
    fn downloaded_bytes_with_wrong_hash_are_never_cached() -> Result<()> {
        let temporary = tempfile::tempdir()?;
        let source = temporary.path().join("source");
        fs::write(&source, ARCHIVE)?;
        let digest = hash_file(&source)?;
        let cache = temporary.path().join("cache");
        let mut runner = Download {
            bytes: b"unreviewed bytes",
            calls: 0,
        };
        assert!(
            verified_archive(
                &mut runner,
                &cache,
                "https://example.invalid/tool",
                &digest,
                "file"
            )
            .is_err()
        );
        assert_eq!(runner.calls, 1);
        assert!(!cache.join(format!("{digest}.archive")).exists());
        Ok(())
    }
}
