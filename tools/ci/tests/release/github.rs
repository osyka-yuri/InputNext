use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail, ensure};
use inputnext_ci::process::{CommandOutput, CommandSpec, Runner};
use serde_json::{Value, json};

use super::fixtures::{
    self, Fixture, REPOSITORY, REPOSITORY_ID, REPOSITORY_OWNER_ID, RUN_ATTEMPT, RUN_ID, SOURCE_SHA,
    VERSION,
};

const PROVENANCE_PREDICATE: &str = "https://slsa.dev/provenance/v1";
const SPDX_PREDICATE: &str = "https://spdx.dev/Document/v2.2";

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) enum AttestationMutation {
    #[default]
    None,
    WrongSource,
    WrongRepositoryId,
    WrongOwnerId,
    WrongRun,
    WrongSigner,
    WrongRef,
    WrongSubject,
    WrongPredicate,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) enum RemoteTagMutation {
    #[default]
    Exact,
    Moved,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) enum RemoteReleaseMutation {
    #[default]
    Exact,
    Published,
    WrongAssetDigest,
    MissingAssetDigest,
    DuplicateAsset,
    UnexpectedAsset,
    WrongIdentity,
}

#[derive(Debug)]
pub(super) struct MockGh {
    public: PathBuf,
    internal: PathBuf,
    pub(super) remote_assets: BTreeSet<String>,
    pub(super) remote_release_exists: bool,
    pub(super) release_lookup_failure: bool,
    pub(super) calls: Vec<Vec<String>>,
    pub(super) upload_failures_remaining: usize,
    pub(super) upload_failure_for: Option<String>,
    pub(super) attestation_mutation: AttestationMutation,
    pub(super) attestation_mutation_at: usize,
    pub(super) remote_tag_mutation: RemoteTagMutation,
    pub(super) remote_release_mutation: RemoteReleaseMutation,
}

impl MockGh {
    pub(super) fn new(fixture: &Fixture) -> Result<Self> {
        let public = fixture.release_output().join("public");
        let mut remote_assets = BTreeSet::new();
        for entry in fs::read_dir(&public)? {
            let entry = entry?;
            if entry.file_type()?.is_file() {
                remote_assets.insert(
                    entry
                        .file_name()
                        .into_string()
                        .map_err(|_| anyhow::anyhow!("fixture asset name is not UTF-8"))?,
                );
            }
        }
        Ok(Self {
            public,
            internal: fixture.release_output().join("internal"),
            remote_assets,
            remote_release_exists: true,
            release_lookup_failure: false,
            calls: Vec::new(),
            upload_failures_remaining: 0,
            upload_failure_for: None,
            attestation_mutation: AttestationMutation::None,
            attestation_mutation_at: 1,
            remote_tag_mutation: RemoteTagMutation::Exact,
            remote_release_mutation: RemoteReleaseMutation::Exact,
        })
    }

    pub(super) fn omit_asset(&mut self, name: &str) {
        self.remote_assets.remove(name);
    }

    pub(super) fn begin_without_release(&mut self) -> Result<String> {
        let last_asset = self
            .remote_assets
            .last()
            .cloned()
            .context("mock release has no assets")?;
        self.remote_release_exists = false;
        self.remote_assets.clear();
        Ok(last_asset)
    }

    pub(super) fn command_count(&self, first: &str, second: Option<&str>) -> usize {
        self.calls
            .iter()
            .filter(|args| {
                args.first().is_some_and(|value| value == first)
                    && second.is_none_or(|value| args.get(1).is_some_and(|arg| arg == value))
            })
            .count()
    }

    pub(super) fn release_write_count(&self) -> usize {
        self.command_count("release", Some("create"))
            + self.command_count("release", Some("upload"))
    }

    fn certificate_summary(mutation: AttestationMutation) -> Result<Value> {
        let source_commit = if mutation == AttestationMutation::WrongSource {
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        } else {
            SOURCE_SHA
        };
        let wrong_version = fixtures::different_version(VERSION)?;
        let source_ref = if mutation == AttestationMutation::WrongRef {
            format!("refs/tags/v{wrong_version}")
        } else {
            format!("refs/tags/v{VERSION}")
        };
        let signer = if mutation == AttestationMutation::WrongSigner {
            format!(
                "https://github.com/{REPOSITORY}/.github/workflows/other.yml@refs/tags/v{VERSION}"
            )
        } else {
            format!(
                "https://github.com/{REPOSITORY}/.github/workflows/release-build.yml@refs/tags/v{VERSION}"
            )
        };
        let run_attempt = if mutation == AttestationMutation::WrongRun {
            "3"
        } else {
            RUN_ATTEMPT
        };
        Ok(json!({
            "sourceRepositoryURI": format!("https://github.com/{REPOSITORY}"),
            "sourceRepositoryIdentifier": if mutation == AttestationMutation::WrongRepositoryId {
                (REPOSITORY_ID + 1).to_string()
            } else {
                REPOSITORY_ID.to_string()
            },
            "sourceRepositoryOwnerIdentifier": if mutation == AttestationMutation::WrongOwnerId {
                (REPOSITORY_OWNER_ID + 1).to_string()
            } else {
                REPOSITORY_OWNER_ID.to_string()
            },
            "sourceRepositoryDigest": source_commit,
            "sourceRepositoryRef": source_ref,
            "buildSignerURI": signer,
            "buildSignerDigest": SOURCE_SHA,
            "runInvocationURI": format!("https://github.com/{REPOSITORY}/actions/runs/{RUN_ID}/attempts/{run_attempt}")
        }))
    }

