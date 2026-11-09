//! Exact release inventories, native package checks, and immutable handoffs.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};

use super::identity::{
    AssetDigest, QualifiedSource, ReleaseIdentity, canonical_directory, load_identity,
    load_metadata, read_json, validate_source, write_new_file, write_new_json,
};
use super::package::{
    SourcePins, ValidatedPackage, hash_file, is_lower_hex_64, regular_file, sha256_file,
    source_pins, validate_native_package,
};
use super::sbom;

const MAX_NOTES_BYTES: u64 = 2 * 1024 * 1024;
const MAX_BUNDLE_BYTES: u64 = 64 * 1024 * 1024;

const PUBLIC_UNSIGNED_FIXED: &[&str] = &["SHA256SUMS"];
const PRIVATE_NAMES: &[&str] = &["release-notes.md", "release-metadata.json"];

#[derive(Debug)]
pub(super) struct LocalRelease {
    pub root: PathBuf,
    pub public: PathBuf,
    pub notes_path: PathBuf,
    pub notes: String,
    pub public_digests: BTreeMap<String, String>,
    pub signed_names: Vec<String>,
}

pub(super) fn prepare(args: &super::PrepareArgs) -> Result<()> {
    let identity = load_identity(&args.identity)?;
    let source_root = canonical_directory(&args.source_root, "source root")?;
    let qualified = validate_source(identity, &source_root)?;
    let pins = source_pins(&source_root)?;
    let package_root = canonical_directory(&args.packages, "native package directory")?;
    let packages = validate_input_packages(&package_root, &qualified.identity, &pins)?;
    let sboms = generate_runtime_sboms(&packages, &qualified.identity, &pins)?;

    let output = require_new_absolute_directory(&args.out, "unsigned output directory")?;
    fs::create_dir(&output).with_context(|| {
        format!(
            "could not create unsigned output directory: {}",
            output.display()
        )
    })?;
    let mut copied_assets = Vec::new();
    for name in zip_names(&qualified.identity) {
        let source = package_root.join(&name);
        let destination = output.join(&name);
        copy_file_without_overwrite(&source, &destination)?;
        let expected = packages
            .get(&name)
            .context("validated package disappeared")?;
        ensure!(
            hash_file(&destination)? == (expected.sha1.clone(), expected.sha256.clone()),
            "native package changed while preparing its release handoff: {name}"
        );
        copied_assets.push(name);
    }
    for (name, bytes) in &sboms {
        let destination = output.join(name);
        write_new_file(&destination, bytes)?;
        copied_assets.push(name.clone());
    }

    let mut checksums = BTreeMap::new();
    for name in copied_assets {
        checksums.insert(name.clone(), sha256_file(&output.join(&name))?);
    }
    let checksum_bytes = checksum_file_bytes(&checksums);
    write_new_file(&output.join("SHA256SUMS"), &checksum_bytes)?;
    let mut asset_digests = checksums
        .iter()
        .map(|(name, sha256)| AssetDigest {
            name: name.clone(),
            sha256: sha256.clone(),
        })
        .collect::<Vec<_>>();
    asset_digests.push(AssetDigest {
        name: "SHA256SUMS".to_owned(),
        sha256: sha256_file(&output.join("SHA256SUMS"))?,
    });
    asset_digests.sort_by(|left, right| left.name.cmp(&right.name));

    let notes_bytes = qualified.notes.as_bytes();
    ensure!(
        notes_bytes.len() as u64 <= MAX_NOTES_BYTES,
        "release notes are too large"
    );
    write_new_file(&output.join("release-notes.md"), notes_bytes)?;
    let metadata = qualified.identity.metadata(
        sha256_file(&output.join("release-notes.md"))?,
        asset_digests,
    );
    write_new_json(&output.join("release-metadata.json"), &metadata)?;
    validate_unsigned_directory(&output, &qualified, &pins)?;
    Ok(())
}

