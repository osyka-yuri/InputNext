//! Package identity, checksum validation, and isolated artifact staging.

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use regex::Regex;
use semver::Version;
use sha2::{Digest, Sha256};
use tempfile::NamedTempFile;

use super::plan::Architecture;

pub(super) fn project_version(root: &Path) -> Result<String> {
    let cmake = fs::read_to_string(root.join("CMakeLists.txt"))
        .context("could not read root CMakeLists.txt")?;
    let version_regex =
        Regex::new(r"(?m)^project\(InputNext VERSION (?P<version>[0-9]+\.[0-9]+\.[0-9]+)\b")
            .context("could not prepare CMake project version parser")?;
    let version = version_regex
        .captures(&cmake)
        .and_then(|captures| captures.name("version"))
        .map(|value| value.as_str())
        .context("root CMake project version was not found")?;
    Version::parse(version).context("root CMake project version is not SemVer")?;
    Ok(version.to_owned())
}

pub(super) fn stage_release_packages(
    root: &Path,
    architecture: Architecture,
) -> Result<Vec<PathBuf>> {
    let arch = architecture.as_str();
    let version = project_version(root)?;
    let package_name = format!("InputNext-{version}-windows-{arch}");
    let build_directory = root
        .join("build")
        .join(format!("windows-{arch}-production"));
    let package_directory = build_directory.join("packages");
    let sources = [
        package_directory.join(format!("{package_name}.zip")),
        package_directory.join(format!("{package_name}-symbols.zip")),
        package_directory.join(format!("{package_name}-SHA256SUMS.txt")),
    ];
    for source in &sources {
        let metadata = fs::symlink_metadata(source)
            .with_context(|| format!("verified package output is missing: {}", source.display()))?;
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "package output must be a regular file"
        );
    }
    verify_package_checksums(&sources)?;

    let staging = root.join("build/ci/packages");
    fs::create_dir_all(&staging).context("could not create release package staging directory")?;
    let staging =
        dunce::canonicalize(&staging).context("could not resolve package staging directory")?;
    ensure!(
        staging.starts_with(root),
        "release package staging directory escaped the repository"
    );
    sources
        .iter()
        .map(|source| {
            let name = source
                .file_name()
                .context("package source has no file name")?;
            let destination = staging.join(name);
            replace_file(source, &destination)?;
            Ok(destination)
        })
        .collect()
}

fn verify_package_checksums(sources: &[PathBuf; 3]) -> Result<()> {
    let checksum_text = fs::read_to_string(&sources[2])
        .context("could not read architecture package checksum file")?;
    let runtime_name = sources[0]
        .file_name()
        .context("runtime package has no file name")?
        .to_string_lossy();
    let symbols_name = sources[1]
        .file_name()
        .context("symbols package has no file name")?
        .to_string_lossy();
    let expected_names = [runtime_name.as_ref(), symbols_name.as_ref()];
    let lines = checksum_text.lines().collect::<Vec<_>>();
    ensure!(
        lines.len() == expected_names.len(),
        "package checksum file has an unexpected inventory"
    );
    let mut seen = BTreeMap::new();
    for line in lines {
        let (hash, name) = line
            .split_once("  ")
            .context("package checksum line is malformed")?;
        ensure!(
            hash.len() == 64 && hash.bytes().all(|byte| byte.is_ascii_hexdigit()),
            "package checksum has an invalid SHA-256"
        );
        ensure!(
            expected_names.contains(&name),
            "package checksum file names an unexpected asset"
        );
        ensure!(
            seen.insert(name.to_owned(), hash.to_owned()).is_none(),
            "duplicate package checksum"
        );
    }
    for source in &sources[..2] {
        let name = source
            .file_name()
            .context("package source has no file name")?
            .to_string_lossy();
        let expected = seen
            .get(name.as_ref())
            .context("package checksum file is missing an asset")?;
        let actual = hash_file(source)?;
        ensure!(
            actual == *expected,
            "package checksum does not match its ZIP"
        );
    }
    Ok(())
}

fn hash_file(path: &Path) -> Result<String> {
    let file = File::open(path).with_context(|| format!("could not open {}", path.display()))?;
    let mut reader = std::io::BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let count = reader
            .read(&mut buffer)
            .context("could not hash package file")?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    let digest = hasher.finalize();
    Ok(encode_sha256(&digest))
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

fn replace_file(source: &Path, destination: &Path) -> Result<()> {
    if let Ok(metadata) = fs::symlink_metadata(destination) {
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "existing staged package must be a regular file"
        );
    }
    let parent = destination
        .parent()
        .context("staged package has no parent")?;
    let mut temporary =
        NamedTempFile::new_in(parent).context("could not create staged package temp file")?;
    let mut input =
        File::open(source).with_context(|| format!("could not open {}", source.display()))?;
    std::io::copy(&mut input, &mut temporary).context("could not copy staged package")?;
    temporary
        .flush()
        .context("could not flush staged package")?;
    temporary
        .as_file()
        .sync_all()
        .context("could not persist staged package data")?;
    temporary
        .persist(destination)
        .context("could not publish staged package")?;
    Ok(())
}
