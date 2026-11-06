//! `CodeQL` SARIF validation and severity reporting.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use clap::Args;
use serde::Serialize;
use serde_json::Value;
use tempfile::NamedTempFile;
use walkdir::WalkDir;

use crate::process::Runner;

mod rules;

const MAX_SARIF_FILES: usize = 1000;
const MAX_SARIF_FILE_BYTES: u64 = 128 * 1024 * 1024;
const MAX_SARIF_TOTAL_BYTES: u64 = 1024 * 1024 * 1024;
const MAX_REPORTED_MESSAGE_CHARS: usize = 4096;
const MAX_FINDINGS: usize = 25_000;
const SECURITY_BLOCKER_SEVERITY: f64 = 7.0;

#[derive(Debug, Args)]
pub struct CodeqlArgs {
    #[arg(long)]
    results: PathBuf,
    #[arg(long)]
    report: Option<PathBuf>,
}

impl CodeqlArgs {
    /// Validates SARIF results and writes a concise evaluation report.
    ///
    /// # Errors
    ///
    /// Returns an error for malformed or incomplete SARIF, blocking findings, or report I/O failures.
    pub fn run(self, _runner: &mut dyn Runner) -> Result<()> {
        let files = sarif_files(&self.results)?;
        let mut report = CodeqlReport {
            schema_version: 1,
            files: Vec::new(),
            findings: Vec::new(),
            blocker_count: 0,
            unclassified_count: 0,
        };

        for path in files {
            let file = parse_sarif_file(&path)?;
            report.blocker_count += file.blocker_count;
            report.unclassified_count += file.unclassified_count;
            report.findings.extend(file.findings);
            report.files.push(path);
        }

        if let Some(path) = &self.report {
            write_report(path, &report)?;
        }
        println!(
            "CodeQL SARIF: {} file(s), {} finding(s), {} security blocker(s), {} unclassified finding(s)",
            report.files.len(),
            report.findings.len(),
            report.blocker_count,
            report.unclassified_count
        );
        for finding in report.findings.iter().filter(|finding| {
            finding
                .security_severity
                .is_some_and(|severity| severity >= SECURITY_BLOCKER_SEVERITY)
        }) {
            println!("BLOCKER {}: {}", finding.rule_id, finding.message);
        }
        ensure!(
            report.blocker_count == 0,
            "CodeQL reported security findings with severity >= {SECURITY_BLOCKER_SEVERITY:.1}"
        );
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize)]
struct CodeqlReport {
    schema_version: u32,
    files: Vec<PathBuf>,
    findings: Vec<CodeqlFinding>,
    blocker_count: usize,
    unclassified_count: usize,
}

#[derive(Clone, Debug, Serialize)]
struct CodeqlFinding {
    file: PathBuf,
    rule_id: String,
    security_severity: Option<f64>,
    level: Option<String>,
    message: String,
    uri: Option<String>,
    start_line: Option<u64>,
}

#[derive(Debug)]
struct FileFindings {
    findings: Vec<CodeqlFinding>,
    blocker_count: usize,
    unclassified_count: usize,
}

fn sarif_files(path: &Path) -> Result<Vec<PathBuf>> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect SARIF path {}", path.display()))?;
    ensure!(
        !metadata.file_type().is_symlink(),
        "SARIF path must not be a symbolic link"
    );

    let mut files = Vec::new();
    let mut total_bytes = 0_u64;
    if metadata.is_file() {
        ensure!(
            path.extension()
                .is_some_and(|extension| extension.to_string_lossy().eq_ignore_ascii_case("sarif")),
            "SARIF input file must have a .sarif extension"
        );
        add_sarif_file(path, &mut files, &mut total_bytes)?;
    } else {
        ensure!(
            metadata.is_dir(),
            "SARIF results path is not a file or directory"
        );
        for entry in WalkDir::new(path).follow_links(false) {
            let entry = entry.context("could not enumerate SARIF results")?;
            let file_type = entry.file_type();
            ensure!(
                !file_type.is_symlink(),
                "SARIF results directory contains a symbolic link"
            );
            if file_type.is_file()
                && entry.path().extension().is_some_and(|extension| {
                    extension.to_string_lossy().eq_ignore_ascii_case("sarif")
                })
            {
                add_sarif_file(entry.path(), &mut files, &mut total_bytes)?;
            }
        }
    }

    files.sort();
    ensure!(!files.is_empty(), "no SARIF files were found");
    Ok(files)
}