fn validate_input_packages(
    package_root: &Path,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<BTreeMap<String, ValidatedPackage>> {
    let package_names = input_package_names(identity);
    require_exact_entries(
        package_root,
        &package_names.iter().map(String::as_str).collect(),
        "native package directory",
        false,
    )?;

    let mut packages = BTreeMap::new();
    for architecture in ["x64", "x86"] {
        let runtime_name = runtime_zip_name(identity, architecture);
        let symbols_name = symbols_zip_name(identity, architecture);
        let runtime = validate_native_package(
            &package_root.join(&runtime_name),
            architecture,
            identity,
            pins,
            false,
        )?;
        let symbols = validate_native_package(
            &package_root.join(&symbols_name),
            architecture,
            identity,
            pins,
            true,
        )?;
        let checksum_name = package_checksum_name(identity, architecture);
        validate_cmake_checksums(
            &package_root.join(checksum_name),
            &[
                (runtime_name.as_str(), runtime.sha256.as_str()),
                (symbols_name.as_str(), symbols.sha256.as_str()),
            ],
        )?;
        packages.insert(runtime_name, runtime);
        packages.insert(symbols_name, symbols);
    }
    Ok(packages)
}

fn generate_runtime_sboms(
    packages: &BTreeMap<String, ValidatedPackage>,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<BTreeMap<String, Vec<u8>>> {
    let mut sboms = BTreeMap::new();
    for architecture in ["x64", "x86"] {
        let package_name = runtime_zip_name(identity, architecture);
        let package = packages
            .get(&package_name)
            .context("validated runtime package disappeared")?;
        let bytes = sbom::generate(package, architecture, identity, pins)?;
        sboms.insert(sbom_name(identity, architecture), bytes);
    }
    Ok(sboms)
}

pub(super) fn check_unsigned(args: &super::CheckUnsignedArgs) -> Result<()> {
    let identity = load_identity(&args.identity)?;
    let source_root = canonical_directory(&args.source_root, "source root")?;
    let qualified = validate_source(identity, &source_root)?;
    let pins = source_pins(&source_root)?;
    let unsigned = canonical_directory(&args.unsigned, "unsigned handoff")?;
    validate_unsigned_directory(&unsigned, &qualified, &pins)
}

pub(super) fn assemble(args: super::AssembleArgs) -> Result<()> {
    let identity = load_identity(&args.identity)?;
    let source_root = canonical_directory(&args.source_root, "source root")?;
    let qualified = validate_source(identity, &source_root)?;
    let pins = source_pins(&source_root)?;
    let unsigned = canonical_directory(&args.unsigned, "unsigned handoff")?;
    validate_unsigned_directory(&unsigned, &qualified, &pins)?;

    let output = require_new_absolute_directory(&args.out, "assembled output directory")?;
    let bundles = [
        (
            provenance_bundle_name(&qualified.identity),
            args.provenance_bundle,
        ),
        (
            sbom_bundle_name(&qualified.identity, "x64"),
            args.sbom_x64_bundle,
        ),
        (
            sbom_bundle_name(&qualified.identity, "x86"),
            args.sbom_x86_bundle,
        ),
    ];
    for (_, path) in &bundles {
        validate_bundle(path)?;
    }

    fs::create_dir(&output).with_context(|| {
        format!(
            "could not create assembled output directory: {}",
            output.display()
        )
    })?;
    let public = output.join("public");
    let internal = output.join("internal");
    fs::create_dir(&public).context("could not create public release directory")?;
    fs::create_dir(&internal).context("could not create private release directory")?;

    for name in unsigned_public_names(&qualified.identity) {
        copy_file_without_overwrite(&unsigned.join(&name), &public.join(&name))?;
    }
    for (name, source) in bundles {
        copy_file_without_overwrite(&source, &public.join(name))?;
    }
    for name in PRIVATE_NAMES {
        copy_file_without_overwrite(&unsigned.join(name), &internal.join(name))?;
    }
    validate_release_directory(&output, &qualified, &pins)?;
    Ok(())
}

pub(super) fn validate_release_directory(
    root: &Path,
    qualified: &QualifiedSource,
    pins: &SourcePins,
) -> Result<LocalRelease> {
    let root = canonical_directory(root, "assembled release directory")?;
    require_exact_directories(&root, &["public", "internal"], "release root")?;
    let public = canonical_directory(&root.join("public"), "public release directory")?;
    let internal = canonical_directory(&root.join("internal"), "private release directory")?;
    let expected_signed = signed_public_names(&qualified.identity);
    let expected_public = expected_signed
        .iter()
        .map(String::as_str)
        .collect::<BTreeSet<_>>();
    let expected_internal = PRIVATE_NAMES.iter().copied().collect::<BTreeSet<_>>();
    require_exact_entries(&public, &expected_public, "public release handoff", false)?;
    require_exact_entries(
        &internal,
        &expected_internal,
        "private release handoff",
        false,
    )?;
    validate_unsigned_contents(&public, &internal, qualified, pins)?;
    for name in &expected_signed {
        if name.ends_with(".sigstore.json") {
            validate_bundle(&public.join(name))?;
        }
    }

    let mut public_digests = BTreeMap::new();
    for name in &expected_signed {
        public_digests.insert(name.clone(), sha256_file(&public.join(name))?);
    }
    Ok(LocalRelease {
        root,
        public,
        notes_path: internal.join("release-notes.md"),
        notes: qualified.notes.clone(),
        public_digests,
        signed_names: expected_signed,
    })
}

fn validate_unsigned_directory(
    root: &Path,
    qualified: &QualifiedSource,
    pins: &SourcePins,
) -> Result<()> {
    let names = unsigned_names(&qualified.identity);
    let expected = names.iter().map(String::as_str).collect::<BTreeSet<_>>();
    require_exact_entries(root, &expected, "unsigned release handoff", false)?;
    validate_unsigned_contents(root, root, qualified, pins)
}

fn validate_unsigned_contents(
    public: &Path,
    internal: &Path,
    qualified: &QualifiedSource,
    pins: &SourcePins,
) -> Result<()> {
    let notes_path = internal.join("release-notes.md");
    let metadata_path = internal.join("release-metadata.json");
    let notes_metadata = fs::symlink_metadata(&notes_path).context("release notes are missing")?;
    ensure!(
        notes_metadata.file_type().is_file() && notes_metadata.len() <= MAX_NOTES_BYTES,
        "release notes must be a bounded regular file"
    );
    let notes_bytes = fs::read(&notes_path).context("could not read release notes")?;
    let notes = std::str::from_utf8(&notes_bytes).context("release notes are not valid UTF-8")?;
    ensure!(!notes.trim().is_empty(), "release notes are empty");
    ensure!(
        notes_bytes == qualified.notes.as_bytes(),
        "release notes differ from the curated source changelog section"
    );
    let metadata = load_metadata(&metadata_path)?;
    let notes_hash = sha256_file(&notes_path)?;
    metadata.validate(&qualified.identity, &notes_hash)?;

    let expected_assets = unsigned_public_names(&qualified.identity);
    let mut expected_metadata = Vec::new();
    for name in &expected_assets {
        let digest = sha256_file(&public.join(name))?;
        expected_metadata.push(AssetDigest {
            name: name.clone(),
            sha256: digest,
        });
    }
    expected_metadata.sort_by(|left, right| left.name.cmp(&right.name));
    ensure!(
        metadata.assets.len() == expected_metadata.len()
            && metadata
                .assets
                .iter()
                .zip(&expected_metadata)
                .all(|(recorded, expected)| {
                    recorded.name == expected.name
                        && recorded.sha256 == expected.sha256
                        && is_lower_hex_64(&recorded.sha256)
                }),
        "release metadata asset list does not exactly match the unsigned handoff"
    );
    let checksum_names = checksum_asset_names(&qualified.identity);
    validate_checksums(public, &checksum_names)?;

    let mut packages = BTreeMap::new();
    for architecture in ["x64", "x86"] {
        let runtime_name = runtime_zip_name(&qualified.identity, architecture);
        let symbols_name = symbols_zip_name(&qualified.identity, architecture);
        let runtime = validate_native_package(
            &public.join(&runtime_name),
            architecture,
            &qualified.identity,
            pins,
            false,
        )?;
        validate_native_package(
            &public.join(symbols_name),
            architecture,
            &qualified.identity,
            pins,
            true,
        )?;
        packages.insert(runtime_name, runtime);
    }
    for architecture in ["x64", "x86"] {
        let name = sbom_name(&qualified.identity, architecture);
        let path = regular_file(&public.join(&name), "runtime SPDX document")?;
        let metadata = fs::metadata(&path)?;
        ensure!(
            metadata.len() <= MAX_NOTES_BYTES * 4,
            "runtime SPDX document is too large"
        );
        let bytes = fs::read(&path).context("could not read runtime SPDX document")?;
        let package = packages
            .get(&runtime_zip_name(&qualified.identity, architecture))
            .context("runtime package disappeared during SPDX verification")?;
        sbom::validate(&bytes, package, architecture, &qualified.identity, pins)?;
    }
    Ok(())
}

fn validate_checksums(public: &Path, package_names: &[String]) -> Result<()> {
    let path = regular_file(&public.join("SHA256SUMS"), "combined checksum file")?;
    let metadata = fs::metadata(&path)?;
    ensure!(
        metadata.len() <= 1024 * 1024,
        "combined checksum file is too large"
    );
    let contents = fs::read_to_string(&path).context("combined checksum file is not UTF-8")?;
    ensure!(
        contents.ends_with('\n') && !contents.contains('\r'),
        "combined checksum file is not canonical"
    );
    let mut actual = BTreeMap::new();
    for line in contents.lines() {
        let (hash, name) = line
            .split_once("  ")
            .context("combined checksum row is malformed")?;
        ensure!(
            !name.contains(' ') && is_lower_hex_64(hash),
            "combined checksum row is malformed"
        );
        ensure!(
            actual.insert(name.to_owned(), hash.to_owned()).is_none(),
            "combined checksum contains a duplicate row"
        );
    }
    let expected_names = package_names.iter().cloned().collect::<BTreeSet<_>>();
    ensure!(
        actual.keys().cloned().collect::<BTreeSet<_>>() == expected_names,
        "combined checksums do not exactly cover four ZIPs and two runtime SPDX files"
    );
    for (name, hash) in actual {
        ensure!(
            sha256_file(&public.join(&name))? == hash,
            "combined checksum mismatch for {name}"
        );
    }
    Ok(())
}

fn validate_cmake_checksums(path: &Path, packages: &[(&str, &str)]) -> Result<()> {
    let path = regular_file(path, "CMake package checksum file")?;
    let metadata = fs::metadata(&path)?;
    ensure!(
        metadata.len() <= 1024 * 1024,
        "CMake package checksum file is too large"
    );
    let contents = fs::read_to_string(&path).context("CMake package checksum file is not UTF-8")?;
    ensure!(
        contents.ends_with('\n') && !contents.contains('\r'),
        "CMake checksum file is not canonical"
    );
    let mut actual = BTreeMap::new();
    for line in contents.lines() {
        let (hash, name) = line
            .split_once("  ")
            .context("CMake checksum row is malformed")?;
        ensure!(
            !name.contains(' ') && is_lower_hex_64(hash),
            "CMake checksum row is malformed"
        );
        ensure!(
            actual.insert(name.to_owned(), hash.to_owned()).is_none(),
            "CMake checksum row is duplicated"
        );
    }
    let expected = packages
        .iter()
        .map(|(name, hash)| ((*name).to_owned(), (*hash).to_owned()))
        .collect::<BTreeMap<_, _>>();
    ensure!(
        actual == expected,
        "CMake package checksums do not match the exact architecture ZIPs"
    );
    Ok(())
}

fn validate_bundle(path: &Path) -> Result<()> {
    let path = regular_file(path, "attestation bundle")?;
    let metadata = fs::metadata(&path)?;
    ensure!(
        metadata.len() > 0 && metadata.len() <= MAX_BUNDLE_BYTES,
        "attestation bundle size is invalid"
    );
    let value = read_json(&path, MAX_BUNDLE_BYTES)?;
    ensure!(
        value.is_object(),
        "attestation bundle must be a JSON object"
    );
    Ok(())
}

fn require_exact_entries(
    directory: &Path,
    expected: &BTreeSet<&str>,
    label: &str,
    allow_directories: bool,
) -> Result<()> {
    let mut actual = BTreeSet::new();
    for entry in fs::read_dir(directory).with_context(|| format!("could not read {label}"))? {
        let entry = entry.with_context(|| format!("could not read an entry in {label}"))?;
        let file_type = entry
            .file_type()
            .context("could not inspect release handoff entry")?;
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| anyhow::anyhow!("{label} contains a non-UTF-8 entry name"))?;
        ensure!(
            file_type.is_file() || (allow_directories && file_type.is_dir()),
            "{label} contains a symlink or special file: {name}"
        );
        ensure!(
            actual.insert(name.clone()),
            "{label} contains a duplicate entry name"
        );
        ensure!(
            expected.contains(name.as_str()),
            "{label} contains an unexpected entry: {name}"
        );
    }
    ensure!(
        actual.len() == expected.len() && expected.iter().all(|name| actual.contains(*name)),
        "{label} is missing one or more required entries"
    );
    Ok(())
}

fn require_exact_directories(directory: &Path, expected: &[&str], label: &str) -> Result<()> {
    let mut actual = BTreeSet::new();
    for entry in fs::read_dir(directory).with_context(|| format!("could not read {label}"))? {
        let entry = entry.with_context(|| format!("could not read an entry in {label}"))?;
        ensure!(
            entry.file_type()?.is_dir(),
            "{label} contains a symlink or non-directory entry"
        );
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| anyhow::anyhow!("{label} contains a non-UTF-8 entry name"))?;
        ensure!(
            actual.insert(name.clone()),
            "{label} contains a duplicate directory"
        );
        ensure!(
            expected.contains(&name.as_str()),
            "{label} contains an unexpected directory: {name}"
        );
    }
    ensure!(
        actual.len() == expected.len() && expected.iter().all(|name| actual.contains(*name)),
        "{label} is missing one or more required directories"
    );
    Ok(())
}

