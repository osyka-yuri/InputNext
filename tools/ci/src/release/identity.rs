//! Workflow identity and source qualification.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, bail, ensure};
use regex::Regex;
use semver::Version;
use serde::{Deserialize, Serialize};

use crate::process::{CommandSpec, Runner, run_checked};

use super::QualifyArgs;
use super::strict_json;

const MAX_CHANGELOG_BYTES: u64 = 8 * 1024 * 1024;
const COMMAND_OUTPUT_LIMIT: usize = 1024 * 1024;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(super) struct ReleaseIdentity {
    schema_version: u8,
    repository: String,
    source_commit: String,
    source_ref: String,
    tag: String,
    version: String,
    run_id: String,
    run_attempt: String,
    workflow_commit: String,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(super) struct ReleaseMetadata {
    schema_version: u8,
    repository: String,
    source_commit: String,
    source_ref: String,
    tag: String,
    version: String,
    run_id: String,
    run_attempt: String,
    workflow_commit: String,
    notes_sha256: String,
    pub(super) assets: Vec<AssetDigest>,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(super) struct AssetDigest {
    pub(super) name: String,
    pub(super) sha256: String,
}

#[derive(Clone, Debug)]
pub(super) struct QualifiedSource {
    pub identity: ReleaseIdentity,
    pub version: String,
    pub notes: String,
}

impl ReleaseIdentity {
    fn from_args(args: &QualifyArgs) -> Result<Self> {
        let version = stable_version_from_tag(&args.tag)?;
        let identity = Self {
            schema_version: 1,
            repository: args.repository.clone(),
            source_commit: args.source_commit.clone(),
            source_ref: args.source_ref.clone(),
            tag: args.tag.clone(),
            version,
            run_id: args.run_id.clone(),
            run_attempt: args.run_attempt.clone(),
            workflow_commit: args.workflow_commit.clone(),
        };
        identity.validate()?;
        Ok(identity)
    }

    pub(super) fn validate(&self) -> Result<()> {
        ensure!(
            self.schema_version == 1,
            "unsupported release identity schema"
        );
        validate_repository(&self.repository)?;
        validate_commit(&self.source_commit, "source commit")?;
        validate_commit(&self.workflow_commit, "workflow commit")?;
        ensure!(
            self.workflow_commit == self.source_commit,
            "defining workflow and source must use the same immutable commit"
        );
        ensure!(
            self.source_ref == format!("refs/tags/{}", self.tag),
            "source ref must be the canonical tag ref"
        );
        ensure!(
            stable_version_from_tag(&self.tag)? == self.version,
            "identity tag and version disagree"
        );
        validate_positive_decimal(&self.run_id, "run ID")?;
        validate_positive_decimal(&self.run_attempt, "run attempt")?;
        Ok(())
    }

    pub(super) fn tag(&self) -> &str {
        &self.tag
    }

    pub(super) fn repository(&self) -> &str {
        &self.repository
    }

    pub(super) fn source_commit(&self) -> &str {
        &self.source_commit
    }

    pub(super) fn workflow_commit(&self) -> &str {
        &self.workflow_commit
    }

    pub(super) fn source_ref(&self) -> &str {
        &self.source_ref
    }

    pub(super) fn version(&self) -> &str {
        &self.version
    }

    pub(super) fn run_id(&self) -> &str {
        &self.run_id
    }

    pub(super) fn run_attempt(&self) -> &str {
        &self.run_attempt
    }

    pub(super) fn metadata(
        &self,
        notes_sha256: String,
        assets: Vec<AssetDigest>,
    ) -> ReleaseMetadata {
        ReleaseMetadata {
            schema_version: self.schema_version,
            repository: self.repository.clone(),
            source_commit: self.source_commit.clone(),
            source_ref: self.source_ref.clone(),
            tag: self.tag.clone(),
            version: self.version.clone(),
            run_id: self.run_id.clone(),
            run_attempt: self.run_attempt.clone(),
            workflow_commit: self.workflow_commit.clone(),
            notes_sha256,
            assets,
        }
    }
}

pub(super) fn load_identity(path: &Path) -> Result<ReleaseIdentity> {
    let value = read_json(path, 1024 * 1024)?;
    let identity: ReleaseIdentity = serde_json::from_value(value)
        .with_context(|| format!("release identity is invalid: {}", path.display()))?;
    identity.validate()?;
    Ok(identity)
}

pub(super) fn load_metadata(path: &Path) -> Result<ReleaseMetadata> {
    let value = read_json(path, 1024 * 1024)?;
    let metadata: ReleaseMetadata = serde_json::from_value(value)
        .with_context(|| format!("release metadata is invalid: {}", path.display()))?;
    Ok(metadata)
}

impl ReleaseMetadata {
    pub(super) fn validate(&self, identity: &ReleaseIdentity, notes_sha256: &str) -> Result<()> {
        identity.validate()?;
        ensure!(
            self.schema_version == identity.schema_version
                && self.repository == identity.repository
                && self.source_commit == identity.source_commit
                && self.source_ref == identity.source_ref
                && self.tag == identity.tag
                && self.version == identity.version
                && self.run_id == identity.run_id
                && self.run_attempt == identity.run_attempt
                && self.workflow_commit == identity.workflow_commit,
            "release metadata does not match the verified workflow run"
        );
        ensure!(
            self.notes_sha256 == notes_sha256,
            "release notes fail the metadata digest check"
        );
        Ok(())
    }
}

pub(super) fn validate_source(identity: ReleaseIdentity, root: &Path) -> Result<QualifiedSource> {
    identity.validate()?;
    let root = canonical_directory(root, "source root")?;
    let version = source_version(&root)?;
    ensure!(
        version == identity.version,
        "tag version {} does not match root CMake project version {version}",
        identity.version
    );
    let notes = release_notes(&root, &version)?;
    Ok(QualifiedSource {
        identity,
        version,
        notes,
    })
}

pub(super) fn canonical_directory(path: &Path, label: &str) -> Result<PathBuf> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect {label}: {}", path.display()))?;
    ensure!(
        metadata.file_type().is_dir(),
        "{label} must be a real directory"
    );
    let canonical = path
        .canonicalize()
        .with_context(|| format!("could not resolve {label}: {}", path.display()))?;
    ensure!(
        canonical.is_dir(),
        "{label} is not a directory: {}",
        canonical.display()
    );
    Ok(canonical)
}

pub(super) fn read_json(path: &Path, limit: u64) -> Result<serde_json::Value> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect JSON file: {}", path.display()))?;
    ensure!(
        metadata.file_type().is_file(),
        "JSON input must be a regular file: {}",
        path.display()
    );
    ensure!(
        metadata.len() <= limit,
        "JSON input is too large: {}",
        path.display()
    );
    let bytes =
        fs::read(path).with_context(|| format!("could not read JSON file: {}", path.display()))?;
    strict_json::parse_slice(&bytes)
        .with_context(|| format!("JSON input is invalid: {}", path.display()))
}