    fn attestation_output(&self, args: &[String]) -> Result<Vec<u8>> {
        let mutation = if self.command_count("attestation", None) == self.attestation_mutation_at {
            self.attestation_mutation
        } else {
            AttestationMutation::None
        };
        let predicate_type = arg_value(args, "--predicate-type")?;
        let subject_path = args
            .get(2)
            .context("attestation command has no subject path")?;
        let subject_name = Path::new(subject_path)
            .file_name()
            .and_then(|name| name.to_str())
            .context("attestation subject has no UTF-8 name")?;
        let mut subjects = if predicate_type == PROVENANCE_PREDICATE {
            let mut subjects = Vec::new();
            for entry in fs::read_dir(&self.public)? {
                let entry = entry?;
                let name = entry
                    .file_name()
                    .into_string()
                    .map_err(|_| anyhow::anyhow!("fixture asset name is not UTF-8"))?;
                if !name.ends_with(".sigstore.json") {
                    subjects.push(json!({
                        "name": name,
                        "digest": { "sha256": fixtures::sha256_file(&entry.path())? }
                    }));
                }
            }
            subjects
        } else {
            vec![json!({
                "name": subject_name,
                "digest": { "sha256": fixtures::sha256_file(Path::new(subject_path))? }
            })]
        };
        if mutation == AttestationMutation::WrongSubject
            && let Some(first) = subjects.first_mut()
        {
            first["name"] = json!("unexpected-subject.zip");
        }
        let statement_predicate = if mutation == AttestationMutation::WrongPredicate {
            "https://example.invalid/wrong-predicate"
        } else {
            predicate_type
        };
        let predicate = if predicate_type == SPDX_PREDICATE {
            let architecture = if subject_name.contains("-x64.") {
                "x64"
            } else {
                "x86"
            };
            let spdx = self.public.join(format!(
                "InputNext-{VERSION}-windows-{architecture}.spdx.json"
            ));
            serde_json::from_slice(&fs::read(spdx)?)?
        } else {
            json!({})
        };
        let statement = json!({
            "predicateType": statement_predicate,
            "subject": subjects,
            "predicate": predicate
        });
        serde_json::to_vec(&json!([{
            "verificationResult": {
                "signature": {
                    "certificate": Self::certificate_summary(mutation)?
                },
                "statement": statement
            }
        }]))
        .context("could not serialize mock attestation result")
    }

    fn api_output(&self, endpoint: &str) -> Result<(i32, Vec<u8>)> {
        if endpoint.contains("/git/ref/tags/") {
            let commit = if self.remote_tag_mutation == RemoteTagMutation::Moved {
                "cccccccccccccccccccccccccccccccccccccccc"
            } else {
                SOURCE_SHA
            };
            return response(
                200,
                &json!({
                    "object": { "type": "commit", "sha": commit }
                }),
            )
            .map(|body| (0, body));
        }
        let is_tag_lookup = endpoint.contains("/releases/tags/");
        let is_id_lookup = endpoint.ends_with("/releases/314");
        if is_tag_lookup || is_id_lookup {
            if !self.remote_release_exists
                || (is_tag_lookup
                    && self.remote_release_mutation != RemoteReleaseMutation::Published)
            {
                return response(404, &json!({ "message": "Not Found" })).map(|body| (1, body));
            }
            let notes = fs::read_to_string(self.internal.join("release-notes.md"))?;
            let mut assets = Vec::new();
            for name in &self.remote_assets {
                let digest = fixtures::sha256_file(&self.public.join(name))?;
                let asset = if self.remote_release_mutation
                    == RemoteReleaseMutation::MissingAssetDigest
                    && assets.is_empty()
                {
                    json!({ "name": name })
                } else {
                    json!({
                        "name": name,
                        "digest": if self.remote_release_mutation
                            == RemoteReleaseMutation::WrongAssetDigest
                            && assets.is_empty()
                        {
                            format!("sha256:{}", "0".repeat(64))
                        } else {
                            format!("sha256:{digest}")
                        }
                    })
                };
                assets.push(asset);
            }
            if self.remote_release_mutation == RemoteReleaseMutation::DuplicateAsset
                && let Some(first) = assets.first().cloned()
            {
                assets.push(first);
            }
            if self.remote_release_mutation == RemoteReleaseMutation::UnexpectedAsset {
                assets.push(json!({
                    "name": "unexpected.zip",
                    "digest": format!("sha256:{}", "0".repeat(64))
                }));
            }
            let wrong_version = fixtures::different_version(VERSION)?;
            let tag_name = if self.remote_release_mutation == RemoteReleaseMutation::WrongIdentity {
                format!("v{wrong_version}")
            } else {
                format!("v{VERSION}")
            };
            let release_name =
                if self.remote_release_mutation == RemoteReleaseMutation::WrongIdentity {
                    format!("InputNext v{wrong_version}")
                } else {
                    format!("InputNext v{VERSION}")
                };
            return response(
                200,
                &json!({
                    "draft": self.remote_release_mutation != RemoteReleaseMutation::Published,
                    "prerelease": false,
                    "tag_name": tag_name,
                    "name": release_name,
                    "body": if self.remote_release_mutation == RemoteReleaseMutation::WrongIdentity { "changed release notes" } else { &notes },
                    "assets": assets
                }),
            )
            .map(|body| (0, body));
        }
        bail!("unexpected mock GitHub API endpoint: {endpoint}")
    }
}

