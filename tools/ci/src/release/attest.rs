//! GitHub CLI cryptographic verification plus exact statement admission.

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use serde_json::Value;

use crate::process::{CommandSpec, Runner, run_checked};

use super::assets::{
    LocalRelease, provenance_bundle_name, sbom_bundle_name, sbom_name, validate_release_directory,
};
use super::identity::{
    ReleaseIdentity, canonical_directory, load_identity, read_json, validate_source,
};
use super::package::{regular_file, source_pins};

const EXPECTED_GH_VERSION: &str = "2.103.3";
const MAX_GH_OUTPUT_BYTES: usize = 32 * 1024 * 1024;
const COMMAND_TIMEOUT: Duration = Duration::from_secs(120);
const SLSA_PROVENANCE_PREDICATE: &str = "https://slsa.dev/provenance/v1";
const SPDX_PREDICATE: &str = "https://spdx.dev/Document/v2.2";

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ReleaseTrust {
    schema_version: u8,
    repository: String,
    repository_id: u64,
    repository_owner_id: u64,
}

impl ReleaseTrust {
    fn load(source_root: &Path, identity: &ReleaseIdentity) -> Result<Self> {
        let path = source_root.join(".github/ci/release-trust.json");
        let value = read_json(&path, 16 * 1024)?;
        let trust: Self =
            serde_json::from_value(value).context("release trust policy is invalid")?;
        trust.validate(identity)?;
        Ok(trust)
    }

    fn validate(&self, identity: &ReleaseIdentity) -> Result<()> {
        ensure!(
            self.schema_version == 1,
            "unsupported release trust policy schema"
        );
        ensure!(
            self.repository == identity.repository(),
            "release trust policy repository differs from qualified identity"
        );
        ensure!(
            self.repository_id != 0 && self.repository_owner_id != 0,
            "release trust policy repository and owner IDs must be nonzero"
        );
        Ok(())
    }
}

pub(super) fn verify(args: &super::VerifyArgs, runner: &mut dyn Runner) -> Result<()> {
    verify_inputs(
        &args.identity,
        &args.source_root,
        &args.release_dir,
        &args.gh,
        runner,
    )
    .map(|_| ())
}

pub(super) fn verify_inputs(
    identity_path: &Path,
    source_root: &Path,
    release_dir: &Path,
    gh: &Path,
    runner: &mut dyn Runner,
) -> Result<LocalRelease> {
    let identity = load_identity(identity_path)?;
    let source_root = canonical_directory(source_root, "source root")?;
    let qualified = validate_source(identity, &source_root)?;
    let trust = ReleaseTrust::load(&source_root, &qualified.identity)?;
    let pins = source_pins(&source_root)?;
    let release = validate_release_directory(release_dir, &qualified, &pins)?;
    let gh = validated_gh(gh)?;
    validate_gh_version(runner, &gh, &source_root)?;
    verify_provenance(
        runner,
        &gh,
        &source_root,
        &release,
        &qualified.identity,
        &trust,
    )?;
    verify_runtime_sboms(
        runner,
        &gh,
        &source_root,
        &release,
        &qualified.identity,
        &trust,
    )?;
    Ok(release)
}

fn validated_gh(path: &Path) -> Result<PathBuf> {
    ensure!(path.is_absolute(), "GitHub CLI path must be absolute");
    regular_file(path, "GitHub CLI")
}

fn validate_gh_version(runner: &mut dyn Runner, gh: &Path, cwd: &Path) -> Result<()> {
    let output = run_checked(
        runner,
        &command_spec(gh, cwd, ["--version"], Duration::from_secs(30)),
    )?;
    let text =
        std::str::from_utf8(&output.stdout).context("GitHub CLI version output is not UTF-8")?;
    let first_line = text
        .lines()
        .next()
        .context("GitHub CLI version output is empty")?;
    ensure!(
        first_line.split_whitespace().take(3).collect::<Vec<_>>()
            == ["gh", "version", EXPECTED_GH_VERSION],
        "GitHub CLI version must be exactly {EXPECTED_GH_VERSION}"
    );
    Ok(())
}

fn verify_provenance(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    release: &LocalRelease,
    identity: &ReleaseIdentity,
    trust: &ReleaseTrust,
) -> Result<()> {
    let subject_name = format!("InputNext-{}-windows-x64.zip", identity.version());
    let subject_path = release.public.join(&subject_name);
    let bundle_path = release.public.join(provenance_bundle_name(identity));
    let output = verify_attestation(
        runner,
        gh,
        cwd,
        identity,
        &subject_path,
        &bundle_path,
        SLSA_PROVENANCE_PREDICATE,
    )?;
    let (verification, statement) = exact_verified_statement(&output)?;
    ensure!(
        statement["predicateType"].as_str() == Some(SLSA_PROVENANCE_PREDICATE),
        "verified provenance predicate type is incorrect"
    );
    let expected = release
        .public_digests
        .iter()
        .filter(|(name, _)| !name.ends_with(".sigstore.json"))
        .map(|(name, digest)| (name.clone(), digest.clone()))
        .collect::<BTreeMap<_, _>>();
    validate_subjects(&statement["subject"], &expected)?;
    validate_certificate(verification, identity, trust)?;
    Ok(())
}