pub(super) fn write_new_json<T: Serialize>(path: &Path, value: &T) -> Result<()> {
    let bytes = serde_json::to_vec_pretty(value).context("could not serialize release identity")?;
    write_new_file(path, &bytes)
}

pub(super) fn write_new_file(path: &Path, bytes: &[u8]) -> Result<()> {
    ensure!(path.is_absolute(), "release output path must be absolute");
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .with_context(|| {
            format!(
                "release output already exists or cannot be created: {}",
                path.display()
            )
        })?;
    file.write_all(bytes)
        .with_context(|| format!("could not write release output: {}", path.display()))
}

pub(super) fn append_github_output(path: &Path, version: &str) -> Result<()> {
    ensure!(path.is_absolute(), "GitHub output path must be absolute");
    let mut file = OpenOptions::new()
        .append(true)
        .create(true)
        .open(path)
        .with_context(|| format!("could not open GitHub output file: {}", path.display()))?;
    writeln!(file, "version={version}")
        .with_context(|| format!("could not append GitHub output file: {}", path.display()))
}

pub(super) fn qualify(args: &QualifyArgs, runner: &mut dyn Runner) -> Result<()> {
    ensure!(
        args.event_name == "push",
        "only stable tag push events may qualify a release"
    );
    let identity = ReleaseIdentity::from_args(args)?;
    let source_root = canonical_directory(&args.source_root, "source root")?;
    ensure!(
        args.git.is_absolute(),
        "Git executable path must be absolute"
    );
    ensure!(
        args.git.is_file(),
        "Git executable does not exist: {}",
        args.git.display()
    );
    ensure!(
        source_root.join(".git").exists(),
        "source root is not a Git checkout"
    );

    let head = git_output(
        runner,
        &args.git,
        &source_root,
        ["rev-parse".to_owned(), "HEAD".to_owned()],
    )?;
    ensure!(
        head == identity.source_commit,
        "checked out source differs from the workflow source commit"
    );
    let peeled = git_output(
        runner,
        &args.git,
        &source_root,
        [
            "rev-parse".to_owned(),
            "--verify".to_owned(),
            "--end-of-options".to_owned(),
            format!("refs/tags/{}^{{commit}}", identity.tag()),
        ],
    )?;
    ensure!(
        peeled == identity.source_commit,
        "event tag does not peel to the source commit"
    );
    let ancestor_spec = command_spec(
        &args.git,
        &source_root,
        [
            "merge-base".to_owned(),
            "--is-ancestor".to_owned(),
            identity.source_commit.clone(),
            "refs/remotes/origin/main".to_owned(),
        ],
    );
    let ancestry = runner.run(&ancestor_spec)?;
    ensure!(
        ancestry.code == Some(0),
        "release source is not reachable from origin/main"
    );
    let status = git_output(
        runner,
        &args.git,
        &source_root,
        [
            "status".to_owned(),
            "--porcelain".to_owned(),
            "--untracked-files=normal".to_owned(),
        ],
    )?;
    ensure!(
        status.is_empty(),
        "release source checkout must be clean before building"
    );
    let qualified = validate_source(identity.clone(), &source_root)?;

    write_new_json(&args.out_identity, &identity)?;
    append_github_output(&args.github_output, &qualified.version)?;
    Ok(())
}

