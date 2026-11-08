//! Official stable-version proposals for the reviewed CI tool lock.

mod sources;

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use clap::Args;
use semver::Version;
use serde::Serialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use tempfile::NamedTempFile;

use crate::process::Runner;

#[derive(Debug, Args)]
pub struct UpdatesArgs {
    #[arg(long)]
    lock: PathBuf,
    #[arg(long)]
    report: PathBuf,
    #[arg(long)]
    offline_fixtures: Option<PathBuf>,
}

impl UpdatesArgs {
    /// Reads official latest stable versions and writes a review report.
    ///
    /// # Errors
    ///
    /// Returns an error if upstream metadata, the lock, or the report path is invalid.
    pub fn run(self, runner: &mut dyn Runner) -> Result<()> {
        let lock_path = self
            .lock
            .canonicalize()
            .with_context(|| format!("could not resolve toolchain lock {}", self.lock.display()))?;
        let lock_metadata =
            fs::symlink_metadata(&lock_path).context("could not inspect toolchain lock")?;
        ensure!(
            lock_metadata.is_file() && !lock_metadata.file_type().is_symlink(),
            "toolchain lock must be a regular file"
        );
        let lock_bytes = fs::read(&lock_path).context("could not read toolchain lock")?;
        let lock: Value =
            serde_json::from_slice(&lock_bytes).context("toolchain lock is not valid JSON")?;
        ensure!(
            lock.get("schemaVersion").and_then(Value::as_u64) == Some(1),
            "unsupported toolchain lock schema"
        );
        let lock_sha256 = hash_bytes(&lock_bytes);
        ensure!(
            !same_output_path(&lock_path, &self.report)?,
            "update report path must not replace the toolchain lock"
        );
        let fixtures = self
            .offline_fixtures
            .as_deref()
            .map(sources::resolve_fixture_root)
            .transpose()?;

        let mut candidates = Vec::new();
        for (name, repository) in GITHUB_TOOLS {
            let current = locked_version(&lock, "tools", name)?;
            let release = sources::github_release(runner, fixtures.as_deref(), name, repository)?;
            candidates.push(candidate(
                name,
                &current,
                &release.version,
                &release.url,
                "github",
            )?);
        }

        let current = locked_version(&lock, "tools", "psscriptanalyzer")?;
        let latest = sources::gallery_latest(runner, fixtures.as_deref())?;
        candidates.push(candidate(
            "psscriptanalyzer",
            &current,
            &latest.version,
            &latest.url,
            "powershell-gallery",
        )?);

        let current = locked_version(&lock, "dependencies", "tomlplusplus")?;
        let release = sources::github_release(
            runner,
            fixtures.as_deref(),
            "tomlplusplus",
            "marzer/tomlplusplus",
        )?;
        candidates.push(candidate(
            "tomlplusplus",
            &current,
            &release.version,
            &release.url,
            "github",
        )?);

        let current = locked_version(&lock, "dependencies", "codeqlBundle")?;
        let release = sources::github_bundle_release(runner, fixtures.as_deref())?;
        candidates.push(candidate(
            "codeqlBundle",
            &current,
            &release.version,
            &release.url,
            "github",
        )?);

        let current = locked_version(&lock, "dependencies", "gameInput")?;
        let latest = sources::nuget_latest(runner, fixtures.as_deref())?;
        candidates.push(candidate(
            "gameInput",
            &current,
            &latest.version,
            &latest.url,
            "nuget",
        )?);

        let report = UpdateReport {
            schema_version: 1,
            lock_sha256,
            candidates,
        };
        write_report(&self.report, &report)?;
        let proposed = report
            .candidates
            .iter()
            .filter(|entry| entry.status == CandidateStatus::ReviewRequired)
            .count();
        println!(
            "Toolchain update report: {} candidates, {} require review; lock unchanged",
            report.candidates.len(),
            proposed
        );
        Ok(())
    }
}

