#[cfg(test)]
mod tests;

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result};
use regex::Regex;
use serde_yaml_ng::Value;
use walkdir::WalkDir;

use crate::process::{CommandSpec, Runner, run_checked};

pub(super) fn check(root: &Path, shellcheck: &Path, runner: &mut dyn Runner) -> Result<()> {
    let temporary = tempfile::tempdir().context("create composite Bash check directory")?;
    let expressions = Regex::new(r"(?s)\$\{\{.*?\}\}")?;
    let mut scripts = Vec::new();
    for directory in [".github", "tools"] {
        for entry in WalkDir::new(root.join(directory)).sort_by_file_name() {
            let entry = entry.context("enumerate repository scripts")?;
            if !entry.file_type().is_file() {
                continue;
            }
            let path = entry.path();
            if path.extension().is_some_and(|extension| extension == "sh") {
                scripts.push(path.to_path_buf());
                continue;
            }
            if !matches!(
                path.file_name().and_then(|name| name.to_str()),
                Some("action.yml" | "action.yaml")
            ) {
                continue;
            }
            let action: Value = serde_yaml_ng::from_str(&fs::read_to_string(path)?)?;
            if action["runs"]["using"].as_str() != Some("composite") {
                continue;
            }
            let steps = action["runs"]["steps"]
                .as_sequence()
                .context("composite action needs steps")?;
            for (index, step) in steps.iter().enumerate() {
                if step["shell"]
                    .as_str()
                    .and_then(|shell| shell.split_ascii_whitespace().next())
                    != Some("bash")
                {
                    continue;
                }
                let script = step["run"]
                    .as_str()
                    .context("Bash step needs a run script")?;
                let name = path
                    .strip_prefix(root)?
                    .to_string_lossy()
                    .replace(['/', '\\'], "_");
                let extracted = temporary
                    .path()
                    .join(format!("{name}-step-{}.bash", index + 1));
                fs::write(
                    &extracted,
                    expressions
                        .replace_all(script, "__GITHUB_EXPRESSION__")
                        .as_bytes(),
                )?;
                scripts.push(extracted);
            }
        }
    }
    if !scripts.is_empty() {
        let mut args = vec![
            OsString::from("--shell=bash"),
            OsString::from("--format=gcc"),
            OsString::from("--"),
        ];
        args.extend(scripts.into_iter().map(PathBuf::into_os_string));
        run_checked(
            runner,
            &CommandSpec {
                program: shellcheck.to_path_buf(),
                args,
                cwd: root.to_path_buf(),
                env: BTreeMap::new(),
                timeout: Duration::from_secs(120),
                output_limit: 4 * 1024 * 1024,
            },
        )
        .context("repository Bash checks failed")?;
    }
    Ok(())
}
