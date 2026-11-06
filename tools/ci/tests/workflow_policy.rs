use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use inputnext_ci::workflows::check_repository;
use serde_yaml_ng::Value;
use tempfile::TempDir;

fn repository() -> Result<PathBuf> {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()
        .context("resolve repository")
}

fn fixture() -> Result<TempDir> {
    let temporary = tempfile::tempdir()?;
    let source = repository()?.join(".github/workflows");
    let destination = temporary.path().join(".github/workflows");
    fs::create_dir_all(&destination)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        if entry.path().is_file() {
            fs::copy(entry.path(), destination.join(entry.file_name()))?;
        }
    }
    Ok(temporary)
}

fn mutate(root: &Path, name: &str, edit: impl FnOnce(&mut Value)) -> Result<()> {
    let path = root.join(".github/workflows").join(name);
    let mut document: Value = serde_yaml_ng::from_str(&fs::read_to_string(&path)?)?;
    edit(&mut document);
    fs::write(path, serde_yaml_ng::to_string(&document)?)?;
    Ok(())
}

#[test]
fn ci_uses_pull_requests_and_main_pushes_without_duplicate_feature_branch_runs() -> Result<()> {
    let path = repository()?.join(".github/workflows/ci.yml");
    let workflow: Value = serde_yaml_ng::from_str(&fs::read_to_string(path)?)?;
    let events = workflow["on"].as_mapping().context("CI events missing")?;
    for name in ["pull_request", "workflow_dispatch", "schedule"] {
        assert!(
            events.contains_key(Value::from(name)),
            "missing {name} event"
        );
    }
    assert_eq!(
        workflow["on"]["push"]["branches"],
        Value::from(vec!["main"])
    );
    Ok(())
}

#[test]
fn stress_failures_preserve_native_verification_logs() -> Result<()> {
    let path = repository()?.join(".github/workflows/quality.yml");
    let workflow: Value = serde_yaml_ng::from_str(&fs::read_to_string(path)?)?;
    let steps = workflow["jobs"]["stress"]["steps"]
        .as_sequence()
        .context("stress job has no steps")?;
    let upload = steps
        .iter()
        .find(|step| {
            step["uses"]
                .as_str()
                .is_some_and(|action| action.starts_with("actions/upload-artifact@"))
        })
        .context("stress job has no diagnostic upload")?;
    assert_eq!(upload["if"].as_str(), Some("always()"));
    let paths = upload["with"]["path"]
        .as_str()
        .context("stress upload has no paths")?;
    assert!(
        paths
            .lines()
            .any(|path| path == "build/windows-*/verification/")
    );
    Ok(())
}

#[test]
fn missing_workflow_is_an_error() -> Result<()> {
    let fixture = fixture()?;
    fs::remove_file(fixture.path().join(".github/workflows/ci.yml"))?;
    assert!(check_repository(fixture.path()).is_err());
    Ok(())
}

#[test]
fn floating_actions_and_retained_checkout_credentials_are_rejected() -> Result<()> {
    for retained in [false, true] {
        let fixture = fixture()?;
        mutate(fixture.path(), "quality.yml", |document| {
            let checkout = &mut document["jobs"]["tooling"]["steps"][0];
            if retained {
                checkout["with"]["persist-credentials"] = Value::Bool(true);
            } else {
                checkout["uses"] = Value::from("actions/checkout@v7");
            }
        })?;
        assert!(check_repository(fixture.path()).is_err());
    }
    Ok(())
}

#[test]
fn ordinary_jobs_cannot_gain_release_authority() -> Result<()> {
    for permission in ["contents", "id-token", "attestations", "actions"] {
        let fixture = fixture()?;
        mutate(fixture.path(), "quality.yml", |document| {
            document["jobs"]["tooling"]["permissions"][permission] = Value::from("write");
        })?;
        assert!(check_repository(fixture.path()).is_err());
    }
    let fixture = fixture()?;
    mutate(fixture.path(), "quality.yml", |document| {
        document["permissions"]["id-token"] = Value::from("write");
    })?;
    assert!(check_repository(fixture.path()).is_err());
    Ok(())
}
