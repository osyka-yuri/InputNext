use anyhow::Result;

use super::commands::{assemble, draft, prepare};
use super::fixtures::{Fixture, VERSION};
use super::github::{MockGh, RemoteReleaseMutation, RemoteTagMutation};

#[test]
fn dry_run_does_not_write_and_complete_retry_is_noop() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    let mut runner = MockGh::new(&fixture)?;

    draft(&fixture, &mut runner, true)?;
    assert_eq!(runner.release_write_count(), 0);

    runner.calls.clear();
    draft(&fixture, &mut runner, false)?;
    assert_eq!(runner.release_write_count(), 0);
    Ok(())
}

#[test]
fn partial_upload_failure_remains_retryable_without_clobbering() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    let mut runner = MockGh::new(&fixture)?;
    let missing = format!("InputNext-{VERSION}-provenance.sigstore.json");
    runner.omit_asset(&missing);
    runner.upload_failures_remaining = 1;

    assert!(draft(&fixture, &mut runner, false).is_err());
    assert_eq!(runner.command_count("release", Some("upload")), 1);
    assert!(!runner.calls.iter().flatten().any(|arg| arg == "--clobber"));

    runner.calls.clear();
    draft(&fixture, &mut runner, false)?;
    assert_eq!(runner.command_count("release", Some("upload")), 1);
    assert_eq!(runner.remote_assets.len(), 10);
    assert!(!runner.calls.iter().flatten().any(|arg| arg == "--clobber"));
    Ok(())
}

#[test]
fn creates_draft_reads_it_back_by_id_and_finishes_a_partial_retry() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;
    let mut runner = MockGh::new(&fixture)?;
    let failed_asset = runner.begin_without_release()?;
    runner.upload_failure_for = Some(failed_asset.clone());

    assert!(draft(&fixture, &mut runner, false).is_err());
    assert_eq!(runner.command_count("release", Some("create")), 1);
    assert_eq!(runner.command_count("release", Some("upload")), 10);
    assert!(runner.remote_release_exists);
    assert_eq!(runner.remote_assets.len(), 9);
    assert!(runner.calls.iter().any(|args| {
        args.first().is_some_and(|arg| arg == "api")
            && args
                .last()
                .is_some_and(|arg| arg.ends_with("/releases/314"))
    }));

    runner.calls.clear();
    draft(&fixture, &mut runner, false)?;
    assert_eq!(runner.command_count("release", Some("create")), 0);
    assert_eq!(runner.command_count("release", Some("upload")), 1);
    assert!(runner.remote_assets.contains(&failed_asset));
    assert_eq!(runner.remote_assets.len(), 10);
    assert!(!runner.calls.iter().flatten().any(|arg| arg == "--clobber"));

    runner.calls.clear();
    draft(&fixture, &mut runner, false)?;
    assert_eq!(runner.command_count("release", Some("create")), 0);
    assert_eq!(runner.command_count("release", Some("upload")), 0);
    Ok(())
}

#[test]
fn refuses_moved_tags_published_releases_and_conflicting_asset_hashes() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;

    let mut moved = MockGh::new(&fixture)?;
    moved.remote_tag_mutation = RemoteTagMutation::Moved;
    assert!(draft(&fixture, &mut moved, false).is_err());
    assert_eq!(moved.release_write_count(), 0);

    let mut published = MockGh::new(&fixture)?;
    published.remote_release_mutation = RemoteReleaseMutation::Published;
    assert!(draft(&fixture, &mut published, false).is_err());
    assert_eq!(published.release_write_count(), 0);

    let mut wrong_digest = MockGh::new(&fixture)?;
    wrong_digest.remote_release_mutation = RemoteReleaseMutation::WrongAssetDigest;
    assert!(draft(&fixture, &mut wrong_digest, false).is_err());
    assert_eq!(wrong_digest.release_write_count(), 0);

    let mut missing_digest = MockGh::new(&fixture)?;
    missing_digest.remote_release_mutation = RemoteReleaseMutation::MissingAssetDigest;
    assert!(draft(&fixture, &mut missing_digest, false).is_err());
    assert_eq!(missing_digest.release_write_count(), 0);

    let mut wrong_identity = MockGh::new(&fixture)?;
    wrong_identity.remote_release_mutation = RemoteReleaseMutation::WrongIdentity;
    assert!(draft(&fixture, &mut wrong_identity, false).is_err());
    assert_eq!(wrong_identity.release_write_count(), 0);

    let mut lookup_failure = MockGh::new(&fixture)?;
    lookup_failure.remote_release_exists = false;
    lookup_failure.release_lookup_failure = true;
    assert!(draft(&fixture, &mut lookup_failure, false).is_err());
    assert_eq!(lookup_failure.release_write_count(), 0);
    Ok(())
}

#[test]
fn refuses_duplicate_and_unexpected_remote_assets() -> Result<()> {
    let fixture = Fixture::new()?;
    prepare(&fixture)?;
    assemble(&fixture)?;

    let mut duplicate = MockGh::new(&fixture)?;
    duplicate.remote_release_mutation = RemoteReleaseMutation::DuplicateAsset;
    assert!(draft(&fixture, &mut duplicate, false).is_err());

    let mut unexpected = MockGh::new(&fixture)?;
    unexpected.remote_release_mutation = RemoteReleaseMutation::UnexpectedAsset;
    assert!(draft(&fixture, &mut unexpected, false).is_err());
    Ok(())
}
