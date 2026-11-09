use std::fs;

use anyhow::{Context, Result};
use serde_json::{Value, json};

use super::commands::{assemble, check_unsigned, draft, prepare, verify};
use super::fixtures::{self, Fixture, VERSION};
use super::github::{AttestationMutation, MockGh};

fn invalid_trust_policies(valid_policy: &Value) -> Result<Vec<(String, Option<Vec<u8>>)>> {
    let valid_text = serde_json::to_string(valid_policy)?;
    let repository_id = valid_policy["repositoryId"]
        .as_u64()
        .context("policy repository ID is missing")?;
    let mut invalid_policies = vec![
        ("missing policy".to_owned(), None),
        ("empty policy".to_owned(), Some(b"{}".to_vec())),
        (
            "oversize policy".to_owned(),
            Some(vec![b' '; 16 * 1024 + 1]),
        ),
        (
            "duplicate key".to_owned(),
            Some(
                format!(
                    "{{\"repositoryId\":{repository_id},{}",
                    valid_text
                        .strip_prefix('{')
                        .context("policy is not an object")?
                )
                .into_bytes(),
            ),
        ),
        (
            "out-of-range ID".to_owned(),
            Some(
                valid_text
                    .replace(
                        &format!("\"repositoryId\":{repository_id}"),
                        "\"repositoryId\":18446744073709551616",
                    )
                    .into_bytes(),
            ),
        ),
    ];
    for (field, value) in [
        ("schemaVersion", json!(2)),
        ("repository", json!("owner/other")),
        ("repositoryId", json!(0)),
        ("repositoryOwnerId", json!(0)),
        ("repositoryId", json!(-1)),
        ("repositoryId", json!(1.5)),
        ("repositoryId", json!("123")),
        ("repositoryOwnerId", json!("456")),
        ("unexpected", json!(true)),
    ] {
        let label = format!("invalid {field}: {value}");
        let mut changed = valid_policy.clone();
        changed[field] = value;
        invalid_policies.push((label, Some(serde_json::to_vec(&changed)?)));
    }
    for field in ["repositoryId", "repositoryOwnerId"] {
        let mut changed = valid_policy.clone();
        changed
            .as_object_mut()
            .context("policy is not an object")?
            .remove(field);
        invalid_policies.push((
            format!("missing {field}"),
            Some(serde_json::to_vec(&changed)?),
        ));
    }
    Ok(invalid_policies)
}

#[test]
fn rejects_curated_notes_workflow_identity_and_trust_policy_drift() -> Result<()> {
    let fixture = Fixture::new()?;
    fs::write(
        fixture.source_root.join("CHANGELOG.md"),
        "## [Unreleased]\n\nWork in progress.\n",
    )?;
    assert!(prepare(&fixture).is_err());

    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    let mut identity: Value = serde_json::from_slice(&fs::read(&fixture.identity_path)?)?;
    identity["runAttempt"] = json!("3");
    fs::write(&fixture.identity_path, serde_json::to_vec(&identity)?)?;
    assert!(check_unsigned(&fixture).is_err());

    let fixture = Fixture::new()?;
    let source_path = fixture.source_root.join("CMakeLists.txt");
    let different = fixtures::different_version(VERSION)?;
    let original = fs::read_to_string(&source_path)?;
    let source = original.replacen(
        &format!("project(InputNext VERSION {VERSION}"),
        &format!("project(InputNext VERSION {different}"),
        1,
    );
    assert_ne!(source, original, "version mutation was not applied");
    fs::write(source_path, source)?;
    assert!(prepare(&fixture).is_err());

    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    let policy_path = fixture.source_root.join(".github/ci/release-trust.json");
    let valid_policy: Value = serde_json::from_slice(&fs::read(&policy_path)?)?;
    for (label, bytes) in invalid_trust_policies(&valid_policy)? {
        if let Some(bytes) = bytes {
            fs::write(&policy_path, bytes)?;
        } else {
            fs::remove_file(&policy_path)?;
        }
        let mut runner = MockGh::new(&fixture)?;
        assert!(verify(&fixture, &mut runner).is_err(), "accepted {label}");
        assert_eq!(runner.calls, Vec::<Vec<String>>::new());
        let mut runner = MockGh::new(&fixture)?;
        assert!(
            draft(&fixture, &mut runner, false).is_err(),
            "draft accepted {label}"
        );
        assert_eq!(runner.calls, Vec::<Vec<String>>::new());
        assert_eq!(runner.release_write_count(), 0);
    }
    Ok(())
}

