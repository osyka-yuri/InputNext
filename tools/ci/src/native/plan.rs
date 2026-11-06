//! Pure mapping from a native check to its executable command sequence.

use std::collections::BTreeMap;
use std::env;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, ensure};
use clap::ValueEnum;
use serde::Serialize;

use crate::process::CommandSpec;
use crate::tooling::Toolset;

use super::package::project_version;

pub(super) const NATIVE_TIMEOUT: Duration = Duration::from_hours(2);
pub(super) const NATIVE_OUTPUT_LIMIT: usize = 64 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq, ValueEnum, Serialize)]
#[serde(rename_all = "snake_case")]
pub(super) enum NativeCheck {
    Tests,
    Quality,
    Package,
    Analyze,
    Asan,
    Ubsan,
    CodeqlDependencies,
    Codeql,
    Stress,
}

impl NativeCheck {
    pub(super) fn as_str(self) -> &'static str {
        match self {
            Self::Tests => "tests",
            Self::Quality => "quality",
            Self::Package => "package",
            Self::Analyze => "analyze",
            Self::Asan => "asan",
            Self::Ubsan => "ubsan",
            Self::CodeqlDependencies => "codeql-dependencies",
            Self::Codeql => "codeql",
            Self::Stress => "stress",
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, ValueEnum, Serialize)]
#[serde(rename_all = "snake_case")]
pub(super) enum Architecture {
    X64,
    X86,
}

