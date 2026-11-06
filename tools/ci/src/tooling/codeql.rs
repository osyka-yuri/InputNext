//! Verified bundle handoff to the `CodeQL` Action's local archive input.

use std::fs::File;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use tempfile::Builder;

use crate::process::Runner;

use super::archive::{self, ArchiveFormat};
use super::download;
use super::lock::DependencyDefinition;

pub(super) fn install_bundle(
    runner: &mut dyn Runner,
    dependency: &DependencyDefinition,
    cache_root: &Path,
    destination: &Path,
) -> Result<PathBuf> {
    ensure!(
        dependency.expected_paths.is_empty() && dependency.extract_root == ".",
        "CodeQL bundle lock must expose the raw archive without an extraction contract"
    );
    ensure!(
        dependency.archive == "tar.gz",
        "CodeQL bundle must use tar.gz"
    );
    let cached = download::verified_archive(
        runner,
        cache_root,
        &dependency.url,
        &dependency.sha256,
        &dependency.archive,
    )?;

    // The Action infers compression from the filename, unlike the shared cache.
    let mut bundle = Builder::new()
        .prefix("codeql-bundle-")
        .suffix(".tar.gz")
        .tempfile_in(destination)
        .context("could not create the job-local CodeQL archive")?;
    io::copy(&mut File::open(cached)?, &mut bundle)
        .context("could not copy the verified CodeQL archive")?;
    bundle
        .flush()
        .context("could not flush the CodeQL archive")?;
    ensure!(
        download::hash_file(bundle.path())? == dependency.sha256,
        "CodeQL archive changed while preparing its handoff"
    );
    archive::inspect(bundle.path(), ArchiveFormat::TarGz)?;
    let path = bundle
        .into_temp_path()
        .keep()
        .context("could not preserve the verified CodeQL archive")?;
    Ok(path)
}

#[cfg(test)]
mod tests;
