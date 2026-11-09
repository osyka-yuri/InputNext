use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use serde_json::json;
use sha2::{Digest, Sha256};
use tempfile::TempDir;
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, ZipWriter};

pub(super) const VERSION: &str = "1.0.0";
pub(super) const REPOSITORY: &str = "owner/repository";
pub(super) const REPOSITORY_ID: u64 = 123;
pub(super) const REPOSITORY_OWNER_ID: u64 = 456;
pub(super) const SOURCE_SHA: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
pub(super) const RUN_ID: &str = "321";
pub(super) const RUN_ATTEMPT: &str = "2";
pub(super) const NOTES: &str = "## [1.0.0] — Test release\n\nDeterministic release fixture.";

pub(super) fn different_version(version: &str) -> Result<String> {
    let mut parsed = semver::Version::parse(version)?;
    parsed.patch += 1;
    Ok(parsed.to_string())
}

pub(super) struct Fixture {
    temporary: TempDir,
    pub(super) source_root: PathBuf,
    pub(super) identity_path: PathBuf,
    pub(super) packages: PathBuf,
}

impl Fixture {
    pub(super) fn new() -> Result<Self> {
        let temporary = tempfile::tempdir().context("could not create release fixture root")?;
        let source_root = temporary.path().join("source");
        fs::create_dir(&source_root)?;
        fs::create_dir(source_root.join("cmake"))?;
        fs::create_dir_all(source_root.join(".github/ci"))?;
        fs::write(
            source_root.join(".github/ci/release-trust.json"),
            serde_json::to_vec(&json!({
                "schemaVersion": 1,
                "repository": REPOSITORY,
                "repositoryId": REPOSITORY_ID,
                "repositoryOwnerId": REPOSITORY_OWNER_ID
            }))?,
        )?;
        fs::write(
            source_root.join("CMakeLists.txt"),
            include_str!("../../../../CMakeLists.txt"),
        )?;
        fs::write(
            source_root.join("cmake/GameInput.cmake"),
            include_str!("../../../../cmake/GameInput.cmake"),
        )?;
        fs::write(
            source_root.join("cmake/Toml.cmake"),
            include_str!("../../../../cmake/Toml.cmake"),
        )?;
        fs::write(source_root.join("CHANGELOG.md"), format!("{NOTES}\n"))?;
        let identity_path = temporary.path().join("trusted-identity.json");
        write_identity(
            &identity_path,
            REPOSITORY,
            SOURCE_SHA,
            SOURCE_SHA,
            RUN_ID,
            RUN_ATTEMPT,
        )?;
        let packages = temporary.path().join("native-packages");
        fs::create_dir(&packages)?;
        create_native_packages(&packages, VERSION, SOURCE_SHA)?;
        Ok(Self {
            temporary,
            source_root,
            identity_path,
            packages,
        })
    }

    pub(super) fn path(&self, relative: &str) -> PathBuf {
        self.temporary.path().join(relative)
    }

    pub(super) fn release_output(&self) -> PathBuf {
        self.path("assembled")
    }

    pub(super) fn unsigned_output(&self) -> PathBuf {
        self.path("unsigned")
    }

    pub(super) fn gh_executable(&self) -> Result<PathBuf> {
        let path = self.path("gh");
        fs::write(&path, b"controlled fake gh executable")?;
        Ok(path)
    }

    pub(super) fn write_bundles(&self) -> Result<[PathBuf; 3]> {
        let paths = [
            self.path("provenance.sigstore.json"),
            self.path("sbom-x64.sigstore.json"),
            self.path("sbom-x86.sigstore.json"),
        ];
        for path in &paths {
            fs::write(
                path,
                br#"{"mediaType":"application/vnd.dev.sigstore.bundle+json;version=0.3"}"#,
            )?;
        }
        Ok(paths)
    }
}

