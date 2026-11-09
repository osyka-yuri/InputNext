use std::fs;

use anyhow::{Result, bail};
use inputnext_ci::process::{CommandOutput, CommandSpec, Runner};
use inputnext_ci::release::{QualifyArgs, ReleaseCommand};
use serde_json::Value;

use super::fixtures::{Fixture, REPOSITORY, RUN_ATTEMPT, RUN_ID, SOURCE_SHA, VERSION};

#[derive(Debug)]
struct MockGit {
    head_sha: String,
    tag_sha: String,
    ancestry_status: i32,
    dirty_status: String,
    calls: Vec<Vec<String>>,
}

impl MockGit {
    fn clean() -> Self {
        Self {
            head_sha: SOURCE_SHA.to_owned(),
            tag_sha: SOURCE_SHA.to_owned(),
            ancestry_status: 0,
            dirty_status: String::new(),
            calls: Vec::new(),
        }
    }
}

impl Runner for MockGit {
    fn run(&mut self, spec: &CommandSpec) -> Result<CommandOutput> {
        let args = spec
            .args
            .iter()
            .map(|arg| arg.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        self.calls.push(args.clone());
        let (code, stdout) = match args.first().map(String::as_str) {
            Some("rev-parse") if args.get(1).is_some_and(|value| value == "HEAD") => {
                (0, format!("{}\n", self.head_sha).into_bytes())
            }
            Some("rev-parse") => (0, format!("{}\n", self.tag_sha).into_bytes()),
            Some("merge-base") => (self.ancestry_status, Vec::new()),
            Some("status") => (0, self.dirty_status.as_bytes().to_vec()),
            _ => bail!("unexpected mock Git command: {args:?}"),
        };
        Ok(CommandOutput {
            code: Some(code),
            stdout,
            stderr: Vec::new(),
        })
    }
}

fn qualify(fixture: &Fixture, runner: &mut dyn Runner) -> Result<()> {
    fs::create_dir(fixture.source_root.join(".git"))?;
    let git = fixture.path("git");
    fs::write(&git, b"controlled fake git executable")?;
    ReleaseCommand::Qualify(QualifyArgs {
        event_name: "push".to_owned(),
        repository: REPOSITORY.to_owned(),
        source_commit: SOURCE_SHA.to_owned(),
        source_ref: format!("refs/tags/v{VERSION}"),
        tag: format!("v{VERSION}"),
        run_id: RUN_ID.to_owned(),
        run_attempt: RUN_ATTEMPT.to_owned(),
        workflow_commit: SOURCE_SHA.to_owned(),
        source_root: fixture.source_root.clone(),
        git,
        out_identity: fixture.path("qualified-identity.json"),
        github_output: fixture.path("github-output"),
    })
    .run(runner)
}

#[test]
fn qualifies_only_a_peeled_main_ancestor_from_a_clean_tag_push() -> Result<()> {
    let fixture = Fixture::new()?;
    let mut runner = MockGit::clean();
    qualify(&fixture, &mut runner)?;
    let identity: Value =
        serde_json::from_slice(&fs::read(fixture.path("qualified-identity.json"))?)?;
    assert_eq!(identity["tag"], format!("v{VERSION}"));
    assert_eq!(
        fs::read_to_string(fixture.path("github-output"))?,
        format!("version={VERSION}\n")
    );
    assert_eq!(runner.calls.len(), 4);

    let fixture = Fixture::new()?;
    let mut moved_tag = MockGit::clean();
    moved_tag.tag_sha = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb".to_owned();
    assert!(qualify(&fixture, &mut moved_tag).is_err());
    assert!(!fixture.path("qualified-identity.json").exists());

    let fixture = Fixture::new()?;
    let mut not_on_main = MockGit::clean();
    not_on_main.ancestry_status = 1;
    assert!(qualify(&fixture, &mut not_on_main).is_err());

    let fixture = Fixture::new()?;
    let mut dirty = MockGit::clean();
    dirty.dirty_status = " M CMakeLists.txt\n".to_owned();
    assert!(qualify(&fixture, &mut dirty).is_err());
    Ok(())
}