fn verify_runtime_sboms(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    release: &LocalRelease,
    identity: &ReleaseIdentity,
    trust: &ReleaseTrust,
) -> Result<()> {
    for architecture in ["x64", "x86"] {
        let zip_name = format!(
            "InputNext-{}-windows-{architecture}.zip",
            identity.version()
        );
        let zip_path = release.public.join(&zip_name);
        let bundle_path = release
            .public
            .join(sbom_bundle_name(identity, architecture));
        let output = verify_attestation(
            runner,
            gh,
            cwd,
            identity,
            &zip_path,
            &bundle_path,
            SPDX_PREDICATE,
        )?;
        let (verification, statement) = exact_verified_statement(&output)?;
        ensure!(
            statement["predicateType"].as_str() == Some(SPDX_PREDICATE),
            "verified runtime SBOM predicate type is incorrect for {architecture}"
        );
        let expected_digest = release
            .public_digests
            .get(&zip_name)
            .context("runtime ZIP digest is missing")?;
        validate_subjects(
            &statement["subject"],
            &BTreeMap::from([(zip_name.clone(), expected_digest.clone())]),
        )?;
        let spdx_path = release.public.join(sbom_name(identity, architecture));
        let spdx = read_json(&spdx_path, 8 * 1024 * 1024)?;
        ensure!(
            statement.get("predicate") == Some(&spdx),
            "verified SPDX predicate differs from the shipped {architecture} document"
        );
        validate_certificate(verification, identity, trust)?;
        ensure!(
            statement["predicate"]["spdxVersion"].as_str() == Some("SPDX-2.2"),
            "verified SPDX statement does not carry an SPDX 2.2 document"
        );
    }
    Ok(())
}

fn verify_attestation(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    identity: &ReleaseIdentity,
    subject: &Path,
    bundle: &Path,
    predicate_type: &str,
) -> Result<Value> {
    let signer_workflow = format!(
        "{}/.github/workflows/release-build.yml",
        identity.repository()
    );
    let args = vec![
        OsString::from("attestation"),
        OsString::from("verify"),
        subject.as_os_str().to_os_string(),
        OsString::from("--repo"),
        OsString::from(identity.repository()),
        OsString::from("--bundle"),
        bundle.as_os_str().to_os_string(),
        OsString::from("--predicate-type"),
        OsString::from(predicate_type),
        OsString::from("--signer-workflow"),
        OsString::from(signer_workflow),
        OsString::from("--signer-digest"),
        OsString::from(identity.workflow_commit()),
        OsString::from("--source-digest"),
        OsString::from(identity.source_commit()),
        OsString::from("--source-ref"),
        OsString::from(identity.source_ref()),
        OsString::from("--deny-self-hosted-runners"),
        OsString::from("--format"),
        OsString::from("json"),
    ];
    let output = run_checked(runner, &command_spec(gh, cwd, args, COMMAND_TIMEOUT))?;
    super::strict_json::parse_slice(&output.stdout)
        .context("GitHub CLI attestation output is invalid JSON")
}

fn exact_verified_statement(output: &Value) -> Result<(&Value, &Value)> {
    let results = if let Some(values) = output.as_array() {
        values.as_slice()
    } else {
        std::slice::from_ref(output)
    };
    ensure!(
        results.len() == 1,
        "attestation verification must return exactly one statement"
    );
    let verification = &results[0]["verificationResult"];
    ensure!(
        verification["signature"]["certificate"].is_object(),
        "attestation output has no verified signing certificate"
    );
    let statement = verification
        .get("statement")
        .context("attestation output has no verified statement")?;
    ensure!(statement.is_object(), "verified statement is not an object");
    Ok((verification, statement))
}

fn validate_subjects(subject_value: &Value, expected: &BTreeMap<String, String>) -> Result<()> {
    let subjects = subject_value
        .as_array()
        .context("verified statement has no subject array")?;
    ensure!(
        subjects.len() == expected.len(),
        "verified attestation subject count is not exact"
    );
    let mut actual = BTreeMap::new();
    for subject in subjects {
        let name = subject["name"]
            .as_str()
            .context("verified subject name is missing")?;
        let digest = subject["digest"]
            .as_object()
            .context("verified subject digest is missing")?;
        ensure!(
            digest.len() == 1,
            "verified subject digest has unexpected algorithms"
        );
        let sha256 = digest["sha256"]
            .as_str()
            .context("verified subject SHA-256 is missing")?;
        ensure!(
            actual.insert(name.to_owned(), sha256.to_owned()).is_none(),
            "verified attestation has duplicate subjects"
        );
    }
    ensure!(
        actual == *expected,
        "verified attestation subjects do not exactly match the admitted assets"
    );
    Ok(())
}