pub(super) fn write_identity(
    path: &Path,
    repository: &str,
    source_sha: &str,
    workflow_sha: &str,
    run_id: &str,
    attempt: &str,
) -> Result<()> {
    let identity = json!({
        "schemaVersion": 1,
        "repository": repository,
        "sourceCommit": source_sha,
        "sourceRef": format!("refs/tags/v{VERSION}"),
        "tag": format!("v{VERSION}"),
        "version": VERSION,
        "runId": run_id,
        "runAttempt": attempt,
        "workflowCommit": workflow_sha
    });
    fs::write(path, serde_json::to_vec_pretty(&identity)?)?;
    Ok(())
}

pub(super) fn create_native_packages(
    directory: &Path,
    version: &str,
    source_sha: &str,
) -> Result<()> {
    for architecture in ["x64", "x86"] {
        let runtime_name = format!("InputNext-{version}-windows-{architecture}.zip");
        let symbols_name = format!("InputNext-{version}-windows-{architecture}-symbols.zip");
        write_runtime_zip(
            &directory.join(&runtime_name),
            version,
            source_sha,
            architecture,
            &[],
        )?;
        write_symbols_zip(
            &directory.join(&symbols_name),
            version,
            source_sha,
            architecture,
        )?;
        let checksum = format!(
            "{}  {runtime_name}\n{}  {symbols_name}\n",
            sha256_file(&directory.join(&runtime_name))?,
            sha256_file(&directory.join(&symbols_name))?
        );
        fs::write(
            directory.join(format!(
                "InputNext-{version}-windows-{architecture}-SHA256SUMS.txt"
            )),
            checksum,
        )?;
    }
    Ok(())
}

pub(super) fn write_runtime_zip(
    path: &Path,
    version: &str,
    source_sha: &str,
    architecture: &str,
    extra_files: &[(String, Vec<u8>)],
) -> Result<()> {
    write_runtime_zip_with_pins(
        path,
        version,
        source_sha,
        architecture,
        "4.0.306",
        "3.4.0",
        extra_files,
    )
}

pub(super) fn write_runtime_zip_with_pins(
    path: &Path,
    version: &str,
    source_sha: &str,
    architecture: &str,
    gameinput_version: &str,
    tomlplusplus_version: &str,
    extra_files: &[(String, Vec<u8>)],
) -> Result<()> {
    let mut files = BTreeMap::new();
    for name in runtime_required_files() {
        let bytes = if *name == "licenses/third-party/Microsoft-GameInput.txt" {
            b"MICROSOFT SOFTWARE LICENSE TERMS\nFixture license text.\n".to_vec()
        } else if *name == "LICENSE" {
            b"MIT License\nInputNext fixture.\n".to_vec()
        } else {
            format!("release fixture file: {name}\n").into_bytes()
        };
        files.insert((*name).to_owned(), bytes);
    }
    files.insert(
        "docs/release.md".to_owned(),
        b"Release documentation fixture.\n".to_vec(),
    );
    for (name, bytes) in extra_files {
        files.insert(name.clone(), bytes.clone());
    }
    let build_info = json!({
        "name": "InputNext",
        "version": version,
        "architecture": architecture,
        "compiler": "MSVC 19.50.0",
        "configuration": "Release",
        "optimization": "LTCG",
        "pgo": "Off",
        "gameinput": gameinput_version,
        "tomlplusplus": tomlplusplus_version,
        "source_revision": source_sha,
        "source_modified": false
    });
    files.insert(
        "build-info.json".to_owned(),
        serde_json::to_vec(&build_info)?,
    );
    let manifest = manifest_bytes(&files)?;
    write_zip(path, &files, &[], &manifest)
}

pub(super) fn write_runtime_zip_with_symlink(
    path: &Path,
    version: &str,
    source_sha: &str,
    architecture: &str,
) -> Result<()> {
    let files = runtime_files(version, source_sha, architecture)?;
    let manifest = manifest_bytes(&files)?;
    let options = SimpleFileOptions::default();
    let mut archive = ZipWriter::new(fs::File::create(path)?);
    for (name, bytes) in &files {
        archive.start_file(name, options)?;
        archive.write_all(bytes)?;
    }
    archive.start_file("manifest.json", options)?;
    archive.write_all(&manifest)?;
    archive.add_symlink("links/runtime.dll", "../bin/inputnext_runtime.dll", options)?;
    archive.finish()?;
    Ok(())
}