impl Runner for MockGh {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        let args = spec
            .args
            .iter()
            .map(|arg| arg.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        self.calls.push(args.clone());
        let (code, stdout, stderr) = if args.as_slice() == ["--version"] {
            (0, b"gh version 2.103.3 (mock)\n".to_vec(), Vec::new())
        } else if args.first().is_some_and(|arg| arg == "attestation") {
            (0, self.attestation_output(&args)?, Vec::new())
        } else if args.first().is_some_and(|arg| arg == "api") {
            let endpoint = args.last().context("mock API command has no endpoint")?;
            let (code, stdout) = self.api_output(endpoint)?;
            (code, stdout, Vec::new())
        } else if args.first().is_some_and(|arg| arg == "release")
            && args.get(1).is_some_and(|arg| arg == "view")
        {
            if self.release_lookup_failure {
                (
                    1,
                    Vec::new(),
                    b"HTTP 401: Requires authentication\n".to_vec(),
                )
            } else if self.remote_release_exists {
                (0, b"{\"databaseId\":314}\n".to_vec(), Vec::new())
            } else {
                (1, Vec::new(), b"error: release not found\n".to_vec())
            }
        } else if args.first().is_some_and(|arg| arg == "release")
            && args.get(1).is_some_and(|arg| arg == "create")
        {
            let expected_tag = format!("v{VERSION}");
            ensure!(
                args.get(2).is_some_and(|arg| arg == &expected_tag)
                    && args.iter().any(|arg| arg == "--draft")
                    && args.iter().any(|arg| arg == "--verify-tag")
                    && args.iter().any(|arg| arg == "--notes-file")
                    && args.iter().any(|arg| arg == "--title")
                    && args.iter().any(|arg| arg == "--repo"),
                "mock draft creation omitted a required option"
            );
            ensure!(
                !self.remote_release_exists,
                "mock attempted to create a release that already exists"
            );
            self.remote_release_exists = true;
            self.remote_assets.clear();
            (0, Vec::new(), Vec::new())
        } else if args.first().is_some_and(|arg| arg == "release")
            && args.get(1).is_some_and(|arg| arg == "upload")
        {
            let path = args
                .get(3)
                .context("mock upload command has no local path")?;
            let name = Path::new(path)
                .file_name()
                .and_then(|value| value.to_str())
                .context("mock upload filename is invalid")?;
            if self.upload_failure_for.as_deref() == Some(name) {
                self.upload_failure_for = None;
                (1, Vec::new(), Vec::new())
            } else if self.upload_failures_remaining > 0 {
                self.upload_failures_remaining -= 1;
                (1, Vec::new(), Vec::new())
            } else {
                self.remote_assets.insert(name.to_owned());
                (0, Vec::new(), Vec::new())
            }
        } else {
            bail!("unexpected mock GitHub command: {args:?}")
        };
        Ok(CommandOutput {
            code: Some(code),
            stdout,
            stderr,
        })
    }
}

fn response(status: u16, value: &Value) -> Result<Vec<u8>> {
    let body = serde_json::to_vec(value)?;
    let header = format!("HTTP/2.0 {status} Mock\r\nContent-Type: application/json\r\n\r\n");
    Ok([header.as_bytes(), &body].concat())
}

fn arg_value<'a>(args: &'a [String], flag: &str) -> Result<&'a str> {
    let index = args
        .iter()
        .position(|value| value == flag)
        .context("mock attestation command is missing an option")?;
    args.get(index + 1)
        .map(String::as_str)
        .context("mock attestation option is missing its value")
}
