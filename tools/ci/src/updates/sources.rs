//! Official metadata adapters for supported upstream tool sources.

mod github;

use std::collections::BTreeMap;
use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use regex::Regex;
use semver::Version;
use serde_json::Value;
use tempfile::NamedTempFile;

use crate::curl;
use crate::process::{CommandSpec, Runner, run_checked};

const QUERY_TIMEOUT: Duration = Duration::from_secs(60);
const QUERY_OUTPUT_LIMIT: usize = 256 * 1024;
const MAX_METADATA_BYTES: u64 = 8 * 1024 * 1024;

#[derive(Clone, Debug)]
pub(super) struct UpstreamVersion {
    pub version: String,
    pub url: String,
}

pub(super) fn github_release(
    runner: &mut dyn Runner,
    fixtures: Option<&Path>,
    name: &str,
    repository: &str,
) -> Result<UpstreamVersion> {
    let bytes = if let Some(directory) = fixtures {
        read_fixture(directory, &format!("{name}.json"))?
    } else {
        fetch_official(
            runner,
            &format!("https://api.github.com/repos/{repository}/releases/latest"),
            true,
        )?
    };
    let release: Value = serde_json::from_slice(&bytes)
        .with_context(|| format!("upstream release metadata for '{name}' is not valid JSON"))?;
    ensure!(
        release.get("draft").and_then(Value::as_bool) == Some(false),
        "upstream release metadata for '{name}' is not a stable release"
    );
    ensure!(
        release.get("prerelease").and_then(Value::as_bool) == Some(false),
        "upstream release metadata for '{name}' is a prerelease"
    );
    let tag = release
        .get("tag_name")
        .and_then(Value::as_str)
        .with_context(|| format!("upstream release metadata for '{name}' has no tag"))?;
    let version = normalize_tag(tag)?;
    let url = release
        .get("html_url")
        .and_then(Value::as_str)
        .with_context(|| format!("upstream release metadata for '{name}' has no URL"))?;
    let expected_prefix = format!("https://github.com/{repository}/releases/");
    ensure!(
        url.starts_with(&expected_prefix),
        "upstream release URL for '{name}' is outside its official repository"
    );
    Ok(UpstreamVersion {
        version,
        url: url.to_owned(),
    })
}

pub(super) fn github_bundle_release(
    runner: &mut dyn Runner,
    fixtures: Option<&Path>,
) -> Result<UpstreamVersion> {
    let releases: Vec<github::Release> = if let Some(directory) = fixtures {
        serde_json::from_slice(&read_fixture(directory, "codeqlBundle.json")?)
            .context("upstream CodeQL release metadata is not valid JSON")?
    } else {
        github::codeql_releases(runner)?
    };
    let mut candidates = Vec::new();
    for release in releases {
        if release.draft != Some(false) || release.prerelease != Some(false) {
            continue;
        }
        let Some(tag) = release.tag_name.as_deref() else {
            continue;
        };
        if !tag.starts_with("codeql-bundle-v") {
            continue;
        }
        let version = normalize_tag(tag)?;
        let url = release
            .html_url
            .as_deref()
            .context("CodeQL bundle release has no URL")?;
        ensure!(
            url.starts_with("https://github.com/github/codeql-action/releases/"),
            "CodeQL bundle release URL is outside its official repository"
        );
        candidates.push((Version::parse(&version)?, version, url.to_owned()));
    }
    candidates.sort_by(|left, right| left.0.cmp(&right.0));
    let (_, version, url) = candidates
        .pop()
        .context("GitHub releases contained no stable CodeQL bundle")?;
    Ok(UpstreamVersion { version, url })
}

pub(super) fn gallery_latest(
    runner: &mut dyn Runner,
    fixtures: Option<&Path>,
) -> Result<UpstreamVersion> {
    let bytes = if let Some(directory) = fixtures {
        read_fixture(directory, "psscriptanalyzer.xml")?
    } else {
        fetch_official(
            runner,
            "https://www.powershellgallery.com/api/v2/FindPackagesById()?id=%27PSScriptAnalyzer%27",
            false,
        )?
    };
    let body = std::str::from_utf8(&bytes).context("PowerShell Gallery feed is not UTF-8")?;
    let version_regex =
        Regex::new(r"(?s)<(?:[A-Za-z0-9_]+:)?Version>([^<]+)</(?:[A-Za-z0-9_]+:)?Version>")
            .context("could not prepare PowerShell Gallery version parser")?;
    let mut versions = stable_versions(
        version_regex
            .captures_iter(body)
            .filter_map(|capture| capture.get(1).map(|value| value.as_str())),
    );
    let version = versions
        .pop()
        .context("PowerShell Gallery feed contained no stable semantic versions")?;
    Ok(UpstreamVersion {
        version: version.to_string(),
        url: "https://www.powershellgallery.com/packages/PSScriptAnalyzer".to_owned(),
    })
}

