//! Repository-specific workflow syntax and authority checks.

mod shell;

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use clap::Args;
use serde_yaml_ng::Value;

use crate::process::{CommandSpec, Runner, run_checked};
use crate::tooling::load_toolset;

const WORKFLOWS: [&str; 6] = [
    "ci.yml",
    "quality.yml",
    "codeql.yml",
    "release-build.yml",
    "release.yml",
    "tool-updates.yml",
];

// actionlint 1.7.12 predates GitHub's defining-workflow job context.
const JOB_CONTEXT_DIAGNOSTIC: &str = r#"^property "(workflow_sha|workflow_repository)" is not defined in object type \{check_run_id:"#;

/// Validate workflow syntax and the repository's release authority boundaries.
#[derive(Debug, Args)]
pub struct WorkflowArgs {
    #[arg(long)]
    root: PathBuf,
    #[arg(long)]
    toolset: PathBuf,
}

impl WorkflowArgs {
    /// Run repository policy checks and the verified actionlint executable.
    ///
    /// # Errors
    /// Returns an error for invalid workflow wiring or failed actionlint execution.
    pub fn run(self, runner: &mut dyn Runner) -> Result<()> {
        let root = dunce::canonicalize(&self.root).context("resolve repository root")?;
        check_repository(&root)?;
        let tools = load_toolset(&self.toolset)?;
        run_checked(
            runner,
            &CommandSpec {
                program: tools.executable("actionlint")?.to_path_buf(),
                args: vec![
                    OsString::from(format!(
                        "-shellcheck={}",
                        tools.executable("shellcheck")?.display()
                    )),
                    OsString::from("-ignore"),
                    OsString::from(JOB_CONTEXT_DIAGNOSTIC),
                ],
                cwd: root.clone(),
                env: BTreeMap::new(),
                timeout: Duration::from_secs(120),
                output_limit: 4 * 1024 * 1024,
            },
        )?;
        shell::check(&root, tools.executable("shellcheck")?, runner)?;
        println!("Workflow, Bash and release authority checks passed.");
        Ok(())
    }
}

/// Check the six maintained workflows without running external tools.
///
/// # Errors
/// Returns an error for missing files, invalid YAML or unsafe release wiring.
pub fn check_repository(root: &Path) -> Result<()> {
    for name in WORKFLOWS {
        let path = root.join(".github/workflows").join(name);
        let text = fs::read_to_string(&path).with_context(|| format!("read {}", path.display()))?;
        let workflow: Value = serde_yaml_ng::from_str(&text).context("parse workflow YAML")?;
        check_workflow(name, &workflow)?;
    }
    check_bootstrap(root)?;
    Ok(())
}

fn check_bootstrap(root: &Path) -> Result<()> {
    let source: Value = serde_yaml_ng::from_str(&fs::read_to_string(
        root.join(".github/actions/source/action.yml"),
    )?)?;
    let guard = &source["runs"]["steps"][0];
    for (key, context) in [
        ("SOURCE_SHA", "${{ github.sha }}"),
        ("SOURCE_REF", "${{ github.ref }}"),
        ("SOURCE_TAG", "${{ github.ref_name }}"),
        ("SOURCE_REPOSITORY", "${{ github.repository }}"),
        ("CALLER_SHA", "${{ github.workflow_sha }}"),
        ("WORKFLOW_SHA", "${{ job.workflow_sha }}"),
        ("WORKFLOW_REPOSITORY", "${{ job.workflow_repository }}"),
        ("EVENT", "${{ github.event_name }}"),
        ("REF_TYPE", "${{ github.ref_type }}"),
    ] {
        ensure!(
            guard["env"][key].as_str() == Some(context),
            "bootstrap must obtain {key} from workflow context"
        );
    }
    ensure!(
        guard["run"].as_str() == Some("bash \"$GITHUB_ACTION_PATH/qualify.sh\""),
        "source guard must run the reviewed script"
    );
    ensure!(
        root.join(".github/actions/source/qualify.sh").is_file(),
        "source guard is missing"
    );
    Ok(())
}

