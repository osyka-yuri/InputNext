//! Native CMake/CTest verification transitions.

mod package;
mod plan;

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use clap::Args;
use serde::Serialize;
use tempfile::NamedTempFile;

use crate::process::Runner;
use crate::tooling::load_toolset;

use self::package::stage_release_packages;
use self::plan::{
    Architecture, NativeCheck, NativeContext, NativePlan, build_commands, default_jobs,
    validate_check_architecture,
};

const REPORT_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Args)]
pub struct NativeArgs {
    #[arg(long, default_value = ".")]
    root: PathBuf,
    #[arg(long)]
    toolset: PathBuf,
    #[arg(long, value_enum)]
    check: NativeCheck,
    #[arg(long, value_enum, default_value = "x64")]
    architecture: Architecture,
    #[arg(long)]
    jobs: Option<usize>,
    #[arg(long)]
    report: Option<PathBuf>,
    #[arg(long)]
    plan: bool,
}

#[derive(Clone, Debug, Serialize)]
struct NativeReport {
    schema_version: u32,
    status: &'static str,
    plan: NativePlan,
    steps: Vec<NativeStep>,
    artifacts: Vec<PathBuf>,
}

#[derive(Clone, Debug, Serialize)]
struct NativeStep {
    label: String,
    code: i32,
}

impl NativeArgs {
    /// Plans or executes a native verification check and reports its outputs.
    ///
    /// # Errors
    ///
    /// Returns an error when the host, toolset, command, package, or report is invalid.
    pub fn run(self, runner: &mut dyn Runner) -> Result<()> {
        ensure!(
            cfg!(windows),
            "native InputNext verification requires a Windows x64 runner"
        );
        ensure!(
            std::env::consts::ARCH == "x86_64",
            "native InputNext verification requires a Windows x64 host"
        );
        if let Some(jobs) = self.jobs {
            ensure!((1..=256).contains(&jobs), "jobs must be between 1 and 256");
        }
        validate_check_architecture(self.check, self.architecture)?;

        let root = dunce::canonicalize(&self.root).with_context(|| {
            format!("could not resolve repository root {}", self.root.display())
        })?;
        ensure!(root.is_dir(), "repository root is not a directory");
        let toolset = load_toolset(&self.toolset)?;
        let cmake = toolset.executable("cmake")?.to_path_buf();
        let powershell = toolset.executable("powershell")?.to_path_buf();
        let jobs = self.jobs.unwrap_or_else(default_jobs);
        let context = NativeContext {
            root: &root,
            powershell: &powershell,
            cmake: &cmake,
            toolset: &toolset,
            jobs,
        };
        let commands = build_commands(self.check, self.architecture, &context)?;
        let plan = NativePlan {
            schema_version: REPORT_SCHEMA_VERSION,
            root: root.clone(),
            check: self.check,
            architecture: self.architecture,
            jobs,
            commands: commands.iter().map(plan::NativeCommand::planned).collect(),
            post_actions: if self.check == NativeCheck::Package {
                vec![
                    "verify runtime and symbols packages".to_owned(),
                    "stage only this architecture's two ZIPs and SHA256SUMS file".to_owned(),
                ]
            } else {
                Vec::new()
            },
        };

        if self.plan {
            if let Some(path) = &self.report {
                write_report(path, &plan)?;
            }
            println!("{}", serde_json::to_string_pretty(&plan)?);
            return Ok(());
        }

        let mut steps = Vec::new();
        for command in &commands {
            let output = runner
                .run(&command.spec)
                .with_context(|| format!("native step '{}' could not complete", command.label))?;
            let Some(code) = output.code else {
                anyhow::bail!(
                    "native step '{}' returned no exit status\n{}",
                    command.label,
                    output.diagnostics()
                );
            };
            ensure!(
                code == 0,
                "native step '{}' exited with status {code}\n{}",
                command.label,
                output.diagnostics()
            );
            steps.push(NativeStep {
                label: command.label.clone(),
                code,
            });
        }

        let artifacts = if self.check == NativeCheck::Package {
            stage_release_packages(&root, self.architecture)?
        } else {
            Vec::new()
        };
        let report = NativeReport {
            schema_version: REPORT_SCHEMA_VERSION,
            status: "passed",
            plan,
            steps,
            artifacts,
        };
        if let Some(path) = &self.report {
            write_report(path, &report)?;
        }
        println!(
            "Native {} check passed for {} with {} step(s)",
            self.check.as_str(),
            self.architecture.as_str(),
            commands.len()
        );
        Ok(())
    }
}

fn write_report<T: Serialize>(path: &Path, report: &T) -> Result<()> {
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).context("could not create native report directory")?;
    let name = path
        .file_name()
        .context("native report path has no file name")?;
    let destination = parent.join(name);
    if let Ok(metadata) = fs::symlink_metadata(&destination) {
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "existing native report must be a regular file"
        );
    }
    let mut temporary =
        NamedTempFile::new_in(parent).context("could not create temporary native report")?;
    serde_json::to_writer_pretty(&mut temporary, report)
        .context("could not encode native report")?;
    temporary
        .write_all(b"\n")
        .context("could not finish native report")?;
    temporary.flush().context("could not flush native report")?;
    temporary
        .persist(&destination)
        .context("could not publish native report")?;
    Ok(())
}