fn validate_certificate(
    verification: &Value,
    identity: &ReleaseIdentity,
    trust: &ReleaseTrust,
) -> Result<()> {
    let certificate = verification["signature"]["certificate"]
        .as_object()
        .context("verified attestation certificate is missing")?;
    let repo_uri = format!("https://github.com/{}", identity.repository());
    let signer = format!(
        "https://github.com/{}/.github/workflows/release-build.yml@{}",
        identity.repository(),
        identity.source_ref()
    );
    let run_uri = format!(
        "https://github.com/{}/actions/runs/{}/attempts/{}",
        identity.repository(),
        identity.run_id(),
        identity.run_attempt()
    );
    ensure!(
        certificate
            .get("sourceRepositoryURI")
            .and_then(Value::as_str)
            == Some(repo_uri.as_str())
            && certificate
                .get("sourceRepositoryDigest")
                .and_then(Value::as_str)
                == Some(identity.source_commit())
            && certificate
                .get("sourceRepositoryRef")
                .and_then(Value::as_str)
                == Some(identity.source_ref())
            && certificate.get("buildSignerURI").and_then(Value::as_str) == Some(signer.as_str())
            && certificate.get("buildSignerDigest").and_then(Value::as_str)
                == Some(identity.workflow_commit())
            && certificate.get("runInvocationURI").and_then(Value::as_str)
                == Some(run_uri.as_str()),
        "attestation certificate source, signer, or run identity differs from workflow context"
    );
    let repository_id = trust.repository_id.to_string();
    let owner_id = trust.repository_owner_id.to_string();
    ensure!(
        certificate
            .get("sourceRepositoryIdentifier")
            .and_then(Value::as_str)
            == Some(repository_id.as_str())
            && certificate
                .get("sourceRepositoryOwnerIdentifier")
                .and_then(Value::as_str)
                == Some(owner_id.as_str()),
        "attestation certificate repository or owner ID differs from trusted policy"
    );
    Ok(())
}