#[test]
fn rejects_missing_or_extra_unsigned_handoff_entries() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    fs::remove_file(fixture.unsigned_output().join("SHA256SUMS"))?;
    assert!(check_unsigned(&fixture).is_err());

    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    fs::write(fixture.unsigned_output().join("extra.txt"), "extra")?;
    assert!(check_unsigned(&fixture).is_err());
    Ok(())
}

#[test]
fn rejects_an_internally_rehashed_but_inaccurate_runtime_sbom() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    let sbom_name = format!("InputNext-{VERSION}-windows-x64.spdx.json");
    let sbom_path = fixture.unsigned_output().join(&sbom_name);
    let mut sbom: Value = serde_json::from_slice(&fs::read(&sbom_path)?)?;
    sbom["files"] = json!([]);
    fs::write(&sbom_path, serde_json::to_vec(&sbom)?)?;

    let checksum_path = fixture.unsigned_output().join("SHA256SUMS");
    let mut rows = fs::read_to_string(&checksum_path)?
        .lines()
        .filter_map(|line| line.split_once("  "))
        .map(|(digest, name)| (name.to_owned(), digest.to_owned()))
        .collect::<std::collections::BTreeMap<_, _>>();
    rows.insert(sbom_name.clone(), fixtures::sha256_file(&sbom_path)?);
    let mut checksum_bytes = Vec::new();
    for (name, digest) in &rows {
        checksum_bytes.extend_from_slice(digest.as_bytes());
        checksum_bytes.extend_from_slice(b"  ");
        checksum_bytes.extend_from_slice(name.as_bytes());
        checksum_bytes.push(b'\n');
    }
    fs::write(&checksum_path, &checksum_bytes)?;

    let metadata_path = fixture.unsigned_output().join("release-metadata.json");
    let mut metadata: Value = serde_json::from_slice(&fs::read(&metadata_path)?)?;
    for asset in metadata["assets"]
        .as_array_mut()
        .context("metadata asset list is missing")?
    {
        match asset["name"].as_str() {
            Some(name) if name == sbom_name => asset["sha256"] = json!(rows[&sbom_name]),
            Some("SHA256SUMS") => {
                asset["sha256"] = json!(fixtures::sha256_file(&checksum_path)?);
            }
            _ => {}
        }
    }
    fs::write(metadata_path, serde_json::to_vec(&metadata)?)?;
    assert!(check_unsigned(&fixture).is_err());
    Ok(())
}

#[test]
fn verify_and_draft_reject_wrong_certificate_source_identity() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    for mutation in [
        AttestationMutation::WrongSource,
        AttestationMutation::WrongRepositoryId,
        AttestationMutation::WrongOwnerId,
    ] {
        for attestation in 1..=3 {
            let expected_error = if mutation == AttestationMutation::WrongSource {
                "attestation certificate source, signer, or run identity differs"
            } else {
                "attestation certificate repository or owner ID differs"
            };
            let mut runner = MockGh::new(&fixture)?;
            runner.attestation_mutation = mutation;
            runner.attestation_mutation_at = attestation;
            let error = verify(&fixture, &mut runner)
                .err()
                .with_context(|| format!("accepted {mutation:?} in attestation {attestation}"))?;
            assert!(format!("{error:#}").contains(expected_error));
            assert_eq!(runner.command_count("attestation", None), attestation);

            let mut runner = MockGh::new(&fixture)?;
            runner.attestation_mutation = mutation;
            runner.attestation_mutation_at = attestation;
            let error = draft(&fixture, &mut runner, false).err().with_context(|| {
                format!("draft accepted {mutation:?} in attestation {attestation}")
            })?;
            assert!(format!("{error:#}").contains(expected_error));
            assert_eq!(runner.command_count("attestation", None), attestation);
            assert_eq!(runner.command_count("api", None), 0);
            assert_eq!(runner.release_write_count(), 0);
        }
    }
    Ok(())
}

#[test]
fn verify_rejects_wrong_attestation_run_signer_ref_subject_and_predicate() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    for mutation in [
        AttestationMutation::WrongRun,
        AttestationMutation::WrongSigner,
        AttestationMutation::WrongRef,
        AttestationMutation::WrongSubject,
        AttestationMutation::WrongPredicate,
    ] {
        let mut runner = MockGh::new(&fixture)?;
        runner.attestation_mutation = mutation;
        assert!(
            verify(&fixture, &mut runner).is_err(),
            "accepted {mutation:?}"
        );
    }
    Ok(())
}