fn require_new_absolute_directory(path: &Path, label: &str) -> Result<PathBuf> {
    ensure!(path.is_absolute(), "{label} path must be absolute");
    ensure!(!path.exists(), "{label} already exists: {}", path.display());
    let parent = path
        .parent()
        .context("release output has no parent directory")?;
    let parent = canonical_directory(parent, "release output parent")?;
    let name = path
        .file_name()
        .context("release output has no final path component")?;
    Ok(parent.join(name))
}

fn copy_file_without_overwrite(source: &Path, destination: &Path) -> Result<()> {
    let source = regular_file(source, "release input file")?;
    let mut source_file = fs::File::open(&source)
        .with_context(|| format!("could not open release input: {}", source.display()))?;
    let mut destination_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(destination)
        .with_context(|| {
            format!(
                "release output already exists or cannot be created: {}",
                destination.display()
            )
        })?;
    std::io::copy(&mut source_file, &mut destination_file)
        .with_context(|| format!("could not copy release input: {}", source.display()))?;
    destination_file
        .flush()
        .with_context(|| format!("could not flush release output: {}", destination.display()))?;
    Ok(())
}

fn checksum_file_bytes(checksums: &BTreeMap<String, String>) -> Vec<u8> {
    let mut bytes = Vec::new();
    for (name, checksum) in checksums {
        bytes.extend_from_slice(checksum.as_bytes());
        bytes.extend_from_slice(b"  ");
        bytes.extend_from_slice(name.as_bytes());
        bytes.push(b'\n');
    }
    bytes
}

