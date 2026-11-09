//! Native package inventory validation and current `CMake` dependency pins.

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail, ensure};
use regex::Regex;
use serde_json::Value;
use sha1::{Digest, Sha1};
use sha2::Sha256;
use zip::ZipArchive;

use super::identity::ReleaseIdentity;

const MAX_ARCHIVE_BYTES: u64 = 1024 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 100 * 1024 * 1024;
const MAX_TOTAL_BYTES: u64 = 500 * 1024 * 1024;
const MAX_ARCHIVE_ENTRIES: usize = 10_000;
const MAX_SMALL_ENTRY_BYTES: u64 = 8 * 1024 * 1024;
const READ_BUFFER_BYTES: usize = 64 * 1024;
const RUNTIME_REQUIRED_FILES: &[&str] = &[
    "bin/inputnext_runtime.dll",
    "bin/xinput1_3.dll",
    "bin/xinput1_4.dll",
    "bin/xinput9_1_0.dll",
    "bin/GameInputRedist.dll",
    "bin/GameInputRawInputProxy.exe",
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "build-info.json",
    "manifest.json",
    "sdk/input_next_core.h",
    "sdk/input_next_effects.h",
    "examples/inputnext.toml",
    "examples/CMakeLists.txt",
    "licenses/third-party/Microsoft-GameInput.txt",
    "licenses/third-party/MIT-GameInput.txt",
    "licenses/third-party/NOTICE-GameInput.txt",
    "licenses/third-party/MIT-tomlplusplus.txt",
    "examples/core/core_example.cpp",
    "examples/core/core_reader.c",
    "examples/core/core_reader.h",
    "examples/effects/effects_example.cpp",
    "examples/effects/effects_demo.c",
    "examples/effects/effects_demo.h",
];

#[derive(Clone, Debug, Eq, PartialEq)]
pub(super) struct FileDigest {
    pub sha1: String,
    pub sha256: String,
    pub size: u64,
}

