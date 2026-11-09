//! Source-qualified preparation, attestation verification, and draft writing.

mod assets;
mod attest;
mod draft;
mod identity;
mod package;
mod sbom;
mod strict_json;

use std::path::PathBuf;

use anyhow::Result;
use clap::{Args, Subcommand};

use crate::process::Runner;

#[derive(Debug, Subcommand)]
pub enum ReleaseCommand {
    /// Qualify the event, tag, source checkout, version, and reviewed release notes.
    Qualify(QualifyArgs),
    /// Validate native packages and prepare exact unsigned public/private release data.
    Prepare(PrepareArgs),
    /// Validate the unsigned handoff before signing.
    CheckUnsigned(CheckUnsignedArgs),
    /// Validate the unsigned handoff and combine it with the three attestation bundles.
    Assemble(AssembleArgs),
    /// Verify local assets and GitHub attestations without writing to GitHub.
    Verify(VerifyArgs),
    /// Revalidate and create or complete an exact draft release.
    Draft(DraftArgs),
}

impl ReleaseCommand {
    /// Runs a release operation through the supplied process runner.
    ///
    /// # Errors
    ///
    /// Returns an error when source qualification, artifact validation, attestation
    /// verification, or the requested draft operation fails.
    pub fn run(self, runner: &mut dyn Runner) -> Result<()> {
        match self {
            Self::Qualify(args) => identity::qualify(&args, runner),
            Self::Prepare(args) => assets::prepare(&args),
            Self::CheckUnsigned(args) => assets::check_unsigned(&args),
            Self::Assemble(args) => assets::assemble(args),
            Self::Verify(args) => attest::verify(&args, runner),
            Self::Draft(args) => draft::run(args, runner),
        }
    }
}

#[derive(Debug, Args)]
pub struct QualifyArgs {
    /// GitHub event that triggered the workflow. Only a stable tag push is releasable.
    #[arg(long, env = "GITHUB_EVENT_NAME", default_value = "push")]
    pub event_name: String,
    /// Canonical owner/repository name from the current workflow context.
    #[arg(long, env = "GITHUB_REPOSITORY")]
    pub repository: String,
    /// Full source commit SHA from the current workflow context.
    #[arg(long, env = "GITHUB_SHA")]
    pub source_commit: String,
    /// Full event ref, expected to be refs/tags/<tag>.
    #[arg(long, env = "GITHUB_REF")]
    pub source_ref: String,
    /// Stable vX.Y.Z event tag.
    #[arg(long, env = "GITHUB_REF_NAME")]
    pub tag: String,
    /// Current GitHub run ID.
    #[arg(long, env = "GITHUB_RUN_ID")]
    pub run_id: String,
    /// Current GitHub run attempt.
    #[arg(long, env = "GITHUB_RUN_ATTEMPT")]
    pub run_attempt: String,
    /// Immutable commit of the defining workflow.
    #[arg(long, env = "INPUTNEXT_WORKFLOW_COMMIT")]
    pub workflow_commit: String,
    /// Trusted checkout of the release source.
    #[arg(long)]
    pub source_root: PathBuf,
    /// Absolute path to Git.
    #[arg(long)]
    pub git: PathBuf,
    /// Write the qualified workflow identity to a trusted job-local file.
    #[arg(long)]
    pub out_identity: PathBuf,
    /// Write `version=<X.Y.Z>` for GitHub Actions step outputs.
    #[arg(long, env = "GITHUB_OUTPUT")]
    pub github_output: PathBuf,
}

#[derive(Debug, Args)]
pub struct PrepareArgs {
    /// Independently qualified identity created from current workflow context.
    #[arg(long)]
    pub identity: PathBuf,
    /// Trusted checkout of the source matching the identity.
    #[arg(long)]
    pub source_root: PathBuf,
    /// Native `CMake` package directory containing four ZIPs and two checksum records.
    #[arg(long)]
    pub packages: PathBuf,
    /// New directory for the exact flat unsigned handoff.
    #[arg(long)]
    pub out: PathBuf,
}

#[derive(Debug, Args)]
pub struct CheckUnsignedArgs {
    #[arg(long)]
    pub identity: PathBuf,
    #[arg(long)]
    pub source_root: PathBuf,
    /// Flat nine-file handoff from the builder job.
    #[arg(long)]
    pub unsigned: PathBuf,
}

#[derive(Debug, Args)]
pub struct AssembleArgs {
    #[arg(long)]
    pub identity: PathBuf,
    #[arg(long)]
    pub source_root: PathBuf,
    #[arg(long)]
    pub unsigned: PathBuf,
    /// Action-produced provenance bundle covering all seven unsigned public subjects.
    #[arg(long)]
    pub provenance_bundle: PathBuf,
    /// Action-produced SPDX predicate bundle for the x64 runtime ZIP.
    #[arg(long)]
    pub sbom_x64_bundle: PathBuf,
    /// Action-produced SPDX predicate bundle for the x86 runtime ZIP.
    #[arg(long)]
    pub sbom_x86_bundle: PathBuf,
    /// New root directory containing public/ and internal/ inventories.
    #[arg(long)]
    pub out: PathBuf,
}

#[derive(Debug, Args)]
pub struct VerifyArgs {
    #[arg(long)]
    pub identity: PathBuf,
    #[arg(long)]
    pub source_root: PathBuf,
    /// Root containing the public/ and internal/ handoffs.
    #[arg(long)]
    pub release_dir: PathBuf,
    /// Absolute path to the pinned GitHub CLI 2.103.3 executable.
    #[arg(long)]
    pub gh: PathBuf,
}

#[derive(Debug, Args)]
pub struct DraftArgs {
    #[arg(long)]
    pub identity: PathBuf,
    #[arg(long)]
    pub source_root: PathBuf,
    /// Root containing the public/ and internal/ handoffs.
    #[arg(long)]
    pub release_dir: PathBuf,
    /// Absolute path to the pinned GitHub CLI 2.103.3 executable.
    #[arg(long)]
    pub gh: PathBuf,
    /// Validate all inputs and print a write plan without GitHub mutations.
    #[arg(long)]
    pub dry_run: bool,
}