fn git_output<I, S>(runner: &mut dyn Runner, git: &Path, cwd: &Path, args: I) -> Result<String>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    let output = run_checked(runner, &command_spec(git, cwd, args))?;
    let text = std::str::from_utf8(&output.stdout).context("Git returned non-UTF-8 output")?;
    Ok(text.trim().to_owned())
}

fn command_spec<I, S>(git: &Path, cwd: &Path, args: I) -> CommandSpec
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    CommandSpec {
        program: git.to_path_buf(),
        args: args.into_iter().map(Into::into).collect(),
        cwd: cwd.to_path_buf(),
        env: std::collections::BTreeMap::default(),
        timeout: Duration::from_secs(30),
        output_limit: COMMAND_OUTPUT_LIMIT,
    }
}

fn validate_repository(repository: &str) -> Result<()> {
    let mut segments = repository.split('/');
    let owner = segments.next().unwrap_or_default();
    let name = segments.next().unwrap_or_default();
    ensure!(
        !owner.is_empty()
            && !name.is_empty()
            && segments.next().is_none()
            && valid_repository_segment(owner)
            && valid_repository_segment(name),
        "repository must be a canonical owner/repository name"
    );
    Ok(())
}

fn valid_repository_segment(segment: &str) -> bool {
    segment != "."
        && segment != ".."
        && segment
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
}

fn validate_commit(commit: &str, label: &str) -> Result<()> {
    ensure!(
        commit.len() == 40
            && commit
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase()),
        "{label} must be a lowercase 40-character Git SHA"
    );
    Ok(())
}

fn validate_positive_decimal(value: &str, label: &str) -> Result<()> {
    ensure!(
        !value.is_empty()
            && value.bytes().all(|byte| byte.is_ascii_digit())
            && value
                .parse::<u64>()
                .is_ok_and(|parsed| parsed > 0 && parsed.to_string() == value),
        "{label} must be a canonical positive decimal integer"
    );
    Ok(())
}

pub(super) fn stable_version_from_tag(tag: &str) -> Result<String> {
    let Some(version) = tag.strip_prefix('v') else {
        bail!("release tag must use the canonical vX.Y.Z form")
    };
    let parsed = Version::parse(version).context("release tag is not a stable semantic version")?;
    ensure!(
        parsed.pre.is_empty() && parsed.build.is_empty() && parsed.to_string() == version,
        "release tag must be a stable semantic version without leading zeroes"
    );
    Ok(version.to_owned())
}