fn add_sarif_file(path: &Path, files: &mut Vec<PathBuf>, total_bytes: &mut u64) -> Result<()> {
    ensure!(files.len() < MAX_SARIF_FILES, "too many SARIF files");
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("could not inspect SARIF file {}", path.display()))?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink(),
        "SARIF result must be a regular file"
    );
    ensure!(
        metadata.len() <= MAX_SARIF_FILE_BYTES,
        "SARIF file exceeds the size limit"
    );
    *total_bytes = total_bytes
        .checked_add(metadata.len())
        .context("SARIF total size overflow")?;
    ensure!(
        *total_bytes <= MAX_SARIF_TOTAL_BYTES,
        "SARIF directory exceeds the total size limit"
    );
    files.push(
        path.canonicalize()
            .with_context(|| format!("could not resolve SARIF file {}", path.display()))?,
    );
    Ok(())
}

fn parse_sarif_file(path: &Path) -> Result<FileFindings> {
    let contents = fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
    let sarif: Value = serde_json::from_slice(&contents)
        .with_context(|| format!("SARIF file {} is not valid JSON", path.display()))?;
    let object = sarif
        .as_object()
        .context("SARIF document must be an object")?;
    ensure!(
        object.get("version").and_then(Value::as_str) == Some("2.1.0"),
        "SARIF document must declare version 2.1.0"
    );
    let runs = required_array(object, "runs")?;
    ensure!(!runs.is_empty(), "SARIF document has no runs");

    let mut output = FileFindings {
        findings: Vec::new(),
        blocker_count: 0,
        unclassified_count: 0,
    };
    for run in runs {
        parse_run(path, run, &mut output)?;
        ensure!(
            output.findings.len() <= MAX_FINDINGS,
            "SARIF document has too many findings"
        );
    }
    Ok(output)
}

fn parse_run(path: &Path, value: &Value, output: &mut FileFindings) -> Result<()> {
    let run = value.as_object().context("SARIF run must be an object")?;
    let tool = required_object(run, "tool")?;
    let components = rules::RuleComponents::parse(tool)?;
    let results = required_array(run, "results")?;
    if run.contains_key("invocations") {
        validate_invocations(required_array(run, "invocations")?)?;
    }

    for result in results {
        let result = result
            .as_object()
            .context("SARIF result must be an object")?;
        let rule = components.resolve(result)?;
        let severity = rule.security_severity;
        let message = required_object(result, "message")?;
        let message_text = required_nonempty_string(message, "text")?;
        let mut finding = CodeqlFinding {
            file: path.to_path_buf(),
            rule_id: rule.id,
            security_severity: severity,
            level: result
                .get("level")
                .map(|value| {
                    value
                        .as_str()
                        .context("SARIF result level must be a string")
                        .map(str::to_owned)
                })
                .transpose()?,
            message: message_text
                .chars()
                .take(MAX_REPORTED_MESSAGE_CHARS)
                .collect(),
            uri: None,
            start_line: None,
        };
        attach_location(result, &mut finding)?;
        match severity {
            Some(value) if value >= SECURITY_BLOCKER_SEVERITY => output.blocker_count += 1,
            None => output.unclassified_count += 1,
            _ => {}
        }
        output.findings.push(finding);
    }
    Ok(())
}

fn validate_invocations(invocations: &[Value]) -> Result<()> {
    for invocation in invocations {
        let invocation = invocation
            .as_object()
            .context("SARIF invocation must be an object")?;
        ensure!(
            invocation
                .get("executionSuccessful")
                .and_then(Value::as_bool)
                .is_some_and(|successful| successful),
            "SARIF invocation is missing a successful execution status"
        );
    }
    Ok(())
}