fn input_package_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = zip_names(identity);
    for architecture in ["x64", "x86"] {
        names.push(package_checksum_name(identity, architecture));
    }
    names
}

fn unsigned_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = unsigned_public_names(identity);
    names.extend(PRIVATE_NAMES.iter().map(|name| (*name).to_owned()));
    names
}

fn unsigned_public_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = zip_names(identity);
    for architecture in ["x64", "x86"] {
        names.push(sbom_name(identity, architecture));
    }
    names.extend(PUBLIC_UNSIGNED_FIXED.iter().map(|name| (*name).to_owned()));
    names.sort();
    names
}

fn signed_public_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = unsigned_public_names(identity);
    names.push(provenance_bundle_name(identity));
    for architecture in ["x64", "x86"] {
        names.push(sbom_bundle_name(identity, architecture));
    }
    names.sort();
    names
}

fn zip_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = Vec::new();
    for architecture in ["x64", "x86"] {
        names.push(runtime_zip_name(identity, architecture));
        names.push(symbols_zip_name(identity, architecture));
    }
    names.sort();
    names
}

fn checksum_asset_names(identity: &ReleaseIdentity) -> Vec<String> {
    let mut names = zip_names(identity);
    for architecture in ["x64", "x86"] {
        names.push(sbom_name(identity, architecture));
    }
    names.sort();
    names
}

