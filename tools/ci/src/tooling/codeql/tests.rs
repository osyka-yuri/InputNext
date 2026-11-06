use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::PathBuf;

use anyhow::{Context, Result, bail};
use flate2::{Compression, write::GzEncoder};
use tar::{Builder, EntryType, Header};
use tempfile::TempDir;

use crate::process::{CommandOutput, CommandSpec, Runner};
use crate::tooling::download::hash_file;
use crate::tooling::lock::{DependencyDefinition, ToolchainLock};
use crate::tooling::report::{ToolKind, ToolchainReport};
use crate::tooling::{install_dependencies, load_toolset, write_json_report};

use super::install_bundle;

struct NoDownload;

impl Runner for NoDownload {
    fn run(&mut self, _spec: &CommandSpec) -> Result<CommandOutput> {
        bail!("network access is forbidden in the cached bundle fixture")
    }
}

struct BundleFixture {
    temporary: TempDir,
    cache: PathBuf,
    cached: PathBuf,
    destination: PathBuf,
    dependency: DependencyDefinition,
}

impl BundleFixture {
    fn new(entry_type: EntryType) -> Result<Self> {
        let temporary = tempfile::tempdir()?;
        let cache = temporary.path().join("cache");
        let destination = temporary.path().join("job");
        fs::create_dir(&cache)?;
        fs::create_dir(&destination)?;

        let mut tar = Builder::new(GzEncoder::new(Vec::new(), Compression::fast()));
        let mut header = Header::new_gnu();
        header.set_mode(0o644);
        header.set_entry_type(entry_type);
        let body: &[u8] = if entry_type == EntryType::Regular {
            b"bundle fixture"
        } else {
            b""
        };
        header.set_size(body.len() as u64);
        if entry_type == EntryType::Symlink {
            header.set_link_name("outside")?;
        }
        header.set_cksum();
        tar.append_data(&mut header, "codeql/codeql.exe", body)?;
        let bytes = tar.into_inner()?.finish()?;
        let source = temporary.path().join("fixture.tar.gz");
        fs::write(&source, bytes)?;
        let sha256 = hash_file(&source)?;
        let cached = cache.join(format!("{sha256}.archive"));
        fs::copy(source, &cached)?;
        Ok(Self {
            temporary,
            cache,
            cached,
            destination,
            dependency: DependencyDefinition {
                version: "2.27.3".to_owned(),
                url: "https://example.invalid/codeql.tar.gz".to_owned(),
                sha256,
                archive: "tar.gz".to_owned(),
                extract_root: ".".to_owned(),
                expected_paths: Vec::new(),
            },
        })
    }
}

#[test]
fn action_handoff_uses_a_verified_gzip_path_and_preserves_shared_cache_names() -> Result<()> {
    let fixture = BundleFixture::new(EntryType::Regular)?;
    let lock = ToolchainLock {
        schema_version: 1,
        tools: BTreeMap::new(),
        dependencies: BTreeMap::from([("codeqlBundle".to_owned(), fixture.dependency.clone())]),
        compilers: BTreeMap::new(),
    };
    let mut tools = BTreeMap::new();
    let mut environment = BTreeMap::new();
    install_dependencies(
        &mut NoDownload,
        &lock,
        BTreeSet::from(["codeqlBundle".to_owned()]),
        &fixture.cache,
        &fixture.destination,
        &mut tools,
        &mut environment,
    )?;
    let report = fixture.temporary.path().join("toolset.json");
    write_json_report(
        &report,
        &ToolchainReport {
            schema_version: 1,
            profile: "codeql".to_owned(),
            platform: if cfg!(windows) {
                "windows-x64"
            } else {
                "linux-x64"
            }
            .to_owned(),
            lock_sha256: "0".repeat(64),
            tools,
            environment,
        },
    )?;
    let toolset = load_toolset(&report)?;
    let exported = PathBuf::from(
        toolset
            .environment()
            .get("INPUTNEXT_CODEQL_BUNDLE")
            .context("tool report must expose the Action's bundle path")?,
    );
    assert!(exported.is_absolute());
    assert!(exported.to_string_lossy().ends_with(".tar.gz"));
    assert_eq!(hash_file(&exported)?, fixture.dependency.sha256);
    assert_eq!(
        fixture.cached.extension().and_then(|ext| ext.to_str()),
        Some("archive")
    );
    let entry = toolset
        .report
        .tools
        .get("codeqlBundle")
        .context("bundle entry")?;
    assert_eq!(entry.kind, ToolKind::Archive);
    assert_eq!(entry.path, exported);
    assert_eq!(entry.archive.as_ref(), Some(&exported));
    let admitted = fs::read(&exported)?;
    fs::write(&fixture.cached, b"corrupt cached bytes")?;
    assert_eq!(fs::read(&exported)?, admitted);
    assert!(
        install_bundle(
            &mut NoDownload,
            &fixture.dependency,
            &fixture.cache,
            &fixture.destination
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn codeql_bundle_rejects_unsafe_contents_and_unsupported_compression() -> Result<()> {
    let mut fixture = BundleFixture::new(EntryType::Symlink)?;
    let error = install_bundle(
        &mut NoDownload,
        &fixture.dependency,
        &fixture.cache,
        &fixture.destination,
    )
    .err()
    .context("unsafe tar bundle was accepted")?;
    assert!(format!("{error:#}").contains("unsupported link or special file type"));
    assert_eq!(fs::read_dir(&fixture.destination)?.count(), 0);
    fixture.dependency.archive = "zip".to_owned();
    let error = install_bundle(
        &mut NoDownload,
        &fixture.dependency,
        &fixture.cache,
        &fixture.destination,
    )
    .err()
    .context("unsupported compression was accepted")?;
    assert!(error.to_string().contains("CodeQL bundle must use tar.gz"));
    assert_eq!(fs::read_dir(&fixture.destination)?.count(), 0);
    Ok(())
}
