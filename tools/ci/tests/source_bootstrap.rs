use std::collections::BTreeMap;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result};
use inputnext_ci::process::{CommandSpec, Runner, SystemRunner, run_checked};

fn executable(name: &str) -> Result<PathBuf> {
    let paths = std::env::var_os("PATH").context("PATH is absent")?;
    std::env::split_paths(&paths)
        .map(|directory| directory.join(name))
        .find(|path| path.is_file())
        .with_context(|| format!("{name} was not found"))
}

fn git_for_windows_bash(git: &Path) -> Result<PathBuf> {
    #[cfg(windows)]
    let bash_name = "bash.exe";
    #[cfg(not(windows))]
    let bash_name = "bash";

    let executable_directory = git.parent().context("Git executable directory")?;
    let parent_name = executable_directory
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default();
    let git_root = if parent_name.eq_ignore_ascii_case("cmd") {
        executable_directory
            .parent()
            .context("Git installation root")?
    } else if parent_name.eq_ignore_ascii_case("bin") {
        let architecture_directory = executable_directory
            .parent()
            .context("Git executable parent")?;
        let architecture_name = architecture_directory
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        if ["ucrt64", "mingw64", "usr"]
            .iter()
            .any(|name| architecture_name.eq_ignore_ascii_case(name))
        {
            architecture_directory
                .parent()
                .context("Git installation root")?
        } else {
            architecture_directory
        }
    } else {
        anyhow::bail!("unsupported Git executable layout: {}", git.display());
    };

    [
        git_root.join("bin").join(bash_name),
        git_root.join("usr").join("bin").join(bash_name),
    ]
    .into_iter()
    .find(|candidate| candidate.is_file())
    .context("Git for Windows Bash is required")
}

fn invoke(program: &Path, root: &Path, args: &[&str]) -> Result<String> {
    let output = run_checked(
        &mut SystemRunner,
        &CommandSpec {
            program: program.to_path_buf(),
            args: args.iter().map(OsString::from).collect(),
            cwd: root.to_path_buf(),
            env: BTreeMap::new(),
            timeout: Duration::from_secs(15),
            output_limit: 1024 * 1024,
        },
    )?;
    Ok(String::from_utf8(output.stdout)?.trim().to_owned())
}

#[test]
fn source_guard_accepts_qualified_tag_and_rejects_identity_drift() -> Result<()> {
    let git = executable(if cfg!(windows) { "git.exe" } else { "git" })?;
    #[cfg(windows)]
    let bash = git_for_windows_bash(&git)?;
    #[cfg(not(windows))]
    let bash = executable("bash")?;
    let fixture = tempfile::tempdir()?;
    let root = fixture.path().to_path_buf();
    invoke(&git, &root, &["init", "--initial-branch=main"])?;
    invoke(
        &git,
        &root,
        &[
            "-c",
            "user.name=Fixture",
            "-c",
            "user.email=fixture@example.invalid",
            "commit",
            "--no-gpg-sign",
            "--allow-empty",
            "-m",
            "fixture",
        ],
    )?;
    let source = invoke(&git, &root, &["rev-parse", "HEAD"])?;
    invoke(&git, &root, &["tag", "v0.1.0"])?;
    invoke(
        &git,
        &root,
        &["update-ref", "refs/remotes/origin/main", &source],
    )?;
    let script =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../.github/actions/source/qualify.sh");
    let script = script.to_string_lossy().replace('\\', "/");
    let mut environment = BTreeMap::from([
        ("EVENT", "push".to_owned()),
        ("REF_TYPE", "tag".to_owned()),
        ("SOURCE_SHA", source.clone()),
        ("SOURCE_REF", "refs/tags/v0.1.0".to_owned()),
        ("SOURCE_TAG", "v0.1.0".to_owned()),
        ("SOURCE_REPOSITORY", "owner/project".to_owned()),
        ("WORKFLOW_REPOSITORY", "owner/project".to_owned()),
        ("WORKFLOW_SHA", source.clone()),
        ("CALLER_SHA", source),
    ]);
    let check = |values: &BTreeMap<&str, String>| -> Result<Option<i32>> {
        Ok(SystemRunner
            .run(&CommandSpec {
                program: bash.clone(),
                args: vec![OsString::from(&script)],
                cwd: root.clone(),
                env: values
                    .iter()
                    .map(|(key, value)| (OsString::from(key), OsString::from(value)))
                    .collect(),
                timeout: Duration::from_secs(15),
                output_limit: 1024 * 1024,
            })?
            .code)
    };
    assert_eq!(check(&environment)?, Some(0));
    for (key, bad) in [
        ("EVENT", "workflow_dispatch"),
        ("SOURCE_TAG", "v00.1.0"),
        ("SOURCE_REF", "refs/heads/main"),
        ("WORKFLOW_REPOSITORY", "other/project"),
        ("WORKFLOW_SHA", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        ("CALLER_SHA", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
    ] {
        let original = environment
            .insert(key, bad.to_owned())
            .context("fixture key")?;
        assert_ne!(check(&environment)?, Some(0), "accepted changed {key}");
        environment.insert(key, original);
    }
    fs::write(root.join("untracked.txt"), "no source execution")?;
    invoke(&git, &root, &["tag", "-d", "v0.1.0"])?;
    assert_ne!(check(&environment)?, Some(0));
    Ok(())
}

#[test]
fn git_bash_is_found_from_supported_git_path_layouts() -> Result<()> {
    let temporary = tempfile::tempdir()?;
    let git_root = temporary.path().join("Git");
    #[cfg(windows)]
    let bash = git_root.join("bin/bash.exe");
    #[cfg(not(windows))]
    let bash = git_root.join("bin/bash");
    fs::create_dir_all(bash.parent().context("Bash parent directory")?)?;
    fs::write(&bash, "fixture")?;

    for relative_git in [
        "cmd/git.exe",
        "ucrt64/bin/git.exe",
        "mingw64/bin/git.exe",
        "usr/bin/git.exe",
    ] {
        let git = git_root.join(relative_git);
        fs::create_dir_all(git.parent().context("Git executable parent")?)?;
        fs::write(&git, "fixture")?;
        assert_eq!(git_for_windows_bash(&git)?, bash);
    }

    let outer_bash =
        temporary
            .path()
            .join("bin")
            .join(if cfg!(windows) { "bash.exe" } else { "bash" });
    fs::create_dir_all(outer_bash.parent().context("Outer Bash parent")?)?;
    fs::write(outer_bash, "unrelated fixture")?;
    fs::remove_file(&bash)?;
    assert!(git_for_windows_bash(&git_root.join("cmd/git.exe")).is_err());
    Ok(())
}
