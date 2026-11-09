//! Idempotent draft creation with remote tag, asset, and retry revalidation.

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsString;
use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, bail, ensure};
use serde_json::Value;

use crate::process::{CommandSpec, Runner};

use super::assets::{LocalRelease, validate_release_directory};
use super::attest::verify_inputs;
use super::identity::{QualifiedSource, canonical_directory, load_identity, validate_source};
use super::package::{SourcePins, source_pins};

const MAX_API_OUTPUT_BYTES: usize = 8 * 1024 * 1024;
const API_TIMEOUT: Duration = Duration::from_secs(60);
const RELEASE_TITLE_PREFIX: &str = "InputNext v";

#[derive(Debug)]
enum ApiResponse {
    Found(Value),
    NotFound,
}

#[derive(Debug)]
struct RemoteRelease {
    missing_assets: Vec<String>,
}

pub(super) fn run(args: super::DraftArgs, runner: &mut dyn Runner) -> Result<()> {
    let verified = verify_inputs(
        &args.identity,
        &args.source_root,
        &args.release_dir,
        &args.gh,
        runner,
    )?;
    let identity = load_identity(&args.identity)?;
    let source_root = canonical_directory(&args.source_root, "source root")?;
    let qualified = validate_source(identity, &source_root)?;
    let pins = source_pins(&source_root)?;
    let local = validate_release_directory(&verified.root, &qualified, &pins)?;
    let gh = args.gh;

    verify_remote_tag(runner, &gh, &source_root, &qualified)?;
    let current = read_release(runner, &gh, &source_root, &local, &qualified, true)?;
    let missing = current.as_ref().map_or_else(
        || local.signed_names.clone(),
        |remote| remote.missing_assets.clone(),
    );
    if args.dry_run {
        if current.is_none() {
            println!(
                "Would create draft {} with {} approved assets.",
                qualified.identity.tag(),
                missing.len()
            );
        } else if missing.is_empty() {
            println!(
                "Draft {} already has the exact approved asset set.",
                qualified.identity.tag()
            );
        } else {
            println!(
                "Would upload {} missing assets to draft {}.",
                missing.len(),
                qualified.identity.tag()
            );
        }
        return Ok(());
    }

    if current.is_none() {
        verify_remote_tag(runner, &gh, &source_root, &qualified)?;
        create_draft(runner, &gh, &source_root, &local, &qualified)?;
        verify_remote_tag(runner, &gh, &source_root, &qualified)?;
        read_release(runner, &gh, &source_root, &local, &qualified, false)?
            .context("GitHub did not return the newly created draft")?;
    }

    for asset_name in missing {
        revalidate_local_release(&local, &qualified, &pins)?;
        verify_remote_tag(runner, &gh, &source_root, &qualified)?;
        let remote = read_release(runner, &gh, &source_root, &local, &qualified, false)?
            .context("draft release disappeared before asset upload")?;
        if !remote.missing_assets.contains(&asset_name) {
            continue;
        }
        upload_asset(runner, &gh, &source_root, &local, &qualified, &asset_name)?;
        verify_remote_tag(runner, &gh, &source_root, &qualified)?;
        let after = read_release(runner, &gh, &source_root, &local, &qualified, false)?
            .context("draft release disappeared after asset upload")?;
        ensure!(
            !after.missing_assets.contains(&asset_name),
            "draft upload did not admit the expected asset: {asset_name}"
        );
    }

    revalidate_local_release(&local, &qualified, &pins)?;
    verify_remote_tag(runner, &gh, &source_root, &qualified)?;
    let final_release = read_release(runner, &gh, &source_root, &local, &qualified, false)?
        .context("draft release disappeared after upload")?;
    ensure!(
        final_release.missing_assets.is_empty(),
        "draft is missing one or more approved assets after upload"
    );
    println!(
        "Verified draft release {} with the exact approved assets.",
        qualified.identity.tag()
    );
    Ok(())
}

