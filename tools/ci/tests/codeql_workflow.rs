use std::fs;
use std::path::Path;

use anyhow::{Context, Result};
use serde_yaml_ng::Value;

const UPLOAD_POLICY: &str = "${{ github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name != github.repository && 'never' || 'always' }}";

fn uses(step: &Value, prefix: &str) -> bool {
    step["uses"]
        .as_str()
        .is_some_and(|name| name.starts_with(prefix))
}

fn succeeds_normally(step: &Value) -> bool {
    let continue_on_error = &step["continue-on-error"];
    let condition = &step["if"];
    (continue_on_error.is_null() || continue_on_error.as_bool() == Some(false))
        && (condition.is_null() || condition.as_str() == Some("success()"))
}

#[test]
fn both_codeql_jobs_use_verified_tools_explicit_upload_modes_and_success_gates() -> Result<()> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../.github/workflows/codeql.yml");
    let workflow: Value = serde_yaml_ng::from_str(&fs::read_to_string(path)?)?;
    for job_name in ["actions", "cpp"] {
        let steps = workflow["jobs"][job_name]["steps"]
            .as_sequence()
            .context("CodeQL steps")?;
        let setup = steps
            .iter()
            .find(|step| step["id"].as_str() == Some("setup"))
            .context("toolchain setup")?;
        assert_eq!(setup["with"]["profile"].as_str(), Some("codeql"));
        let init = steps
            .iter()
            .position(|step| uses(step, "github/codeql-action/init@"))
            .context("CodeQL init")?;
        assert_eq!(
            steps[init]["with"]["tools"].as_str(),
            Some("${{ steps.bundle.outputs.path }}")
        );
        let analysis = steps
            .iter()
            .position(|step| uses(step, "github/codeql-action/analyze@"))
            .context("CodeQL analysis")?;
        let gate = steps
            .iter()
            .position(|step| {
                step["run"]
                    .as_str()
                    .is_some_and(|script| script.contains("codeql --results"))
            })
            .context("local SARIF gate")?;
        assert!(gate > analysis);
        assert!(succeeds_normally(&steps[analysis]));
        assert!(succeeds_normally(&steps[gate]));
        assert_eq!(
            steps[analysis]["with"]["upload"].as_str(),
            Some(UPLOAD_POLICY)
        );
        if job_name == "cpp" {
            let native_step = |check: &str| {
                steps.iter().position(|step| {
                    step["run"].as_str().is_some_and(|script| {
                        script.contains(&format!("-Check {check} -Architecture "))
                    })
                })
            };
            let prepare = native_step("codeql-dependencies").context("Catch2 build")?;
            let capture = native_step("codeql").context("C++ capture")?;
            assert_eq!(steps[init]["with"]["build-mode"].as_str(), Some("manual"));
            assert!(prepare < init && init < capture && capture < analysis);
            assert!(succeeds_normally(&steps[prepare]));
            assert!(succeeds_normally(&steps[capture]));
        }
    }
    Ok(())
}