fn command_spec<I, S>(program: &Path, cwd: &Path, args: I, timeout: Duration) -> CommandSpec
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    CommandSpec {
        program: program.to_path_buf(),
        args: args.into_iter().map(Into::into).collect(),
        cwd: cwd.to_path_buf(),
        env: BTreeMap::default(),
        timeout,
        output_limit: MAX_GH_OUTPUT_BYTES,
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use anyhow::{Context, Result};
    use serde_json::{Value, json};

    use crate::release::identity::ReleaseIdentity;

    use super::{
        ReleaseTrust, SLSA_PROVENANCE_PREDICATE, exact_verified_statement, validate_certificate,
        validate_subjects,
    };

    fn captured_provenance() -> Result<(Value, ReleaseIdentity, ReleaseTrust)> {
        // Complete GitHub CLI 2.103.3 output from a historical repository instance.
        let output = serde_json::from_str(include_str!(
            "../../tests/release/gh-2.103.3-provenance.json"
        ))?;
        let identity: ReleaseIdentity = serde_json::from_value(json!({
            "schemaVersion": 1,
            "repository": "osyka-yuri/InputNext",
            "sourceCommit": "81dccfa03a45b32763fd0f9769e9836df610cf28",
            "sourceRef": "refs/tags/v0.1.0",
            "tag": "v0.1.0",
            "version": "0.1.0",
            "runId": "42061328129",
            "runAttempt": "1",
            "workflowCommit": "81dccfa03a45b32763fd0f9769e9836df610cf28"
        }))?;
        identity.validate()?;
        let trust: ReleaseTrust = serde_json::from_value(json!({
            "schemaVersion": 1,
            "repository": "osyka-yuri/InputNext",
            "repositoryId": 1_447_205_594,
            "repositoryOwnerId": 51_163_508
        }))?;
        trust.validate(&identity)?;
        Ok((output, identity, trust))
    }

    #[test]
    fn accepts_captured_pinned_github_cli_verification_result() -> Result<()> {
        let (output, identity, trust) = captured_provenance()?;
        let (verification, statement) = exact_verified_statement(&output)?;
        validate_certificate(verification, &identity, &trust)?;
        assert_eq!(statement["predicateType"], SLSA_PROVENANCE_PREDICATE);
        // Expected digests were independently computed from the downloaded files.
        let expected = BTreeMap::from(
            [
                (
                    "InputNext-0.1.0-windows-x64.zip",
                    "5a41e8f9b035947d3678a70245d7b8f8ae47a7a45f91c960a8ee7a5c485d3299",
                ),
                (
                    "InputNext-0.1.0-windows-x64-symbols.zip",
                    "70980a30f32b92f24b9f99268b8a1b01a48bcf7802646810aad54a834c7eb225",
                ),
                (
                    "InputNext-0.1.0-windows-x86.zip",
                    "2a36c0aec98545e488dff5cb8743ef8742cf54c42ae70abe73f7d589439424c2",
                ),
                (
                    "InputNext-0.1.0-windows-x86-symbols.zip",
                    "61c8a705441bba628dd21e48c7afd1d8e6ed004f826ca84a6c09b3e50f1bbc77",
                ),
                (
                    "InputNext-0.1.0-windows-x64.spdx.json",
                    "4639a7c07b334266b66f384e0711f2a7b877107cce507d8aeb0ffa02ace03af0",
                ),
                (
                    "InputNext-0.1.0-windows-x86.spdx.json",
                    "dabed62fa0fc5860b68d52222838a6e847c6fb02d246d111fc8dc6b37d634378",
                ),
                (
                    "SHA256SUMS",
                    "b1ab227ae4f65b6324ebce2b283ae967d17c82bf1d87633533baea9f7c025168",
                ),
            ]
            .map(|(name, digest)| (name.to_owned(), digest.to_owned())),
        );
        validate_subjects(&statement["subject"], &expected)?;
        let current_trust: ReleaseTrust =
            serde_json::from_str(include_str!("../../../../.github/ci/release-trust.json"))?;
        current_trust.validate(&identity)?;
        assert!(validate_certificate(verification, &identity, &current_trust).is_err());
        Ok(())
    }

    #[test]
    fn captured_certificate_rejects_invalid_or_missing_identity_fields() -> Result<()> {
        let (output, identity, trust) = captured_provenance()?;
        let (verification, _) = exact_verified_statement(&output)?;
        for field in [
            "sourceRepositoryURI",
            "sourceRepositoryIdentifier",
            "sourceRepositoryOwnerIdentifier",
            "sourceRepositoryDigest",
            "sourceRepositoryRef",
            "buildSignerURI",
            "buildSignerDigest",
            "runInvocationURI",
        ] {
            for invalid in [
                None,
                Some(Value::Null),
                Some(json!(42)),
                Some(json!("wrong")),
                Some(json!({
                    "sha1": verification["signature"]["certificate"][field]
                })),
            ] {
                let mut changed = verification.clone();
                let certificate = changed["signature"]["certificate"]
                    .as_object_mut()
                    .context("captured certificate is missing")?;
                if let Some(value) = invalid {
                    certificate.insert(field.to_owned(), value);
                } else {
                    certificate.remove(field);
                }
                assert!(
                    validate_certificate(&changed, &identity, &trust).is_err(),
                    "accepted invalid or missing {field}"
                );
            }
        }
        Ok(())
    }

    #[test]
    fn rejects_certificates_outside_the_verified_summary() -> Result<()> {
        let (output, identity, trust) = captured_provenance()?;
        let certificate = output[0]["verificationResult"]["signature"]["certificate"].clone();
        for invalid in [Value::Null, json!([]), json!("invalid")] {
            let mut changed = output.clone();
            changed[0]["attestation"] = json!({
                "signature": { "certificate": certificate }
            });
            changed[0]["verificationResult"]["statement"]["predicate"]["certificate"] =
                certificate.clone();
            changed[0]["verificationResult"]["signature"]["certificate"] = invalid;
            assert!(exact_verified_statement(&changed).is_err());
        }

        let mut nested = output;
        nested[0]["verificationResult"]["signature"]["certificate"] = json!({
            "extensions": certificate
        });
        assert!(validate_certificate(&nested[0]["verificationResult"], &identity, &trust).is_err());
        Ok(())
    }

    #[test]
    fn subject_admission_rejects_duplicate_or_extra_entries() {
        let expected = BTreeMap::from([("artifact.zip".to_owned(), "a".repeat(64))]);
        let duplicate = json!([
            {"name": "artifact.zip", "digest": {"sha256": "a".repeat(64)}},
            {"name": "artifact.zip", "digest": {"sha256": "a".repeat(64)}}
        ]);
        assert!(validate_subjects(&duplicate, &expected).is_err());
        let extra_algorithm = json!([
            {"name": "artifact.zip", "digest": {"sha256": "a".repeat(64), "sha1": "b".repeat(40)}}
        ]);
        assert!(validate_subjects(&extra_algorithm, &expected).is_err());
    }
}
