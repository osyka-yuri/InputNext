# Continuous integration

GitHub Actions runs the same native presets and verification scripts used locally. Workflows define events, permissions, job dependencies and artifacts. The Rust CLI in `tools/ci` verifies tools, plans commands and checks SARIF. PowerShell imports the Windows compiler environment. Build tools are excluded from runtime packages.

## Checks

Pull requests and pushes to `main` run Rust formatting, strict Clippy and tests on Windows and Linux, workflow validation, native x64/x86 tests and production-package checks, x64 clang-tidy and MSVC analysis, ASan on both architectures, and x64 UBSan. Feature branches use pull-request checks to avoid duplicate push runs. Weekly runs also enable controlled stress tests and C++ CodeQL capture. CodeQL workflow analysis runs on ordinary checks. Hardware effects and game acceptance remain manual.

The lint toolset pins ShellCheck for both hosts. Actionlint checks Bash embedded in workflows; the same command checks Bash in composite actions and `.sh` files under `.github` and `tools`. Failed external commands retain bounded stdout and stderr tails without logging their arguments or environment.

The C++ job builds Catch2 before initializing CodeQL. It then traces the project and test build in the same directory, reusing the compiled third-party test library.

CodeQL analysis must succeed before the local SARIF check runs. The check rejects malformed reports, failed invocations and findings with `security-severity >= 7.0`. Findings without that score are reported as unclassified and do not block, including those with SARIF `level: error`. Other findings remain in the SARIF artifacts. Fork pull requests never upload to code scanning. Other events use `always`.

Rust is pinned by `rust-toolchain.toml`; dependencies use `Cargo.lock`. Downloaded tools are pinned in `.github/ci/toolchain-lock.json`. Cached archive bytes are hashed on every use, extracted into fresh directories and probed before use. Tool updates produce a report for review; they do not install candidates, edit pins or open pull requests. CodeQL bundle discovery follows release pagination; a failed page or enumeration limit prevents a partial update report. Dependabot proposes separate action and Cargo dependency updates.

Run the tooling checks locally:

```powershell
cargo fmt --all -- --check
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace
```

Use `cargo run --locked -p inputnext-ci -- --help` for commands. `native --plan` prints the selected commands without building. Native runs require a verified toolset and the architecture-matched Visual Studio environment; `tools/ci/Invoke-Native.ps1` imports the compiler report and invokes that command. Existing `tools/verify.ps1` remains available from a Developer PowerShell.

Workflow syntax checks allow GitHub's documented `job.workflow_sha` and `job.workflow_repository` fields and the hosted `ubuntu-26.04` label, which actionlint 1.7.12 does not yet recognize. Other diagnostics remain errors. Review these exceptions when updating actionlint.
