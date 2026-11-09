use std::process::ExitCode;

use clap::{Parser, Subcommand};
use inputnext_ci::{codeql, native, process, release, tooling, updates, workflows};

#[derive(Debug, Parser)]
#[command(version, about = "InputNext verification and release tools")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    #[command(subcommand)]
    Toolchain(tooling::ToolchainCommand),
    Native(native::NativeArgs),
    Codeql(codeql::CodeqlArgs),
    Updates(updates::UpdatesArgs),
    Workflows(workflows::WorkflowArgs),
    #[command(subcommand)]
    Release(release::ReleaseCommand),
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let mut runner = process::SystemRunner;
    let result = match cli.command {
        Command::Toolchain(command) => command.run(&mut runner),
        Command::Native(command) => command.run(&mut runner),
        Command::Codeql(command) => command.run(&mut runner),
        Command::Updates(command) => command.run(&mut runner),
        Command::Workflows(command) => command.run(&mut runner),
        Command::Release(command) => command.run(&mut runner),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("inputnext-ci: {error:#}");
            ExitCode::FAILURE
        }
    }
}