fn revalidate_local_release(
    release: &LocalRelease,
    qualified: &QualifiedSource,
    pins: &SourcePins,
) -> Result<()> {
    let current = validate_release_directory(&release.root, qualified, pins)?;
    ensure!(
        current.public_digests == release.public_digests
            && current.signed_names == release.signed_names
            && current.notes == release.notes,
        "release files changed after attestation verification"
    );
    Ok(())
}

fn verify_remote_tag(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    qualified: &QualifiedSource,
) -> Result<()> {
    let identity = &qualified.identity;
    let endpoint = format!(
        "repos/{}/git/ref/tags/{}",
        identity.repository(),
        identity.tag()
    );
    let ApiResponse::Found(reference) = gh_api(runner, gh, cwd, &endpoint, false)? else {
        bail!("the remote release tag no longer exists");
    };
    let mut object = reference
        .get("object")
        .cloned()
        .context("remote tag reference has no target object")?;
    let mut visited = BTreeSet::new();
    for _ in 0..32 {
        let object_type = object["type"]
            .as_str()
            .context("remote tag target type is missing")?;
        let sha = object["sha"]
            .as_str()
            .context("remote tag target SHA is missing")?;
        ensure!(
            is_lower_commit(sha),
            "remote tag target has a noncanonical Git SHA"
        );
        match object_type {
            "commit" => {
                ensure!(
                    sha == identity.source_commit(),
                    "remote release tag no longer resolves to the admitted source commit"
                );
                return Ok(());
            }
            "tag" => {
                ensure!(
                    visited.insert(sha.to_owned()),
                    "remote annotated tag contains a cycle"
                );
                let endpoint = format!("repos/{}/git/tags/{sha}", identity.repository());
                let ApiResponse::Found(tag) = gh_api(runner, gh, cwd, &endpoint, false)? else {
                    bail!("could not resolve remote annotated tag object");
                };
                object = tag
                    .get("object")
                    .cloned()
                    .context("remote annotated tag has no target object")?;
            }
            _ => bail!("remote release tag does not point to a commit"),
        }
    }
    bail!("remote annotated tag nesting exceeds the verification limit")
}

fn read_release(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    local: &LocalRelease,
    qualified: &QualifiedSource,
    allow_not_found: bool,
) -> Result<Option<RemoteRelease>> {
    let tag_endpoint = format!(
        "repos/{}/releases/tags/{}",
        qualified.identity.repository(),
        qualified.identity.tag()
    );
    if let ApiResponse::Found(value) = gh_api(runner, gh, cwd, &tag_endpoint, true)? {
        validate_remote_release_identity(&value, local, qualified)?;
        let missing_assets = validate_remote_assets(&value, local)?;
        return Ok(Some(RemoteRelease { missing_assets }));
    }

    let Some(release_id) = lookup_release_id(runner, gh, cwd, qualified)? else {
        if allow_not_found {
            return Ok(None);
        }
        bail!("the published or draft release no longer exists");
    };

    let endpoint = format!(
        "repos/{}/releases/{release_id}",
        qualified.identity.repository()
    );
    let value = match gh_api(runner, gh, cwd, &endpoint, allow_not_found)? {
        ApiResponse::Found(value) => value,
        ApiResponse::NotFound => return Ok(None),
    };
    validate_remote_release_identity(&value, local, qualified)?;
    let missing_assets = validate_remote_assets(&value, local)?;
    Ok(Some(RemoteRelease { missing_assets }))
}

