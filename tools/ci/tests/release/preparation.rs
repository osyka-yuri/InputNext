use std::collections::BTreeSet;
use std::fs;
use std::path::Path;

use anyhow::{Context, Result, bail};
use serde_json::Value;

use super::commands::{assemble, check_unsigned, prepare};
use super::fixtures::{self, Fixture, SOURCE_SHA, VERSION};

fn entry_names(directory: &Path) -> Result<BTreeSet<String>> {
    fs::read_dir(directory)?
        .map(|entry| {
            entry?
                .file_name()
                .into_string()
                .map_err(|_| anyhow::anyhow!("fixture entry name is not UTF-8"))
        })
        .collect()
}

fn validate_spdx_schema(path: &Path) -> Result<()> {
    let schema: Value = serde_json::from_str(include_str!("spdx-schema-2.2.2.json"))?;
    let document: Value = serde_json::from_slice(&fs::read(path)?)?;
    let validator = jsonschema::validator_for(&schema)
        .context("could not load the vendored SPDX 2.2.2 schema")?;
    if let Err(error) = validator.validate(&document) {
        bail!(
            "generated SPDX document fails schema at {}: {error}",
            path.display()
        );
    }
    Ok(())
}

#[test]
fn prepares_exact_nine_file_handoff_and_assembles_exact_public_inventory() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    check_unsigned(&fixture)?;
    assert_eq!(entry_names(&fixture.unsigned_output())?.len(), 9);

    assemble(&fixture)?;
    assert_eq!(
        entry_names(&fixture.release_output().join("public"))?.len(),
        10
    );
    assert_eq!(
        entry_names(&fixture.release_output().join("internal"))?.len(),
        2
    );
    Ok(())
}

#[test]
fn generated_runtime_spdx_documents_match_the_official_2_2_2_schema() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    for architecture in ["x64", "x86"] {
        validate_spdx_schema(&fixture.unsigned_output().join(format!(
            "InputNext-{VERSION}-windows-{architecture}.spdx.json"
        )))?;
    }
    Ok(())
}

#[test]
fn rejects_extra_package_inputs_and_corrupt_cmake_checksums() -> Result<()> {
    let fixture = Fixture::new()?;
    fs::write(fixture.packages.join("unexpected.txt"), "extra")?;
    assert!(prepare(&fixture).is_err());

    let fixture = Fixture::new()?;
    fs::write(
        fixture
            .packages
            .join(format!("InputNext-{VERSION}-windows-x64-SHA256SUMS.txt")),
        format!("{}  InputNext-{VERSION}-windows-x64.zip\n", "0".repeat(64)),
    )?;
    assert!(prepare(&fixture).is_err());
    Ok(())
}

#[test]
fn rejects_traversal_duplicate_and_symlink_zip_entries() -> Result<()> {
    let cases = ["traversal", "duplicate", "case-collision", "symlink"];
    for case in cases {
        let fixture = Fixture::new()?;
        let runtime = fixture
            .packages
            .join(format!("InputNext-{VERSION}-windows-x64.zip"));
        match case {
            "traversal" => fixtures::write_runtime_zip(
                &runtime,
                VERSION,
                SOURCE_SHA,
                "x64",
                &[("../escape.txt".to_owned(), b"escape".to_vec())],
            )?,
            "duplicate" => fixtures::add_duplicate_central_entry(&runtime, "LICENSE")?,
            "case-collision" => fixtures::write_runtime_zip(
                &runtime,
                VERSION,
                SOURCE_SHA,
                "x64",
                &[(
                    "BIN/inputnext_runtime.dll".to_owned(),
                    b"collision".to_vec(),
                )],
            )?,
            "symlink" => {
                fixtures::write_runtime_zip_with_symlink(&runtime, VERSION, SOURCE_SHA, "x64")?;
            }
            _ => bail!("unknown ZIP fixture case"),
        }
        assert!(prepare(&fixture).is_err(), "{case} ZIP entry was accepted");
    }
    Ok(())
}

#[test]
fn rejects_unknown_zip_members_even_with_a_matching_manifest() -> Result<()> {
    let fixture = Fixture::new()?;
    let runtime = fixture
        .packages
        .join(format!("InputNext-{VERSION}-windows-x64.zip"));
    fixtures::write_runtime_zip(
        &runtime,
        VERSION,
        SOURCE_SHA,
        "x64",
        &[("bin/unexpected.dll".to_owned(), b"extra".to_vec())],
    )?;
    let error = prepare(&fixture)
        .err()
        .context("unknown ZIP member was accepted")?;
    assert!(format!("{error:#}").contains("file outside the CMake distribution shape"));
    Ok(())
}

#[test]
fn rejects_runtime_build_metadata_that_disagrees_with_source_dependency_pins() -> Result<()> {
    let fixture = Fixture::new()?;
    fixtures::write_runtime_zip_with_pins(
        &fixture
            .packages
            .join(format!("InputNext-{VERSION}-windows-x64.zip")),
        VERSION,
        SOURCE_SHA,
        "x64",
        "4.0.306",
        "9.9.9",
        &[],
    )?;
    assert!(prepare(&fixture).is_err());
    Ok(())
}