fn attach_location(
    result: &serde_json::Map<String, Value>,
    finding: &mut CodeqlFinding,
) -> Result<()> {
    let Some(locations) = result.get("locations") else {
        return Ok(());
    };
    let locations = locations
        .as_array()
        .context("SARIF result locations must be an array")?;
    let Some(first) = locations.first() else {
        return Ok(());
    };
    let location = first
        .as_object()
        .context("SARIF result location must be an object")?;
    let physical = location
        .get("physicalLocation")
        .and_then(Value::as_object)
        .context("SARIF physical location must be an object")?;
    if let Some(uri) = physical
        .get("artifactLocation")
        .and_then(Value::as_object)
        .and_then(|artifact| artifact.get("uri"))
    {
        finding.uri = Some(
            uri.as_str()
                .context("SARIF artifact URI must be a string")?
                .to_owned(),
        );
    }
    if let Some(line) = physical
        .get("region")
        .and_then(Value::as_object)
        .and_then(|region| region.get("startLine"))
    {
        finding.start_line = Some(
            line.as_u64()
                .context("SARIF startLine must be a nonnegative integer")?,
        );
    }
    Ok(())
}

fn parse_severity(value: &Value) -> Result<f64> {
    let severity = match value {
        Value::String(text) => text
            .parse::<f64>()
            .with_context(|| format!("invalid SARIF security-severity '{text}'"))?,
        Value::Number(number) => number
            .as_f64()
            .context("SARIF security-severity is not numeric")?,
        _ => anyhow::bail!("SARIF security-severity must be a number or numeric string"),
    };
    ensure!(
        severity.is_finite() && (0.0..=10.0).contains(&severity),
        "SARIF security-severity must be finite and between 0 and 10"
    );
    Ok(severity)
}

fn required_array<'a>(
    object: &'a serde_json::Map<String, Value>,
    name: &str,
) -> Result<&'a [Value]> {
    object
        .get(name)
        .with_context(|| format!("SARIF object is missing required '{name}'"))?
        .as_array()
        .map(Vec::as_slice)
        .with_context(|| format!("SARIF '{name}' must be an array"))
}

fn required_object<'a>(
    object: &'a serde_json::Map<String, Value>,
    name: &str,
) -> Result<&'a serde_json::Map<String, Value>> {
    object
        .get(name)
        .with_context(|| format!("SARIF object is missing required '{name}'"))?
        .as_object()
        .with_context(|| format!("SARIF '{name}' must be an object"))
}

fn required_nonempty_string<'a>(
    object: &'a serde_json::Map<String, Value>,
    name: &str,
) -> Result<&'a str> {
    let value = object
        .get(name)
        .with_context(|| format!("SARIF object is missing required '{name}'"))?
        .as_str()
        .with_context(|| format!("SARIF '{name}' must be a string"))?;
    ensure!(!value.trim().is_empty(), "SARIF '{name}' must not be empty");
    Ok(value)
}

fn write_report(path: &Path, report: &CodeqlReport) -> Result<()> {
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).context("could not create CodeQL report directory")?;
    let name = path.file_name().context("CodeQL report has no file name")?;
    let destination = parent.join(name);
    if let Ok(metadata) = fs::symlink_metadata(&destination) {
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "existing CodeQL report must be a regular file"
        );
    }
    let mut output =
        NamedTempFile::new_in(parent).context("could not create temporary CodeQL report")?;
    serde_json::to_writer_pretty(&mut output, report).context("could not encode CodeQL report")?;
    output
        .write_all(b"\n")
        .context("could not finish CodeQL report")?;
    output.flush().context("could not flush CodeQL report")?;
    output
        .persist(&destination)
        .context("could not publish CodeQL report")?;
    Ok(())
}

#[cfg(test)]
mod tests;