const GITHUB_TOOLS: [(&str, &str); 8] = [
    ("cmake", "Kitware/CMake"),
    ("ninja", "ninja-build/ninja"),
    ("powershell", "PowerShell/PowerShell"),
    ("llvm", "llvm/llvm-project"),
    ("gh", "cli/cli"),
    ("actionlint", "rhysd/actionlint"),
    ("shellcheck", "koalaman/shellcheck"),
    ("git-cliff", "orhun/git-cliff"),
];

#[derive(Clone, Debug, Serialize)]
struct UpdateReport {
    schema_version: u32,
    lock_sha256: String,
    candidates: Vec<UpdateCandidate>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
enum CandidateStatus {
    Current,
    ReviewRequired,
    UpstreamOlder,
}

#[derive(Clone, Debug, Serialize)]
struct UpdateCandidate {
    component: String,
    current_version: String,
    latest_stable_version: String,
    status: CandidateStatus,
    source: String,
    source_url: String,
}

fn candidate(
    component: &str,
    current: &str,
    latest: &str,
    source_url: &str,
    source: &str,
) -> Result<UpdateCandidate> {
    let current_version = Version::parse(current)
        .with_context(|| format!("locked component '{component}' has an invalid version"))?;
    let latest_version = Version::parse(latest)
        .with_context(|| format!("upstream component '{component}' has an invalid version"))?;
    let status = match latest_version.cmp(&current_version) {
        std::cmp::Ordering::Greater => CandidateStatus::ReviewRequired,
        std::cmp::Ordering::Equal => CandidateStatus::Current,
        std::cmp::Ordering::Less => CandidateStatus::UpstreamOlder,
    };
    Ok(UpdateCandidate {
        component: component.to_owned(),
        current_version: current_version.to_string(),
        latest_stable_version: latest_version.to_string(),
        status,
        source: source.to_owned(),
        source_url: source_url.to_owned(),
    })
}

fn locked_version(lock: &Value, section: &str, name: &str) -> Result<String> {
    lock.get(section)
        .and_then(Value::as_object)
        .with_context(|| format!("toolchain lock has no '{section}' object"))?
        .get(name)
        .and_then(Value::as_object)
        .with_context(|| format!("toolchain lock has no '{section}.{name}' entry"))?
        .get("version")
        .and_then(Value::as_str)
        .map(str::to_owned)
        .with_context(|| format!("toolchain lock has no version for '{section}.{name}'"))
}

fn same_output_path(lock_path: &Path, report_path: &Path) -> Result<bool> {
    if report_path == lock_path {
        return Ok(true);
    }
    let report_parent = report_path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    if !report_parent.exists() {
        return Ok(false);
    }
    let report_parent = report_parent.canonicalize()?;
    let report_name = report_path
        .file_name()
        .context("update report has no file name")?;
    Ok(report_parent.join(report_name) == lock_path)
}

fn hash_bytes(bytes: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    let digest = hasher.finalize();
    encode_sha256(&digest)
}

fn encode_sha256(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn write_report(path: &Path, report: &UpdateReport) -> Result<()> {
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).context("could not create tool update report directory")?;
    let parent = parent
        .canonicalize()
        .context("could not resolve tool update report directory")?;
    let name = path
        .file_name()
        .context("tool update report has no file name")?;
    let destination = parent.join(name);
    if let Ok(metadata) = fs::symlink_metadata(&destination) {
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "existing update report must be a regular file"
        );
    }
    let mut temporary =
        NamedTempFile::new_in(&parent).context("could not create temporary update report")?;
    serde_json::to_writer_pretty(&mut temporary, report)
        .context("could not encode update report")?;
    temporary
        .write_all(b"\n")
        .context("could not finish update report")?;
    temporary.flush().context("could not flush update report")?;
    temporary
        .persist(&destination)
        .context("could not publish update report")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::sources::{normalize_tag, stable_versions};
    use super::{CandidateStatus, candidate};
    use anyhow::{Result, bail};
    use serde_json::{Value, json};
    use std::fs;
    use std::path::Path;

    use crate::process::{CommandOutput, CommandSpec, Runner};