fn runtime_zip_name(identity: &ReleaseIdentity, architecture: &str) -> String {
    format!(
        "InputNext-{}-windows-{architecture}.zip",
        identity.version()
    )
}

fn symbols_zip_name(identity: &ReleaseIdentity, architecture: &str) -> String {
    format!(
        "InputNext-{}-windows-{architecture}-symbols.zip",
        identity.version()
    )
}

fn package_checksum_name(identity: &ReleaseIdentity, architecture: &str) -> String {
    format!(
        "InputNext-{}-windows-{architecture}-SHA256SUMS.txt",
        identity.version()
    )
}

pub(super) fn sbom_name(identity: &ReleaseIdentity, architecture: &str) -> String {
    format!(
        "InputNext-{}-windows-{architecture}.spdx.json",
        identity.version()
    )
}

pub(super) fn provenance_bundle_name(identity: &ReleaseIdentity) -> String {
    format!("InputNext-{}-provenance.sigstore.json", identity.version())
}

pub(super) fn sbom_bundle_name(identity: &ReleaseIdentity, architecture: &str) -> String {
    format!(
        "InputNext-{}-windows-{architecture}.sbom.sigstore.json",
        identity.version()
    )
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::{checksum_file_bytes, validate_cmake_checksums};

    #[test]
    fn combined_checksum_rows_are_sorted() -> Result<(), Box<dyn std::error::Error>> {
        let checksums = BTreeMap::from([
            ("z.zip".to_owned(), "f".repeat(64)),
            ("a.zip".to_owned(), "a".repeat(64)),
        ]);
        let contents = String::from_utf8(checksum_file_bytes(&checksums))?;
        assert!(contents.starts_with(&format!("{}  a.zip\n", "a".repeat(64))));
        assert!(contents.contains(&format!("{}  z.zip\n", "f".repeat(64))));
        Ok(())
    }

    #[test]
    fn cmake_checksums_are_required_to_be_exact() -> Result<(), Box<dyn std::error::Error>> {
        let temporary = tempfile::tempdir()?;
        let checksum_path = temporary.path().join("package-SHA256SUMS.txt");
        std::fs::write(
            &checksum_path,
            format!(
                "{}  runtime.zip\n{}  symbols.zip\n",
                "a".repeat(64),
                "b".repeat(64)
            ),
        )?;
        let hash_a = "a".repeat(64);
        let hash_b = "b".repeat(64);
        assert!(
            validate_cmake_checksums(
                &checksum_path,
                &[
                    ("runtime.zip", hash_a.as_str()),
                    ("symbols.zip", hash_b.as_str())
                ],
            )
            .is_ok()
        );
        Ok(())
    }
}