fn source_version(root: &Path) -> Result<String> {
    let path = root.join("CMakeLists.txt");
    let contents = read_utf8_regular_file(&path, 8 * 1024 * 1024, "root CMakeLists.txt")?;
    let expression = Regex::new(
        r"(?mi)^[ \t]*(?i:project)[ \t]*\([ \t]*InputNext[ \t]+(?i:VERSION)[ \t]+(?P<version>(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))(?:[ \t)]|$)",
    )
    .context("could not compile source version expression")?;
    let matches = expression
        .captures_iter(&contents)
        .filter_map(|capture| capture.name("version"))
        .map(|capture| capture.as_str().to_owned())
        .collect::<Vec<_>>();
    ensure!(
        matches.len() == 1,
        "root CMakeLists.txt must define exactly one numeric InputNext project version"
    );
    matches
        .into_iter()
        .next()
        .context("root CMake project version disappeared")
}

fn release_notes(root: &Path, version: &str) -> Result<String> {
    let path = root.join("CHANGELOG.md");
    let contents = read_utf8_regular_file(&path, MAX_CHANGELOG_BYTES, "CHANGELOG.md")?;
    let heading = Regex::new(
        r"^##[ \t]+(?P<label>(?:\[[0-9]+\.[0-9]+\.[0-9]+\]|\[Unreleased\]|[0-9]+\.[0-9]+\.[0-9]+|Unreleased))(?:[ \t]+(?:-|–|—)[ \t]*(?P<tail>.*))?[ \t]*$",
    )
    .context("could not compile release heading expression")?;
    let lines = contents.lines().collect::<Vec<_>>();
    let matching = lines
        .iter()
        .enumerate()
        .filter_map(|(index, line)| {
            let captures = heading.captures(line)?;
            let label = captures.name("label")?.as_str();
            let label = label.strip_prefix('[').unwrap_or(label);
            let label = label.strip_suffix(']').unwrap_or(label);
            (label == version).then_some(index)
        })
        .collect::<Vec<_>>();
    ensure!(
        matching.len() == 1,
        "CHANGELOG.md must contain exactly one curated section for version {version}"
    );
    let start = matching[0];
    let end = lines
        .iter()
        .enumerate()
        .skip(start + 1)
        .find_map(|(index, line)| {
            let remainder = line.strip_prefix("##")?;
            remainder
                .chars()
                .next()
                .is_some_and(char::is_whitespace)
                .then_some(index)
        })
        .unwrap_or(lines.len());
    let body = lines[start + 1..end].join("\n");
    ensure!(
        !body.trim().is_empty(),
        "CHANGELOG.md section for version {version} is empty"
    );
    Ok(lines[start..end].join("\n").trim_end().to_owned())
}

fn read_utf8_regular_file(path: &Path, limit: u64, label: &str) -> Result<String> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect {label}: {}", path.display()))?;
    ensure!(
        metadata.file_type().is_file(),
        "{label} must be a regular file: {}",
        path.display()
    );
    ensure!(
        metadata.len() <= limit,
        "{label} is too large: {}",
        path.display()
    );
    fs::read_to_string(path)
        .with_context(|| format!("{label} is not valid UTF-8: {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::{ReleaseIdentity, stable_version_from_tag, validate_repository};

    #[test]
    fn validates_canonical_release_identity() {
        let identity = ReleaseIdentity {
            schema_version: 1,
            repository: "owner/repository".to_owned(),
            source_commit: "a".repeat(40),
            source_ref: "refs/tags/v1.2.3".to_owned(),
            tag: "v1.2.3".to_owned(),
            version: "1.2.3".to_owned(),
            run_id: "123".to_owned(),
            run_attempt: "2".to_owned(),
            workflow_commit: "a".repeat(40),
        };

        assert!(identity.validate().is_ok());
    }

    #[test]
    fn rejects_pre_release_and_noncanonical_tags() {
        assert!(stable_version_from_tag("v1.2.3-rc.1").is_err());
        assert!(stable_version_from_tag("v01.2.3").is_err());
        assert!(stable_version_from_tag("1.2.3").is_err());
    }

    #[test]
    fn rejects_noncanonical_repositories() {
        assert!(validate_repository("owner/repo/nested").is_err());
        assert!(validate_repository("../repo").is_err());
        assert!(validate_repository("owner/repo").is_ok());
    }
}
