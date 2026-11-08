//! Complete, bounded GitHub release enumeration for `CodeQL` update proposals.

use std::collections::BTreeSet;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail, ensure};
use serde::Deserialize;

use crate::process::Runner;

use super::{QUERY_TIMEOUT, fetch_official_with_timeout};

const PAGE_SIZE: usize = 100;
const MAX_PAGES: usize = 100;
const MAX_RESPONSE_BYTES: usize = 128 * 1024 * 1024;
const ENUMERATION_TIMEOUT: Duration = Duration::from_mins(5);

#[derive(Debug, Deserialize)]
pub(super) struct Release {
    pub id: Option<u64>,
    pub draft: Option<bool>,
    pub prerelease: Option<bool>,
    pub tag_name: Option<String>,
    pub html_url: Option<String>,
}

pub(super) fn codeql_releases(runner: &mut dyn Runner) -> Result<Vec<Release>> {
    let started = Instant::now();
    let mut releases = Vec::new();
    let mut seen = BTreeSet::new();
    let mut total_bytes = 0_usize;
    for page in 1..=MAX_PAGES {
        let remaining = ENUMERATION_TIMEOUT.saturating_sub(started.elapsed());
        ensure!(!remaining.is_zero(), "CodeQL release enumeration timed out");
        let url = format!(
            "https://api.github.com/repos/github/codeql-action/releases?per_page={PAGE_SIZE}&page={page}"
        );
        let bytes = fetch_official_with_timeout(runner, &url, true, remaining.min(QUERY_TIMEOUT))
            .with_context(|| format!("could not read CodeQL release page {page}"))?;
        ensure!(
            started.elapsed() <= ENUMERATION_TIMEOUT,
            "CodeQL release enumeration timed out"
        );
        total_bytes = total_bytes
            .checked_add(bytes.len())
            .context("CodeQL release metadata size overflow")?;
        ensure!(
            total_bytes <= MAX_RESPONSE_BYTES,
            "CodeQL release metadata exceeds the total size limit"
        );
        let entries: Vec<Release> = serde_json::from_slice(&bytes)
            .with_context(|| format!("CodeQL release page {page} is not a valid release array"))?;
        ensure!(
            entries.len() <= PAGE_SIZE,
            "CodeQL release page exceeds the requested page size"
        );
        for entry in &entries {
            let id = entry
                .id
                .filter(|id| *id > 0)
                .context("CodeQL release has no positive release ID")?;
            ensure!(
                seen.insert(id),
                "CodeQL release pages contain a duplicate release ID"
            );
        }
        let last_page = entries.len() < PAGE_SIZE;
        releases.extend(entries);
        if last_page {
            return Ok(releases);
        }
    }
    bail!("CodeQL release enumeration exceeds the page limit; no update report was produced")
}

#[cfg(test)]
mod tests;