    #[test]
    fn release_tags_normalize_known_prefixes() -> Result<()> {
        assert_eq!(normalize_tag("v4.4.5")?, "4.4.5");
        assert_eq!(normalize_tag("llvmorg-23.2.0")?, "23.2.0");
        assert_eq!(normalize_tag("codeql-bundle-v2.28.0")?, "2.28.0");
        Ok(())
    }

    #[test]
    fn stable_version_selection_excludes_prereleases() {
        let mut versions = stable_versions(["1.25.0-preview", "1.24.0", "1.25.0"].into_iter());
        assert_eq!(
            versions.pop().map(|version| version.to_string()).as_deref(),
            Some("1.25.0")
        );
    }

    #[test]
    fn newer_candidates_require_review_without_editing_the_lock() -> Result<()> {
        let proposal = candidate(
            "cmake",
            "4.4.4",
            "4.5.0",
            "https://github.com/Kitware/CMake/releases/tag/v4.5.0",
            "github",
        )?;
        assert_eq!(proposal.status, CandidateStatus::ReviewRequired);
        assert_eq!(proposal.latest_stable_version, "4.5.0");
        Ok(())
    }

    #[test]
    fn offline_fixture_report_reads_supported_sources_without_changing_lock() -> Result<()> {
        let fixtures = tempfile::tempdir()?;
        for (name, repository) in super::GITHUB_TOOLS {
            write_release(fixtures.path(), name, repository, "v1.1.0")?;
        }
        write_release(
            fixtures.path(),
            "tomlplusplus",
            "marzer/tomlplusplus",
            "v3.5.0",
        )?;
        fs::write(
            fixtures.path().join("psscriptanalyzer.xml"),
            "<feed><entry><Version>1.25.0</Version><Version>1.26.0-preview</Version></entry></feed>",
        )?;
        fs::write(
            fixtures.path().join("codeqlBundle.json"),
            serde_json::to_vec(&json!([
                {
                    "draft": false,
                    "prerelease": false,
                    "tag_name": "codeql-bundle-v2.28.0",
                    "html_url": "https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.28.0"
                }
            ]))?,
        )?;
        fs::write(
            fixtures.path().join("gameInput.json"),
            serde_json::to_vec(&json!({"versions": ["4.0.302", "4.1.0"]}))?,
        )?;

        let lock_path = fixtures.path().join("toolchain-lock.json");
        let report_path = fixtures.path().join("updates.json");
        let mut tools = serde_json::Map::new();
        for (name, _) in super::GITHUB_TOOLS {
            tools.insert(name.to_owned(), json!({"version": "1.0.0"}));
        }
        tools.insert("psscriptanalyzer".to_owned(), json!({"version": "1.25.0"}));
        let lock = json!({
            "schemaVersion": 1,
            "tools": tools,
            "dependencies": {
                "tomlplusplus": {"version": "3.4.0"},
                "codeqlBundle": {"version": "2.27.2"},
                "gameInput": {"version": "4.0.302"}
            }
        });
        let lock_bytes = serde_json::to_vec_pretty(&lock)?;
        fs::write(&lock_path, &lock_bytes)?;

        super::UpdatesArgs {
            lock: lock_path.clone(),
            report: report_path.clone(),
            offline_fixtures: Some(fixtures.path().to_path_buf()),
        }
        .run(&mut OfflineRunner)?;

        let report: Value = serde_json::from_slice(&fs::read(report_path)?)?;
        assert_eq!(report["candidates"].as_array().map(Vec::len), Some(12));
        assert_eq!(fs::read(lock_path)?, lock_bytes);
        Ok(())
    }

    fn write_release(directory: &Path, name: &str, repository: &str, tag: &str) -> Result<()> {
        fs::write(
            directory.join(format!("{name}.json")),
            serde_json::to_vec(&json!({
                "draft": false,
                "prerelease": false,
                "tag_name": tag,
                "html_url": format!("https://github.com/{repository}/releases/tag/{tag}")
            }))?,
        )?;
        Ok(())
    }

    struct OfflineRunner;

    impl Runner for OfflineRunner {
        fn run(&mut self, _spec: &CommandSpec) -> Result<CommandOutput> {
            bail!("offline update fixtures unexpectedly attempted a process")
        }
    }
}