pub(super) fn write_symbols_zip(
    path: &Path,
    version: &str,
    source_sha: &str,
    architecture: &str,
) -> Result<()> {
    let mut files = BTreeMap::from([
        (
            "symbols/inputnext_runtime.pdb".to_owned(),
            b"fixture runtime pdb".to_vec(),
        ),
        (
            "symbols/xinput1_3.pdb".to_owned(),
            b"fixture xinput 1.3 pdb".to_vec(),
        ),
        (
            "symbols/xinput1_4.pdb".to_owned(),
            b"fixture xinput 1.4 pdb".to_vec(),
        ),
        (
            "symbols/xinput9_1_0.pdb".to_owned(),
            b"fixture xinput 9.1.0 pdb".to_vec(),
        ),
    ]);
    let info = json!({
        "name": "InputNext",
        "version": version,
        "architecture": architecture,
        "compiler": "MSVC 19.50.0",
        "configuration": "Release",
        "optimization": "LTCG",
        "pgo": "Off",
        "gameinput": "4.0.306",
        "tomlplusplus": "3.4.0",
        "source_revision": source_sha,
        "source_modified": false
    });
    files.insert("build-info.json".to_owned(), serde_json::to_vec(&info)?);
    let manifest = manifest_bytes(&files)?;
    write_zip(path, &files, &[], &manifest)
}

pub(super) fn add_duplicate_central_entry(path: &Path, duplicate_name: &str) -> Result<()> {
    let mut bytes = fs::read(path)?;
    let eocd = find_eocd(&bytes)?;
    let central_size = read_u32(&bytes, eocd + 12)? as usize;
    let central_offset = read_u32(&bytes, eocd + 16)? as usize;
    let central_end = central_offset
        .checked_add(central_size)
        .context("central directory range overflow")?;
    ensure!(
        central_end == eocd,
        "fixture ZIP has an unexpected central directory layout"
    );
    let mut cursor = central_offset;
    let mut duplicate = None;
    while cursor < central_end {
        ensure!(
            read_u32(&bytes, cursor)? == 0x0201_4b50,
            "invalid central entry signature"
        );
        let name_length = read_u16(&bytes, cursor + 28)? as usize;
        let extra_length = read_u16(&bytes, cursor + 30)? as usize;
        let comment_length = read_u16(&bytes, cursor + 32)? as usize;
        let length = 46 + name_length + extra_length + comment_length;
        let name_start = cursor + 46;
        let name_end = name_start + name_length;
        let name = std::str::from_utf8(&bytes[name_start..name_end])?;
        if name == duplicate_name {
            duplicate = Some(bytes[cursor..cursor + length].to_vec());
            break;
        }
        cursor += length;
    }
    let duplicate = duplicate.context("requested fixture ZIP entry was not found")?;
    bytes.splice(eocd..eocd, duplicate.iter().copied());
    let new_eocd = find_eocd(&bytes)?;
    let count_on_disk = read_u16(&bytes, new_eocd + 8)?;
    let count_total = read_u16(&bytes, new_eocd + 10)?;
    let size = read_u32(&bytes, new_eocd + 12)?;
    bytes[new_eocd + 8..new_eocd + 10].copy_from_slice(
        &count_on_disk
            .checked_add(1)
            .context("too many ZIP entries")?
            .to_le_bytes(),
    );
    bytes[new_eocd + 10..new_eocd + 12].copy_from_slice(
        &count_total
            .checked_add(1)
            .context("too many ZIP entries")?
            .to_le_bytes(),
    );
    bytes[new_eocd + 12..new_eocd + 16].copy_from_slice(
        &size
            .checked_add(u32::try_from(duplicate.len())?)
            .context("central directory too large")?
            .to_le_bytes(),
    );
    fs::write(path, bytes)?;
    Ok(())
}