impl Architecture {
    pub(super) fn as_str(self) -> &'static str {
        match self {
            Self::X64 => "x64",
            Self::X86 => "x86",
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub(super) struct NativePlan {
    pub schema_version: u32,
    pub root: PathBuf,
    pub check: NativeCheck,
    pub architecture: Architecture,
    pub jobs: usize,
    pub commands: Vec<PlannedCommand>,
    pub post_actions: Vec<String>,
}

#[derive(Clone, Debug, Serialize)]
pub(super) struct PlannedCommand {
    pub label: String,
    pub program: PathBuf,
    pub args: Vec<String>,
    pub cwd: PathBuf,
}

#[derive(Debug)]
pub(super) struct NativeCommand {
    pub label: String,
    pub spec: CommandSpec,
}

impl NativeCommand {
    pub(super) fn new(
        label: impl Into<String>,
        program: PathBuf,
        args: Vec<OsString>,
        cwd: &Path,
        env: BTreeMap<OsString, OsString>,
    ) -> Self {
        Self {
            label: label.into(),
            spec: CommandSpec {
                program,
                args,
                cwd: cwd.to_path_buf(),
                env,
                timeout: NATIVE_TIMEOUT,
                output_limit: NATIVE_OUTPUT_LIMIT,
            },
        }
    }

    pub(super) fn planned(&self) -> PlannedCommand {
        PlannedCommand {
            label: self.label.clone(),
            program: self.spec.program.clone(),
            args: self
                .spec
                .args
                .iter()
                .map(|argument| argument.to_string_lossy().into_owned())
                .collect(),
            cwd: self.spec.cwd.clone(),
        }
    }
}

pub(super) struct NativeContext<'a> {
    pub root: &'a Path,
    pub powershell: &'a Path,
    pub cmake: &'a Path,
    pub toolset: &'a Toolset,
    pub jobs: usize,
}

pub(super) fn validate_check_architecture(
    check: NativeCheck,
    architecture: Architecture,
) -> Result<()> {
    ensure!(
        !matches!(
            check,
            NativeCheck::Quality | NativeCheck::Analyze | NativeCheck::Ubsan
        ) || architecture == Architecture::X64,
        "the '{}' check is supported only for x64",
        check.as_str()
    );
    Ok(())
}

pub(super) fn build_commands(
    check: NativeCheck,
    architecture: Architecture,
    context: &NativeContext<'_>,
) -> Result<Vec<NativeCommand>> {
    let environment = command_environment(check, context.jobs, context.toolset)?;
    match check {
        NativeCheck::Tests
        | NativeCheck::Quality
        | NativeCheck::Analyze
        | NativeCheck::Asan
        | NativeCheck::Ubsan
        | NativeCheck::Stress => Ok(vec![verification_command(
            check,
            architecture,
            context,
            environment,
        )]),
        NativeCheck::CodeqlDependencies | NativeCheck::Codeql => {
            Ok(codeql_commands(check, architecture, context, environment))
        }
        NativeCheck::Package => package_commands(architecture, context, environment),
    }
}

fn verification_command(
    check: NativeCheck,
    architecture: Architecture,
    context: &NativeContext<'_>,
    environment: BTreeMap<OsString, OsString>,
) -> NativeCommand {
    let mut args = vec![
        "-NoLogo".into(),
        "-NoProfile".into(),
        "-NonInteractive".into(),
        "-File".into(),
        context.root.join("tools/verify.ps1").into_os_string(),
        "-Architecture".into(),
        architecture.as_str().into(),
    ];
    match check {
        NativeCheck::Quality => args.push("-CheckQuality".into()),
        NativeCheck::Analyze => args.extend(["-NativeProfile".into(), "Analyze".into()]),
        NativeCheck::Asan => args.extend(["-NativeProfile".into(), "Address".into()]),
        NativeCheck::Ubsan => args.extend(["-NativeProfile".into(), "Undefined".into()]),
        NativeCheck::Stress => args.push("-IncludeStress".into()),
        NativeCheck::Tests
        | NativeCheck::Package
        | NativeCheck::CodeqlDependencies
        | NativeCheck::Codeql => {}
    }
    NativeCommand::new(
        check.as_str(),
        context.powershell.to_path_buf(),
        args,
        context.root,
        environment,
    )
}

fn codeql_commands(
    check: NativeCheck,
    architecture: Architecture,
    context: &NativeContext<'_>,
    environment: BTreeMap<OsString, OsString>,
) -> Vec<NativeCommand> {
    let preset = codeql_preset(architecture);
    let mut build_args = vec![
        "--build".into(),
        "--preset".into(),
        preset.clone().into(),
        "--parallel".into(),
        context.jobs.to_string().into(),
    ];
    let build_label = if check == NativeCheck::CodeqlDependencies {
        build_args.extend([OsString::from("--target"), "Catch2WithMain".into()]);
        "build-codeql-dependencies"
    } else {
        "build-release-capture"
    };
    vec![
        NativeCommand::new(
            "configure-release-capture",
            context.cmake.to_path_buf(),
            vec!["--preset".into(), preset.into()],
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            build_label,
            context.cmake.to_path_buf(),
            build_args,
            context.root,
            environment,
        ),
    ]
}

fn codeql_preset(architecture: Architecture) -> String {
    format!("windows-{}-release", architecture.as_str())
}

fn package_commands(
    architecture: Architecture,
    context: &NativeContext<'_>,
    environment: BTreeMap<OsString, OsString>,
) -> Result<Vec<NativeCommand>> {
    let arch = architecture.as_str();
    let preset = format!("windows-{arch}-production");
    let build_directory = context.root.join("build").join(&preset);
    let version = project_version(context.root)?;
    let package_name = format!("InputNext-{version}-windows-{arch}");
    let runtime_package = build_directory
        .join("packages")
        .join(format!("{package_name}.zip"));
    let symbols_package = build_directory
        .join("packages")
        .join(format!("{package_name}-symbols.zip"));
    let verify_script = context.root.join("tools/verify_package.ps1");
    let mut runtime_args =
        package_verify_arguments(&verify_script, &runtime_package, &build_directory, arch);
    runtime_args.push("-RunTrustedChecks".into());
    let mut symbols_args =
        package_verify_arguments(&verify_script, &symbols_package, &build_directory, arch);
    symbols_args.push("-Symbols".into());

    let trusted_checks_configuration = vec![
        "--preset".into(),
        format!("windows-{arch}-production").into(),
        "-DBUILD_TESTING=ON".into(),
        "-DINPUTNEXT_BUILD_SDK_EXAMPLES=ON".into(),
    ];

    Ok(vec![
        NativeCommand::new(
            "configure-production",
            context.cmake.to_path_buf(),
            vec!["--preset".into(), preset.clone().into()],
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            "build-production-package",
            context.cmake.to_path_buf(),
            vec![
                "--build".into(),
                "--preset".into(),
                preset.into(),
                "--parallel".into(),
                context.jobs.to_string().into(),
            ],
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            "configure-trusted-package-checks",
            context.cmake.to_path_buf(),
            trusted_checks_configuration,
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            "build-trusted-package-checks",
            context.cmake.to_path_buf(),
            vec![
                "--build".into(),
                "--preset".into(),
                format!("windows-{arch}-production").into(),
                "--target".into(),
                "inputnext_distribution_checks".into(),
                "--parallel".into(),
                context.jobs.to_string().into(),
            ],
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            "verify-runtime-package",
            context.powershell.to_path_buf(),
            runtime_args,
            context.root,
            environment.clone(),
        ),
        NativeCommand::new(
            "verify-symbols-package",
            context.powershell.to_path_buf(),
            symbols_args,
            context.root,
            environment,
        ),
    ])
}

fn package_verify_arguments(
    script: &Path,
    package: &Path,
    build_directory: &Path,
    architecture: &str,
) -> Vec<OsString> {
    vec![
        "-NoLogo".into(),
        "-NoProfile".into(),
        "-NonInteractive".into(),
        "-File".into(),
        script.as_os_str().to_owned(),
        "-Package".into(),
        package.as_os_str().to_owned(),
        "-BuildDirectory".into(),
        build_directory.as_os_str().to_owned(),
        "-Architecture".into(),
        architecture.into(),
    ]
}

fn command_environment(
    check: NativeCheck,
    jobs: usize,
    toolset: &Toolset,
) -> Result<BTreeMap<OsString, OsString>> {
    let mut values = BTreeMap::new();
    if let Some(verified_path) = toolset.environment().get("PATH") {
        let mut paths = env::split_paths(OsStr::new(verified_path)).collect::<Vec<_>>();
        if let Some(current_path) = env::var_os("PATH") {
            paths.extend(env::split_paths(&current_path));
        }
        values.insert(
            OsString::from("PATH"),
            env::join_paths(paths).context("could not combine verified tool and process PATH")?,
        );
    }
    if let Some(verified_module_path) = toolset.environment().get("PSModulePath") {
        let mut paths = env::split_paths(OsStr::new(verified_module_path)).collect::<Vec<_>>();
        if let Some(current_module_path) = env::var_os("PSModulePath") {
            paths.extend(env::split_paths(&current_module_path));
        }
        values.insert(
            OsString::from("PSModulePath"),
            env::join_paths(paths).context("could not combine PowerShell module paths")?,
        );
    }
    if matches!(check, NativeCheck::CodeqlDependencies | NativeCheck::Codeql) {
        for name in ["INPUTNEXT_CODEQL_BUNDLE", "INPUTNEXT_CODEQL_BUNDLE_VERSION"] {
            if let Some(value) = toolset.environment().get(name) {
                values.insert(name.into(), value.into());
            }
        }
    }
    values.insert(
        OsString::from("CMAKE_BUILD_PARALLEL_LEVEL"),
        jobs.to_string().into(),
    );
    Ok(values)
}

pub(super) fn default_jobs() -> usize {
    std::thread::available_parallelism()
        .map_or(1, std::num::NonZero::get)
        .min(256)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::path::{Path, PathBuf};

    use anyhow::Result;

    use crate::tooling::{Toolset, load_toolset};

    use super::{Architecture, NativeCheck, NativeContext, build_commands, package_commands};

    #[test]
    fn codeql_prebuild_uses_the_capture_preset_and_builds_only_catch2() -> Result<()> {
        let temporary = tempfile::tempdir()?;
        let toolset = fixture_toolset(temporary.path())?;
        let context = NativeContext {
            root: temporary.path(),
            powershell: Path::new("pwsh.exe"),
            cmake: Path::new("cmake.exe"),
            toolset: &toolset,
            jobs: 16,
        };
        for architecture in [Architecture::X64, Architecture::X86] {
            let preset = format!("windows-{}-release", architecture.as_str());
            let prepare = build_commands(NativeCheck::CodeqlDependencies, architecture, &context)?;
            let capture = build_commands(NativeCheck::Codeql, architecture, &context)?;
            assert_eq!(prepare.len(), 2);
            assert_eq!(capture.len(), 2);
            assert_eq!(prepare[0].spec.args, ["--preset", &preset]);
            assert_eq!(prepare[0].spec.args, capture[0].spec.args);
            assert_eq!(
                prepare[1].spec.args,
                [
                    "--build",
                    "--preset",
                    &preset,
                    "--parallel",
                    "16",
                    "--target",
                    "Catch2WithMain"
                ]
            );
            assert_eq!(capture[1].spec.args, prepare[1].spec.args[..5]);
        }
        Ok(())
    }

    fn has_arg(command: &super::NativeCommand, expected: &str) -> bool {
        command.spec.args.iter().any(|arg| arg == expected)
    }

    fn assert_trusted_package_commands(
        commands: &[super::NativeCommand],
        cmake: &Path,
        arch: &str,
    ) -> Result<()> {
        let command = |label: &str| {
            commands
                .iter()
                .enumerate()
                .find(|(_, planned)| planned.label == label)
                .ok_or_else(|| anyhow::anyhow!("missing {label} command"))
        };
        let (production_index, production_build) = command("build-production-package")?;
        let (configure_index, configure) = command("configure-trusted-package-checks")?;
        let (build_index, build) = command("build-trusted-package-checks")?;
        let (runtime_index, verify_runtime) = command("verify-runtime-package")?;
        let (symbols_index, verify_symbols) = command("verify-symbols-package")?;

        let preset = format!("windows-{arch}-production");
        assert!(has_arg(production_build, &preset));
        assert_eq!(configure.spec.program, cmake);
        assert!(has_arg(configure, &preset));
        assert!(has_arg(configure, "-DBUILD_TESTING=ON"));
        assert!(has_arg(configure, "-DINPUTNEXT_BUILD_SDK_EXAMPLES=ON"));
        assert!(has_arg(build, "inputnext_distribution_checks"));
        assert!(has_arg(verify_runtime, "-RunTrustedChecks"));
        assert!(has_arg(verify_symbols, "-Symbols"));
        assert!(production_index < configure_index);
        assert!(configure_index < build_index);
        assert!(build_index < runtime_index);
        assert!(runtime_index < symbols_index);
        Ok(())
    }

    fn fixture_toolset(root: &Path) -> Result<Toolset> {
        let tool_path = root.join("powershell.exe");
        std::fs::write(&tool_path, "fixture")?;
        let tool_sha256 = "0".repeat(64);
        let report_path = root.join("toolset.json");
        std::fs::write(
            &report_path,
            serde_json::to_vec(&serde_json::json!({
                "schema_version": 1,
                "profile": "build",
                "platform": "windows-x64",
                "lock_sha256": "0".repeat(64),
                "tools": {
                    "powershell": {
                        "version": "7.6.9",
                        "path": tool_path,
                        "kind": "executable",
                        "archive": null,
                        "sha256": tool_sha256,
                    }
                },
                "environment": {},
            }))?,
        )?;
        load_toolset(&report_path)
    }

    #[test]
    fn production_package_plan_builds_trusted_distribution_checks_before_verification() -> Result<()>
    {
        let temporary = tempfile::tempdir()?;
        let root = temporary.path();
        std::fs::write(
            root.join("CMakeLists.txt"),
            "project(InputNext VERSION 1.0.0)\n",
        )?;
        let powershell = PathBuf::from("pwsh.exe");
        let cmake = PathBuf::from("cmake.exe");
        let toolset = fixture_toolset(root)?;
        for architecture in [Architecture::X64, Architecture::X86] {
            let context = NativeContext {
                root,
                powershell: &powershell,
                cmake: &cmake,
                toolset: &toolset,
                jobs: 16,
            };
            let commands = package_commands(architecture, &context, BTreeMap::new())?;
            assert_trusted_package_commands(&commands, &cmake, architecture.as_str())?;
        }
        Ok(())
    }
}