fn lookup_release_id(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    qualified: &QualifiedSource,
) -> Result<Option<u64>> {
    let args = [
        OsString::from("release"),
        OsString::from("view"),
        OsString::from(qualified.identity.tag()),
        OsString::from("--json"),
        OsString::from("databaseId"),
        OsString::from("--repo"),
        OsString::from(qualified.identity.repository()),
    ];
    let outcome = runner.run(&command_spec(gh, cwd, args, API_TIMEOUT))?;
    if outcome.code != Some(0) {
        let stderr = std::str::from_utf8(&outcome.stderr)
            .context("GitHub CLI release lookup error is not UTF-8")?;
        let diagnostic = stderr.trim();
        let diagnostic = diagnostic
            .strip_prefix("error: ")
            .or_else(|| diagnostic.strip_prefix("gh: "))
            .unwrap_or(diagnostic);
        if diagnostic == "release not found" {
            return Ok(None);
        }
        bail!("GitHub CLI could not look up the release: {diagnostic}");
    }

    let value = super::strict_json::parse_slice(&outcome.stdout)
        .context("GitHub CLI release lookup returned invalid JSON")?;
    ensure!(
        value.as_object().is_some_and(|fields| fields.len() == 1),
        "GitHub CLI release lookup returned unexpected fields"
    );
    let release_id = value["databaseId"]
        .as_u64()
        .filter(|value| *value > 0)
        .context("GitHub CLI release lookup omitted a valid database ID")?;
    Ok(Some(release_id))
}

fn validate_remote_release_identity(
    release: &Value,
    local: &LocalRelease,
    qualified: &QualifiedSource,
) -> Result<()> {
    let title = format!("{RELEASE_TITLE_PREFIX}{}", qualified.identity.version());
    let body = release["body"].as_str().unwrap_or_default();
    ensure!(
        release["draft"].as_bool() == Some(true),
        "a published release exists; refusing to mutate it"
    );
    ensure!(
        release["prerelease"].as_bool() == Some(false)
            && release["tag_name"].as_str() == Some(qualified.identity.tag())
            && release["name"].as_str() == Some(title.as_str())
            && trim_terminal_newlines(body) == trim_terminal_newlines(&local.notes),
        "existing draft tag, title, or notes differ from the admitted release"
    );
    Ok(())
}

fn validate_remote_assets(release: &Value, local: &LocalRelease) -> Result<Vec<String>> {
    let assets = release["assets"]
        .as_array()
        .context("GitHub release response has no asset inventory")?;
    let mut seen = BTreeMap::new();
    for asset in assets {
        let name = asset["name"]
            .as_str()
            .context("GitHub release asset name is missing")?;
        let expected = local
            .public_digests
            .get(name)
            .with_context(|| format!("unexpected release asset: {name}"))?;
        ensure!(
            seen.insert(name.to_owned(), expected.clone()).is_none(),
            "GitHub release contains a duplicate asset: {name}"
        );
        let digest = asset["digest"]
            .as_str()
            .context("GitHub release asset SHA-256 is unavailable")?;
        ensure!(
            digest.strip_prefix("sha256:") == Some(expected.as_str()),
            "existing draft asset digest differs from the approved bytes: {name}"
        );
    }
    Ok(local
        .signed_names
        .iter()
        .filter(|name| !seen.contains_key(*name))
        .cloned()
        .collect())
}

fn create_draft(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    local: &LocalRelease,
    qualified: &QualifiedSource,
) -> Result<()> {
    let args = vec![
        OsString::from("release"),
        OsString::from("create"),
        OsString::from(qualified.identity.tag()),
        OsString::from("--draft"),
        OsString::from("--verify-tag"),
        OsString::from("--title"),
        OsString::from(format!(
            "{RELEASE_TITLE_PREFIX}{}",
            qualified.identity.version()
        )),
        OsString::from("--notes-file"),
        local.notes_path.as_os_str().to_os_string(),
        OsString::from("--repo"),
        OsString::from(qualified.identity.repository()),
    ];
    let outcome = runner.run(&command_spec(gh, cwd, args, API_TIMEOUT))?;
    if outcome.code == Some(0) {
        return Ok(());
    }
    let appeared = read_release(runner, gh, cwd, local, qualified, true)?;
    ensure!(
        appeared.is_some(),
        "GitHub CLI could not create the draft release"
    );
    Ok(())
}

