use std::collections::VecDeque;
use std::fs;

use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};

use crate::process::{CommandOutput, CommandSpec, Runner};
use crate::updates::sources::github_bundle_release;

use super::{MAX_PAGES, PAGE_SIZE, codeql_releases};

#[derive(Debug)]
struct PageRunner {
    pages: VecDeque<Vec<u8>>,
    calls: usize,
    failure_at: Option<usize>,
}

impl PageRunner {
    fn new(pages: &[Value]) -> Result<Self> {
        Ok(Self {
            pages: pages
                .iter()
                .map(serde_json::to_vec)
                .collect::<Result<VecDeque<_>, _>>()?,
            calls: 0,
            failure_at: None,
        })
    }
}

impl Runner for PageRunner {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        self.calls += 1;
        let expected_url = format!(
            "https://api.github.com/repos/github/codeql-action/releases?per_page=100&page={}",
            self.calls
        );
        ensure!(
            spec.args
                .last()
                .is_some_and(|arg| arg.to_str() == Some(expected_url.as_str())),
            "unexpected release page URL"
        );
        let output_index = spec
            .args
            .iter()
            .position(|arg| arg == "--output")
            .context("metadata command has no output path")?;
        let output = spec
            .args
            .get(output_index + 1)
            .context("metadata output argument is missing")?;
        if self.failure_at == Some(self.calls) {
            return Ok(CommandOutput {
                code: Some(22),
                stdout: Vec::new(),
                stderr: b"later page failed".to_vec(),
            });
        }
        fs::write(
            output,
            self.pages
                .pop_front()
                .context("unexpected extra page read")?,
        )?;
        Ok(CommandOutput {
            code: Some(0),
            stdout: Vec::new(),
            stderr: Vec::new(),
        })
    }
}

fn release(id: usize, tag: &str) -> Value {
    json!({
        "id": id,
        "draft": false,
        "prerelease": false,
        "tag_name": tag,
        "html_url": format!("https://github.com/github/codeql-action/releases/tag/{tag}")
    })
}

fn full_page(first_id: usize) -> Value {
    Value::Array(
        (first_id..first_id + PAGE_SIZE)
            .map(|id| release(id, "v4.30.0"))
            .collect(),
    )
}

#[test]
fn stable_bundle_on_later_page_is_not_hidden_by_action_releases() -> Result<()> {
    let mut runner = PageRunner::new(&[
        full_page(1),
        json!([
            release(101, "codeql-bundle-v2.30.0"),
            {"id": 102, "draft": false, "prerelease": true, "tag_name": "codeql-bundle-v2.31.0-preview"},
            release(103, "codeql-bundle-v2.29.0")
        ]),
    ])?;
    let latest = github_bundle_release(&mut runner, None)?;
    assert_eq!(latest.version, "2.30.0");
    assert_eq!(runner.calls, 2);
    Ok(())
}

#[test]
fn full_last_page_requires_a_following_empty_page() -> Result<()> {
    let mut runner = PageRunner::new(&[full_page(1), json!([])])?;
    assert_eq!(codeql_releases(&mut runner)?.len(), PAGE_SIZE);
    assert_eq!(runner.calls, 2);
    Ok(())
}

#[test]
fn newer_bundle_on_later_page_supersedes_the_first_candidate() -> Result<()> {
    let mut first_page = full_page(1);
    *first_page
        .as_array_mut()
        .and_then(|entries| entries.first_mut())
        .context("fixture has no first release")? = release(1, "codeql-bundle-v2.29.0");
    let mut runner =
        PageRunner::new(&[first_page, json!([release(101, "codeql-bundle-v2.30.0")])])?;
    assert_eq!(github_bundle_release(&mut runner, None)?.version, "2.30.0");
    assert_eq!(runner.calls, 2);
    Ok(())
}

#[test]
fn rejects_repeated_pages_and_invalid_release_ids() -> Result<()> {
    let mut repeated = PageRunner::new(&[full_page(1), json!([release(1, "v4.30.0")])])?;
    assert!(codeql_releases(&mut repeated).is_err());
    for invalid in [json!([{}]), json!([{"id": 0}]), json!([{"id": "1"}])] {
        assert!(codeql_releases(&mut PageRunner::new(&[invalid])?).is_err());
    }
    Ok(())
}

#[test]
fn later_page_process_or_parse_failure_never_returns_partial_releases() -> Result<()> {
    let mut failed = PageRunner::new(&[full_page(1)])?;
    failed.failure_at = Some(2);
    let error = codeql_releases(&mut failed)
        .err()
        .context("failure was accepted")?;
    assert!(format!("{error:#}").contains("later page failed"));
    let mut malformed = PageRunner::new(&[full_page(1)])?;
    malformed.pages.push_back(b"not JSON".to_vec());
    assert!(codeql_releases(&mut malformed).is_err());
    assert!(codeql_releases(&mut PageRunner::new(&[json!({})])?).is_err());
    assert!(codeql_releases(&mut PageRunner::new(&[full_page(1), json!({})])?).is_err());
    Ok(())
}

#[test]
fn excessive_pages_and_page_size_are_rejected() -> Result<()> {
    let mut overfull = full_page(1);
    overfull
        .as_array_mut()
        .context("fixture is not an array")?
        .push(release(101, "v4.30.0"));
    assert!(codeql_releases(&mut PageRunner::new(&[overfull])?).is_err());
    let pages: Vec<_> = (0..MAX_PAGES)
        .map(|index| full_page(index * PAGE_SIZE + 1))
        .collect();
    let mut runner = PageRunner::new(&pages)?;
    assert!(codeql_releases(&mut runner).is_err());
    assert_eq!(runner.calls, MAX_PAGES);
    Ok(())
}