pub(super) fn nuget_latest(
    runner: &mut dyn Runner,
    fixtures: Option<&Path>,
) -> Result<UpstreamVersion> {
    let bytes = if let Some(directory) = fixtures {
        read_fixture(directory, "gameInput.json")?
    } else {
        fetch_official(
            runner,
            "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/index.json",
            false,
        )?
    };
    let metadata: Value =
        serde_json::from_slice(&bytes).context("NuGet version index is not valid JSON")?;
    let versions = metadata
        .get("versions")
        .and_then(Value::as_array)
        .context("NuGet version index has no versions array")?;
    let mut stable = stable_versions(versions.iter().filter_map(Value::as_str));
    let version = stable
        .pop()
        .context("NuGet version index contained no stable semantic versions")?;
    Ok(UpstreamVersion {
        version: version.to_string(),
        url: "https://www.nuget.org/packages/Microsoft.GameInput".to_owned(),
    })
}

pub(super) fn stable_versions<'a>(values: impl Iterator<Item = &'a str>) -> Vec<Version> {
    let mut versions = values
        .filter_map(|value| Version::parse(value).ok())
        .filter(|version| version.pre.is_empty())
        .collect::<Vec<_>>();
    versions.sort();
    versions.dedup();
    versions
}

pub(super) fn normalize_tag(tag: &str) -> Result<String> {
    let mut normalized = tag;
    for prefix in ["codeql-bundle-", "llvmorg-"] {
        if let Some(stripped) = normalized.strip_prefix(prefix) {
            normalized = stripped;
        }
    }
    normalized = normalized.strip_prefix('v').unwrap_or(normalized);
    let version = Version::parse(normalized)
        .with_context(|| format!("upstream stable tag '{tag}' is not semantic versioning"))?;
    ensure!(
        version.pre.is_empty(),
        "upstream tag '{tag}' is a prerelease"
    );
    Ok(version.to_string())
}

fn fetch_official(runner: &mut dyn Runner, url: &str, github_headers: bool) -> Result<Vec<u8>> {
    fetch_official_with_timeout(runner, url, github_headers, QUERY_TIMEOUT)
}

fn fetch_official_with_timeout(
    runner: &mut dyn Runner,
    url: &str,
    github_headers: bool,
    timeout: Duration,
) -> Result<Vec<u8>> {
    ensure!(
        url.starts_with("https://"),
        "official metadata URL must use HTTPS"
    );
    let curl = curl::executable()?;
    let temporary = NamedTempFile::new().context("could not create upstream metadata file")?;
    let temporary_path = temporary.into_temp_path();
    let mut args: Vec<OsString> = vec![
        "--fail".into(),
        "--location".into(),
        "--silent".into(),
        "--show-error".into(),
        "--max-redirs".into(),
        "5".into(),
        "--proto".into(),
        "=https".into(),
        "--proto-redir".into(),
        "=https".into(),
        "--max-filesize".into(),
        MAX_METADATA_BYTES.to_string().into(),
        "--max-time".into(),
        timeout.as_secs_f64().to_string().into(),
        "--output".into(),
        temporary_path.as_os_str().to_owned(),
    ];
    if github_headers {
        args.extend([
            "--header".into(),
            "Accept: application/vnd.github+json".into(),
            "--header".into(),
            "X-GitHub-Api-Version: 2022-11-28".into(),
        ]);
    }
    args.push(url.into());
    let cwd = env::current_dir()
        .context("could not read current working directory")?
        .canonicalize()
        .context("could not resolve current working directory")?;
    let spec = CommandSpec {
        program: curl,
        args,
        cwd,
        env: BTreeMap::new(),
        timeout: timeout + Duration::from_secs(5),
        output_limit: QUERY_OUTPUT_LIMIT,
    };
    run_checked(runner, &spec).context("official metadata request failed")?;
    let metadata = fs::symlink_metadata(&temporary_path)
        .context("official metadata request produced no response file")?;
    ensure!(
        metadata.is_file() && metadata.len() <= MAX_METADATA_BYTES,
        "official metadata response is not a bounded regular file"
    );
    fs::read(&temporary_path).context("could not read official metadata response")
}

fn read_fixture(directory: &Path, name: &str) -> Result<Vec<u8>> {
    let path = directory.join(name);
    let metadata = fs::symlink_metadata(&path)
        .with_context(|| format!("offline fixture '{}' is missing", path.display()))?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink(),
        "offline fixture must be a regular file"
    );
    ensure!(
        metadata.len() <= MAX_METADATA_BYTES,
        "offline fixture exceeds the metadata size limit"
    );
    fs::read(path).context("could not read offline metadata fixture")
}

pub(super) fn resolve_fixture_root(path: &Path) -> Result<PathBuf> {
    let resolved = path.canonicalize().with_context(|| {
        format!(
            "could not resolve offline fixture directory {}",
            path.display()
        )
    })?;
    ensure!(
        resolved.is_dir(),
        "offline fixtures path is not a directory"
    );
    Ok(resolved)
}
