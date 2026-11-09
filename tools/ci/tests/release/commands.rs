use anyhow::{Result, bail};
use inputnext_ci::process::{CommandOutput, CommandSpec, Runner};
use inputnext_ci::release::{
    AssembleArgs, CheckUnsignedArgs, DraftArgs, PrepareArgs, ReleaseCommand, VerifyArgs,
};

use super::fixtures::Fixture;

#[derive(Debug, Default)]
struct OfflineRunner;

impl Runner for OfflineRunner {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        bail!(
            "unexpected external command in offline test: {}",
            spec.program.display()
        )
    }
}

pub(super) fn prepare(fixture: &Fixture) -> Result<()> {
    ReleaseCommand::Prepare(PrepareArgs {
        identity: fixture.identity_path.clone(),
        source_root: fixture.source_root.clone(),
        packages: fixture.packages.clone(),
        out: fixture.unsigned_output(),
    })
    .run(&mut OfflineRunner)
}

pub(super) fn assemble(fixture: &Fixture) -> Result<()> {
    let bundles = fixture.write_bundles()?;
    ReleaseCommand::Assemble(AssembleArgs {
        identity: fixture.identity_path.clone(),
        source_root: fixture.source_root.clone(),
        unsigned: fixture.unsigned_output(),
        provenance_bundle: bundles[0].clone(),
        sbom_x64_bundle: bundles[1].clone(),
        sbom_x86_bundle: bundles[2].clone(),
        out: fixture.release_output(),
    })
    .run(&mut OfflineRunner)
}

pub(super) fn check_unsigned(fixture: &Fixture) -> Result<()> {
    ReleaseCommand::CheckUnsigned(CheckUnsignedArgs {
        identity: fixture.identity_path.clone(),
        source_root: fixture.source_root.clone(),
        unsigned: fixture.unsigned_output(),
    })
    .run(&mut OfflineRunner)
}

pub(super) fn draft(fixture: &Fixture, runner: &mut dyn Runner, dry_run: bool) -> Result<()> {
    ReleaseCommand::Draft(DraftArgs {
        identity: fixture.identity_path.clone(),
        source_root: fixture.source_root.clone(),
        release_dir: fixture.release_output(),
        gh: fixture.gh_executable()?,
        dry_run,
    })
    .run(runner)
}

pub(super) fn verify(fixture: &Fixture, runner: &mut dyn Runner) -> Result<()> {
    ReleaseCommand::Verify(VerifyArgs {
        identity: fixture.identity_path.clone(),
        source_root: fixture.source_root.clone(),
        release_dir: fixture.release_output(),
        gh: fixture.gh_executable()?,
    })
    .run(runner)
}