fn upload_asset(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    local: &LocalRelease,
    qualified: &QualifiedSource,
    asset_name: &str,
) -> Result<()> {
    ensure!(
        local.signed_names.iter().any(|name| name == asset_name),
        "release writer was asked to upload an unapproved asset"
    );
    let path = local.public.join(asset_name);
    let args = vec![
        OsString::from("release"),
        OsString::from("upload"),
        OsString::from(qualified.identity.tag()),
        path.as_os_str().to_os_string(),
        OsString::from("--repo"),
        OsString::from(qualified.identity.repository()),
    ];
    let outcome = runner.run(&command_spec(gh, cwd, args, API_TIMEOUT))?;
    if outcome.code == Some(0) {
        return Ok(());
    }
    let remote = read_release(runner, gh, cwd, local, qualified, false)?
        .context("draft release disappeared after a failed upload")?;
    ensure!(
        !remote.missing_assets.iter().any(|name| name == asset_name),
        "GitHub CLI upload failed before the expected asset was admitted: {asset_name}"
    );
    Ok(())
}

fn gh_api(
    runner: &mut dyn Runner,
    gh: &Path,
    cwd: &Path,
    endpoint: &str,
    allow_not_found: bool,
) -> Result<ApiResponse> {
    let args = vec![
        OsString::from("api"),
        OsString::from("--include"),
        OsString::from("--hostname"),
        OsString::from("github.com"),
        OsString::from("-H"),
        OsString::from("Accept: application/vnd.github+json"),
        OsString::from("-H"),
        OsString::from("X-GitHub-Api-Version: 2022-11-28"),
        OsString::from(endpoint),
    ];
    let outcome = runner.run(&command_spec(gh, cwd, args, API_TIMEOUT))?;
    let response = parse_included_response(&outcome.stdout)?;
    if response.0 == 404 && allow_not_found {
        ensure!(
            outcome.code.is_some_and(|code| code != 0),
            "GitHub API returned an unexpected successful 404 response"
        );
        return Ok(ApiResponse::NotFound);
    }
    ensure!(
        response.0 == 200 && outcome.code == Some(0),
        "GitHub API read returned an unexpected HTTP or process status"
    );
    let value = super::strict_json::parse_slice(response.1)
        .context("GitHub API response is invalid JSON")?;
    Ok(ApiResponse::Found(value))
}

fn parse_included_response(bytes: &[u8]) -> Result<(u16, &[u8])> {
    let text = std::str::from_utf8(bytes).context("GitHub API response is not UTF-8")?;
    let (headers, body) = text
        .split_once("\r\n\r\n")
        .or_else(|| text.split_once("\n\n"))
        .context("GitHub API response has no HTTP header/body boundary")?;
    let status_line = headers
        .lines()
        .next()
        .context("GitHub API response has no status line")?;
    let status = status_line
        .split_whitespace()
        .nth(1)
        .context("GitHub API response status is malformed")?
        .parse::<u16>()
        .context("GitHub API response status is malformed")?;
    Ok((status, body.as_bytes()))
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
        output_limit: MAX_API_OUTPUT_BYTES,
    }
}

fn trim_terminal_newlines(value: &str) -> &str {
    value.trim_end_matches(['\r', '\n'])
}

fn is_lower_commit(value: &str) -> bool {
    value.len() == 40
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests {
    use super::{parse_included_response, trim_terminal_newlines};

    #[test]
    fn parses_http_status_included_by_gh_api() -> Result<(), Box<dyn std::error::Error>> {
        let (status, body) = parse_included_response(
            b"HTTP/2.0 404 Not Found\r\nContent-Type: application/json\r\n\r\n{\"message\":\"Not Found\"}",
        )?;
        assert_eq!(status, 404);
        assert_eq!(body, br#"{"message":"Not Found"}"#);
        Ok(())
    }

    #[test]
    fn compares_release_notes_without_terminal_line_endings() {
        assert_eq!(trim_terminal_newlines("notes\r\n"), "notes");
        assert_ne!(trim_terminal_newlines("notes changed"), "notes");
    }
}