#[derive(Clone, Debug)]
pub(super) struct ValidatedPackage {
    pub path: PathBuf,
    pub sha1: String,
    pub sha256: String,
    pub files: BTreeMap<String, FileDigest>,
    pub selected_contents: BTreeMap<String, Vec<u8>>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(super) struct DependencyPin {
    pub name: String,
    pub version: String,
    pub purl: String,
    pub download_location: String,
    pub source_sha256: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(super) struct SourcePins {
    pub game_input: DependencyPin,
    pub tomlplusplus: DependencyPin,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum EntryKind {
    Directory,
    File,
}

pub(super) fn source_pins(root: &Path) -> Result<SourcePins> {
    let root_cmake = read_source_text(&root.join("CMakeLists.txt"), "root CMakeLists.txt")?;
    let gameinput_cmake =
        read_source_text(&root.join("cmake/GameInput.cmake"), "GameInput CMake pin")?;
    let toml_cmake = read_source_text(&root.join("cmake/Toml.cmake"), "toml++ CMake pin")?;

    let gameinput_version = match_one(
        r#"(?m)^[ \t]*set\([ \t]*INPUTNEXT_GAMEINPUT_VERSION[ \t]+"([0-9]+\.[0-9]+\.[0-9]+)"[ \t]*\)[ \t]*$"#,
        &root_cmake,
        "GameInput version",
    )?;
    let toml_version = match_one(
        r#"(?m)^[ \t]*set\([ \t]*INPUTNEXT_TOML_VERSION[ \t]+"([0-9]+\.[0-9]+\.[0-9]+)"[ \t]*\)[ \t]*$"#,
        &root_cmake,
        "toml++ version",
    )?;
    validate_stable_version(&gameinput_version, "GameInput")?;
    validate_stable_version(&toml_version, "toml++")?;

    let gameinput_url = "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/${INPUTNEXT_GAMEINPUT_VERSION}/microsoft.gameinput.${INPUTNEXT_GAMEINPUT_VERSION}.nupkg";
    let toml_url = "https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v${INPUTNEXT_TOML_VERSION}";
    ensure!(
        match_one(
            r"(?m)^[ \t]*URL[ \t]+([^\r\n]+)[ \t]*$",
            &gameinput_cmake,
            "GameInput source URL"
        )? == gameinput_url,
        "GameInput source URL differs from the approved NuGet source"
    );
    ensure!(
        match_one(
            r"(?m)^[ \t]*URL[ \t]+([^\r\n]+)[ \t]*$",
            &toml_cmake,
            "toml++ source URL"
        )? == toml_url,
        "toml++ source URL differs from the approved GitHub archive source"
    );
    let gameinput_hash = match_one(
        r"(?m)^[ \t]*URL_HASH[ \t]+SHA256=([0-9a-f]{64})[ \t]*$",
        &gameinput_cmake,
        "GameInput source archive SHA-256",
    )?;
    let toml_hash = match_one(
        r"(?m)^[ \t]*URL_HASH[ \t]+SHA256=([0-9a-f]{64})[ \t]*$",
        &toml_cmake,
        "toml++ source archive SHA-256",
    )?;

    Ok(SourcePins {
        game_input: DependencyPin {
            name: "Microsoft.GameInput".to_owned(),
            purl: format!("pkg:nuget/Microsoft.GameInput@{gameinput_version}"),
            download_location: format!(
                "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/{gameinput_version}/microsoft.gameinput.{gameinput_version}.nupkg"
            ),
            version: gameinput_version,
            source_sha256: gameinput_hash,
        },
        tomlplusplus: DependencyPin {
            name: "tomlplusplus".to_owned(),
            purl: format!("pkg:github/marzer/tomlplusplus@v{toml_version}"),
            download_location: format!(
                "https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v{toml_version}"
            ),
            version: toml_version,
            source_sha256: toml_hash,
        },
    })
}

pub(super) fn validate_native_package(
    path: &Path,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
    symbols: bool,
) -> Result<ValidatedPackage> {
    ensure!(
        architecture == "x64" || architecture == "x86",
        "unsupported release package architecture"
    );
    let package_path = regular_file(path, "package ZIP")?;
    let metadata = fs::metadata(&package_path)?;
    ensure!(
        metadata.len() <= MAX_ARCHIVE_BYTES,
        "package ZIP is too large"
    );
    let (zip_sha1, zip_sha256) = hash_file(&package_path)?;
    let file = File::open(&package_path)
        .with_context(|| format!("could not open package ZIP: {}", package_path.display()))?;
    let mut archive = ZipArchive::new(file)
        .with_context(|| format!("invalid package ZIP: {}", package_path.display()))?;
    let inspected = inspect_archive(&mut archive)?;
    validate_manifest(&inspected.files, &inspected.selected_contents)?;
    validate_build_info(
        &inspected.files,
        &inspected.selected_contents,
        architecture,
        identity,
        pins,
    )?;
    validate_package_shape(&inspected.files, architecture, symbols)?;

    Ok(ValidatedPackage {
        path: package_path,
        sha1: zip_sha1,
        sha256: zip_sha256,
        files: inspected.files,
        selected_contents: inspected.selected_contents,
    })
}

#[derive(Debug)]
struct InspectedArchive {
    files: BTreeMap<String, FileDigest>,
    selected_contents: BTreeMap<String, Vec<u8>>,
}

fn inspect_archive(archive: &mut ZipArchive<File>) -> Result<InspectedArchive> {
    ensure!(
        !archive.is_empty() && archive.len() <= MAX_ARCHIVE_ENTRIES,
        "package ZIP entry count is outside the safe limit"
    );
    let mut inspection = ArchiveInspection::default();
    for index in 0..archive.len() {
        inspection.read_entry(archive, index)?;
    }
    inspection.finish()
}

#[derive(Debug, Default)]
struct ArchiveInspection {
    kinds: BTreeMap<String, EntryKind>,
    folded: BTreeMap<String, String>,
    files: BTreeMap<String, FileDigest>,
    selected_contents: BTreeMap<String, Vec<u8>>,
    total_size: u64,
}

impl ArchiveInspection {
    fn read_entry(&mut self, archive: &mut ZipArchive<File>, index: usize) -> Result<()> {
        let mut entry = archive
            .by_index(index)
            .with_context(|| format!("could not read package ZIP entry {index}"))?;
        ensure!(!entry.encrypted(), "encrypted ZIP entries are not accepted");
        ensure!(!entry.is_symlink(), "ZIP symlink entries are not accepted");
        let name = std::str::from_utf8(entry.name_raw())
            .context("ZIP entry path is not valid UTF-8")?
            .to_owned();
        let (path, kind) = validate_entry_name(&name, entry.is_dir())?;
        let mode_type = entry.unix_mode().unwrap_or_default() & 0o170_000;
        match kind {
            EntryKind::Directory => ensure!(
                mode_type == 0 || mode_type == 0o040_000,
                "ZIP directory has a non-directory file type: {name}"
            ),
            EntryKind::File => ensure!(
                mode_type == 0 || mode_type == 0o100_000,
                "ZIP entry is not a regular file: {name}"
            ),
        }
        let folded_name = path.to_lowercase();
        if let Some(previous) = self.folded.insert(folded_name, path.clone()) {
            bail!("ZIP contains duplicate or case-colliding paths: {previous} and {path}");
        }
        ensure!(
            self.kinds.insert(path.clone(), kind).is_none(),
            "ZIP contains a duplicate path: {path}"
        );
        if kind == EntryKind::Directory {
            ensure!(
                entry.size() == 0,
                "ZIP directory entry contains data: {path}"
            );
            return Ok(());
        }
        let declared_size = entry.size();
        ensure!(
            declared_size <= MAX_ENTRY_BYTES,
            "ZIP entry exceeds the per-file size limit: {path}"
        );
        self.total_size = self
            .total_size
            .checked_add(declared_size)
            .context("ZIP expanded-size counter overflow")?;
        ensure!(
            self.total_size <= MAX_TOTAL_BYTES,
            "ZIP exceeds the expanded-size limit"
        );

        let mut sha1 = Sha1::new();
        let mut sha256 = Sha256::new();
        let keep_contents = matches!(path.as_str(), "manifest.json" | "build-info.json")
            || path == "licenses/third-party/Microsoft-GameInput.txt";
        let mut contents = keep_contents.then(Vec::new);
        let mut read_size = 0_u64;
        let mut buffer = vec![0_u8; READ_BUFFER_BYTES];
        loop {
            let count = entry
                .read(&mut buffer)
                .with_context(|| format!("could not stream ZIP entry: {path}"))?;
            if count == 0 {
                break;
            }
            let count = u64::try_from(count).context("ZIP read returned an invalid byte count")?;
            read_size = read_size
                .checked_add(count)
                .context("ZIP entry size counter overflow")?;
            ensure!(
                read_size <= declared_size,
                "ZIP entry expanded beyond its declared size: {path}"
            );
            sha1.update(&buffer[..usize::try_from(count)?]);
            sha256.update(&buffer[..usize::try_from(count)?]);
            if let Some(contents) = &mut contents {
                ensure!(
                    read_size <= MAX_SMALL_ENTRY_BYTES,
                    "ZIP metadata entry is too large: {path}"
                );
                contents.extend_from_slice(&buffer[..usize::try_from(count)?]);
            }
        }
        ensure!(
            read_size == declared_size,
            "ZIP entry size differs from its header: {path}"
        );
        self.files.insert(
            path.clone(),
            FileDigest {
                sha1: hex_encode(&sha1.finalize()),
                sha256: hex_encode(&sha256.finalize()),
                size: read_size,
            },
        );
        if let Some(contents) = contents {
            self.selected_contents.insert(path, contents);
        }
        Ok(())
    }

    fn finish(self) -> Result<InspectedArchive> {
        validate_path_tree(&self.kinds)?;
        ensure!(!self.files.is_empty(), "package ZIP contains no files");
        Ok(InspectedArchive {
            files: self.files,
            selected_contents: self.selected_contents,
        })
    }
}

pub(super) fn hash_file(path: &Path) -> Result<(String, String)> {
    let mut file = File::open(path)
        .with_context(|| format!("could not open file for hashing: {}", path.display()))?;
    let mut sha1 = Sha1::new();
    let mut sha256 = Sha256::new();
    let mut buffer = vec![0_u8; READ_BUFFER_BYTES];
    loop {
        let count = file
            .read(&mut buffer)
            .with_context(|| format!("could not hash file: {}", path.display()))?;
        if count == 0 {
            break;
        }
        sha1.update(&buffer[..count]);
        sha256.update(&buffer[..count]);
    }
    Ok((hex_encode(&sha1.finalize()), hex_encode(&sha256.finalize())))
}

pub(super) fn sha256_file(path: &Path) -> Result<String> {
    hash_file(path).map(|(_, sha256)| sha256)
}

pub(super) fn regular_file(path: &Path, label: &str) -> Result<PathBuf> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect {label}: {}", path.display()))?;
    ensure!(
        metadata.file_type().is_file(),
        "{label} must be a regular file: {}",
        path.display()
    );
    path.canonicalize()
        .with_context(|| format!("could not resolve {label}: {}", path.display()))
}

fn read_source_text(path: &Path, label: &str) -> Result<String> {
    let path = regular_file(path, label)?;
    let metadata = fs::metadata(&path)?;
    ensure!(metadata.len() <= 1024 * 1024, "{label} is too large");
    fs::read_to_string(&path).with_context(|| format!("{label} is not valid UTF-8"))
}

fn validate_entry_name(name: &str, directory: bool) -> Result<(String, EntryKind)> {
    ensure!(!name.is_empty(), "ZIP entry has an empty path");
    ensure!(
        !name.starts_with('/')
            && !name.contains('\\')
            && !name.contains(':')
            && !name.contains(';')
            && !name.contains('\0'),
        "ZIP entry path is not canonical: {name:?}"
    );
    ensure!(!name.starts_with("//"), "ZIP entry path is absolute");
    let trailing_separator = name.ends_with('/');
    ensure!(
        trailing_separator == directory,
        "ZIP entry directory marker disagrees with its type: {name:?}"
    );
    let without_separator = name.strip_suffix('/').unwrap_or(name);
    let components = without_separator.split('/').collect::<Vec<_>>();
    ensure!(
        !components.is_empty()
            && components
                .iter()
                .all(|component| !component.is_empty() && *component != "." && *component != ".."),
        "ZIP entry path contains an empty or traversal component: {name:?}"
    );
    ensure!(
        !Path::new(without_separator).is_absolute(),
        "ZIP entry path is absolute: {name:?}"
    );
    Ok((
        without_separator.to_owned(),
        if directory {
            EntryKind::Directory
        } else {
            EntryKind::File
        },
    ))
}

fn validate_path_tree(kinds: &BTreeMap<String, EntryKind>) -> Result<()> {
    for path in kinds.keys() {
        let components = path.split('/').collect::<Vec<_>>();
        for end in 1..components.len() {
            let parent = components[..end].join("/");
            ensure!(
                kinds.get(&parent) != Some(&EntryKind::File),
                "ZIP path is nested below a regular file: {parent}"
            );
        }
    }
    Ok(())
}

fn validate_manifest(
    files: &BTreeMap<String, FileDigest>,
    selected_contents: &BTreeMap<String, Vec<u8>>,
) -> Result<()> {
    let bytes = selected_contents
        .get("manifest.json")
        .context("package ZIP has no manifest.json")?;
    let manifest: Value =
        super::strict_json::parse_slice(bytes).context("package manifest JSON is invalid")?;
    let object = manifest
        .as_object()
        .context("package manifest must be a JSON object")?;
    ensure!(
        object.len() == 2 && manifest["schema"].as_u64() == Some(1),
        "unsupported package manifest shape"
    );
    let expected = manifest["files"]
        .as_object()
        .context("package manifest files must be a JSON object")?;
    ensure!(!expected.is_empty(), "package manifest has no files");
    ensure!(
        expected.len() + 1 == files.len(),
        "package manifest does not cover every package file"
    );
    for (path, recorded) in expected {
        let (canonical, kind) = validate_entry_name(path, false)?;
        ensure!(
            canonical == *path && kind == EntryKind::File,
            "manifest path is not canonical: {path}"
        );
        let recorded = recorded
            .as_str()
            .context("package manifest hash must be a string")?;
        ensure!(
            is_lower_hex_64(recorded),
            "package manifest SHA-256 is invalid for {path}"
        );
        let actual = files
            .get(path)
            .with_context(|| format!("package manifest lists a missing file: {path}"))?;
        ensure!(
            actual.sha256 == recorded,
            "package manifest SHA-256 mismatch for {path}"
        );
    }
    ensure!(
        !expected.contains_key("manifest.json"),
        "package manifest must not hash itself"
    );
    Ok(())
}

fn validate_build_info(
    files: &BTreeMap<String, FileDigest>,
    selected_contents: &BTreeMap<String, Vec<u8>>,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<()> {
    let bytes = selected_contents
        .get("build-info.json")
        .context("package ZIP has no build-info.json")?;
    let info: Value =
        super::strict_json::parse_slice(bytes).context("package build-info JSON is invalid")?;
    ensure!(
        info["name"].as_str() == Some("InputNext")
            && info["version"].as_str() == Some(identity.version())
            && info["architecture"].as_str() == Some(architecture)
            && info["configuration"].as_str() == Some("Release")
            && info["optimization"].as_str() == Some("LTCG")
            && info["source_revision"].as_str() == Some(identity.source_commit())
            && info["source_modified"].as_bool() == Some(false),
        "package build metadata differs from the clean source release identity"
    );
    ensure!(
        matches!(info["pgo"].as_str(), Some("Off" | "Optimize")),
        "package is not a supported production PGO build"
    );
    ensure!(
        info["gameinput"].as_str() == Some(pins.game_input.version.as_str())
            && info["tomlplusplus"].as_str() == Some(pins.tomlplusplus.version.as_str()),
        "package dependency versions differ from the source CMake pins"
    );
    ensure!(
        files.contains_key("manifest.json") && files.contains_key("build-info.json"),
        "package metadata is incomplete"
    );
    Ok(())
}

fn validate_package_shape(
    files: &BTreeMap<String, FileDigest>,
    architecture: &str,
    symbols: bool,
) -> Result<()> {
    if symbols {
        let expected = [
            "build-info.json",
            "manifest.json",
            "symbols/inputnext_runtime.pdb",
            "symbols/xinput1_3.pdb",
            "symbols/xinput1_4.pdb",
            "symbols/xinput9_1_0.pdb",
        ]
        .into_iter()
        .collect::<std::collections::BTreeSet<_>>();
        let actual = files
            .keys()
            .map(String::as_str)
            .collect::<std::collections::BTreeSet<_>>();
        ensure!(
            actual == expected,
            "symbols package does not match the CMake distribution shape"
        );
        return Ok(());
    }

    for required in RUNTIME_REQUIRED_FILES {
        ensure!(
            files.contains_key(*required),
            "runtime package is missing required {architecture} file {required}"
        );
    }
    for path in files.keys() {
        ensure!(
            RUNTIME_REQUIRED_FILES.contains(&path.as_str()) || valid_distribution_doc(path),
            "runtime package has a file outside the CMake distribution shape: {path}"
        );
    }
    Ok(())
}

fn valid_distribution_doc(path: &str) -> bool {
    let Some(filename) = path.strip_prefix("docs/") else {
        return false;
    };
    let Some(stem) = filename.strip_suffix(".md") else {
        return false;
    };
    !stem.is_empty()
        && stem
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

fn match_one(pattern: &str, contents: &str, label: &str) -> Result<String> {
    let expression = Regex::new(pattern).with_context(|| format!("invalid {label} expression"))?;
    let matches = expression
        .captures_iter(contents)
        .filter_map(|capture| capture.get(1))
        .map(|capture| capture.as_str().to_owned())
        .collect::<Vec<_>>();
    ensure!(matches.len() == 1, "source must define exactly one {label}");
    matches
        .into_iter()
        .next()
        .context("source field disappeared")
}

fn validate_stable_version(version: &str, label: &str) -> Result<()> {
    let parsed =
        semver::Version::parse(version).with_context(|| format!("invalid {label} version"))?;
    ensure!(
        parsed.pre.is_empty() && parsed.build.is_empty() && parsed.to_string() == version,
        "{label} version must be stable and canonical"
    );
    Ok(())
}

pub(super) fn is_lower_hex_64(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

pub(super) fn hex_encode(bytes: &[u8]) -> String {
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(encoded, "{byte:02x}");
    }
    encoded
}

#[cfg(test)]
mod tests {
    use super::validate_entry_name;

    #[test]
    fn rejects_unsafe_archive_paths_and_accepts_relative_files() {
        assert!(validate_entry_name("../escape", false).is_err());
        assert!(validate_entry_name("C:/absolute", false).is_err());
        assert!(validate_entry_name("folder\\escape", false).is_err());
        assert!(validate_entry_name("bin/runtime.dll", false).is_ok());
        assert!(validate_entry_name("bin/", true).is_ok());
    }
}