fn check_workflow(name: &str, workflow: &Value) -> Result<()> {
    let events = workflow["on"]
        .as_mapping()
        .context("workflow needs explicit events")?;
    for forbidden in ["pull_request_target", "workflow_run"] {
        ensure!(
            !events.contains_key(Value::from(forbidden)),
            "{name}: unsupported authority event {forbidden}"
        );
    }
    let defaults = workflow["permissions"]
        .as_mapping()
        .context("workflow needs explicit default permissions")?;
    ensure!(
        defaults.len() == 1 && workflow["permissions"]["contents"].as_str() == Some("read"),
        "{name}: workflow defaults must grant only contents read"
    );
    let jobs = workflow["jobs"]
        .as_mapping()
        .context("workflow needs jobs")?;
    for (job_name, job) in jobs {
        let job_name = job_name
            .as_str()
            .context("job identifier must be a string")?;
        let signing = name == "release-build.yml" && job_name == "sign";
        let writing = name == "release.yml" && job_name == "draft";
        let forwarding = name == "release.yml" && job_name == "build-and-attest";
        if forwarding {
            ensure!(
                job["uses"].as_str() == Some("./.github/workflows/release-build.yml"),
                "release authority may only be forwarded to the local builder workflow"
            );
        }
        if signing || forwarding {
            ensure!(
                job["permissions"]["id-token"].as_str() == Some("write")
                    && job["permissions"]["attestations"].as_str() == Some("write"),
                "signing requires its explicit narrow permissions"
            );
        }
        if writing {
            ensure!(
                job["permissions"]["contents"].as_str() == Some("write"),
                "draft writer needs explicit contents permission"
            );
        }
        if let Some(permissions) = job["permissions"].as_mapping() {
            for (permission, level) in permissions {
                let permission = permission.as_str().context("permission must be a string")?;
                let level = level
                    .as_str()
                    .context("permission level must be a string")?;
                if level == "write" {
                    ensure!(
                        match permission {
                            "contents" => writing,
                            "id-token" | "attestations" => signing || forwarding,
                            "security-events" =>
                                name == "codeql.yml"
                                    || name == "quality.yml"
                                    || name == "ci.yml"
                                    || name == "release.yml",
                            _ => false,
                        },
                        "{name}/{job_name}: unexpected write permission {permission}"
                    );
                }
            }
        }
        check_steps(
            name,
            job_name,
            job,
            signing || writing || (name == "release.yml" && job_name == "verify"),
        )?;
    }
    Ok(())
}

fn check_steps(name: &str, job_name: &str, job: &Value, privileged: bool) -> Result<()> {
    let Some(steps) = job["steps"].as_sequence() else {
        ensure!(
            job["uses"].as_str().is_some(),
            "{name}/{job_name}: missing steps or reusable workflow"
        );
        return Ok(());
    };
    let mut source_checked = false;
    let mut trusted_bootstrap = false;
    for step in steps {
        if step["id"].as_str() == Some("source") {
            source_checked = step["uses"].as_str() == Some("./trusted/.github/actions/source");
        }
        let Some(action) = step["uses"].as_str() else {
            continue;
        };
        if action.starts_with("./") {
            ensure!(
                !action.contains("..") && !action.contains('\\'),
                "unsafe local action path"
            );
        } else {
            let (_, revision) = action
                .rsplit_once('@')
                .context("external action must be pinned")?;
            ensure!(
                revision.len() == 40
                    && revision
                        .bytes()
                        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase()),
                "{name}: action needs a full lowercase commit SHA"
            );
        }
        if action.starts_with("actions/checkout@") {
            ensure!(
                step["with"]["persist-credentials"].as_bool() == Some(false),
                "{name}/{job_name}: checkout retains credentials"
            );
        }
        if action.starts_with("actions/download-artifact@") {
            ensure!(
                step["with"]["artifact-ids"].as_str().is_some()
                    && step["with"]["digest-mismatch"].as_str() == Some("error"),
                "{name}/{job_name}: artifact download must use immutable IDs and reject digest drift"
            );
        }
        if privileged {
            ensure!(
                !action.starts_with("actions/cache"),
                "privileged jobs must not restore executable caches"
            );
            if action.ends_with("/.github/actions/bootstrap") {
                ensure!(
                    source_checked
                        && action == "./trusted/.github/actions/bootstrap"
                        && step["with"]["source"].as_str() == Some("trusted")
                        && step["with"]["cache"].as_str() == Some("false"),
                    "release helper must be compiled from qualified workflow source without executable caches"
                );
                trusted_bootstrap = true;
            }
        }
    }
    if privileged {
        ensure!(
            source_checked && trusted_bootstrap,
            "{name}/{job_name}: missing qualified source bootstrap"
        );
        ensure!(
            job["if"].as_str().is_some(),
            "privileged job requires an explicit admission condition"
        );
    }
    Ok(())
}