pub(super) fn runtime_required_files() -> &'static [&'static str] {
    &[
        "bin/inputnext_runtime.dll",
        "bin/xinput1_3.dll",
        "bin/xinput1_4.dll",
        "bin/xinput9_1_0.dll",
        "bin/GameInputRedist.dll",
        "bin/GameInputRawInputProxy.exe",
        "LICENSE",
        "README.md",
        "THIRD_PARTY_NOTICES.md",
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
    ]
}

pub(super) fn sha256_file(path: &Path) -> Result<String> {
    let bytes = fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
    Ok(hex(&Sha256::digest(bytes)))
}

fn runtime_files(
    version: &str,
    source_sha: &str,
    architecture: &str,
) -> Result<BTreeMap<String, Vec<u8>>> {
    let mut files = BTreeMap::new();
    for name in runtime_required_files() {
        let bytes = if *name == "licenses/third-party/Microsoft-GameInput.txt" {
            b"MICROSOFT SOFTWARE LICENSE TERMS\nFixture license text.\n".to_vec()
        } else if *name == "LICENSE" {
            b"MIT License\nInputNext fixture.\n".to_vec()
        } else {
            format!("release fixture file: {name}\n").into_bytes()
        };
        files.insert((*name).to_owned(), bytes);
    }
    files.insert(
        "docs/release.md".to_owned(),
        b"Release documentation fixture.\n".to_vec(),
    );
    let build_info = json!({
        "name": "InputNext",
        "version": version,
        "architecture": architecture,
        "compiler": "MSVC 19.50.0",
        "configuration": "Release",
        "optimization": "LTCG",
        "pgo": "Off",
        "gameinput": "4.0.306",
        "tomlplusplus": "3.4.0",
        "source_revision": source_sha,
        "source_modified": false
    });
    files.insert(
        "build-info.json".to_owned(),
        serde_json::to_vec(&build_info)?,
    );
    Ok(files)
}

fn write_zip(
    path: &Path,
    files: &BTreeMap<String, Vec<u8>>,
    extra_files: &[(String, Vec<u8>)],
    manifest: &[u8],
) -> Result<()> {
    let file = fs::File::create(path)?;
    let mut archive = ZipWriter::new(file);
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Stored);
    for (name, bytes) in files {
        archive.start_file(name, options)?;
        archive.write_all(bytes)?;
    }
    archive.start_file("manifest.json", options)?;
    archive.write_all(manifest)?;
    for (name, bytes) in extra_files {
        archive.start_file(name, options)?;
        archive.write_all(bytes)?;
    }
    archive.finish()?;
    Ok(())
}

fn manifest_bytes(files: &BTreeMap<String, Vec<u8>>) -> Result<Vec<u8>> {
    let mut hashes = BTreeMap::new();
    for (name, bytes) in files {
        hashes.insert(name, hex(&Sha256::digest(bytes)));
    }
    serde_json::to_vec(&json!({"schema": 1, "files": hashes}))
        .context("could not serialize package manifest")
}

fn find_eocd(bytes: &[u8]) -> Result<usize> {
    let range_start = bytes.len().saturating_sub(65_557);
    bytes[range_start..]
        .windows(4)
        .rposition(|window| window == b"PK\x05\x06")
        .map(|relative| range_start + relative)
        .context("ZIP end-of-central-directory record was not found")
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16> {
    let slice = bytes
        .get(offset..offset + 2)
        .context("ZIP fixture is truncated")?;
    Ok(u16::from_le_bytes([slice[0], slice[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let slice = bytes
        .get(offset..offset + 4)
        .context("ZIP fixture is truncated")?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn hex(bytes: &[u8]) -> String {
    let mut result = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        if write!(result, "{byte:02x}").is_err() {
            return String::new();
        }
    }
    result
}
