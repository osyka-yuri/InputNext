# Development

## Requirements

- Windows with Visual Studio C++ Build Tools and a Windows SDK.
- CMake 4.4.4 or newer and Ninja on `PATH`.
- PowerShell 7.6 or newer for the scripts.
- clang-format and clang-tidy 23.1.3 on `PATH` for quality checks.
- clang-cl 23.1.3 on `PATH` for the Undefined native profile.
- PSScriptAnalyzer 1.25.0 or newer for PowerShell analysis and formatting.

Use a Visual Studio Developer PowerShell configured for x64 or x86. Configure defaults to `cl` when no compiler is supplied, using the compiler and SDK from that environment. It preserves the resolved compiler on subsequent configurations and rejects a target that does not match the preset. C++23, standard C++ exception handling (`/EHsc`), the static MSVC runtime, strict conformance, and warnings as errors apply to project targets. Dependency sources do not inherit project warning options.

Project C++ targets define `NOMINMAX`; dependency targets retain their own compile definitions.

MSVC also enables additional conversion, enum-switch and virtual-destructor warnings, `/sdl` security checks, and `/options:strict`. Compiler-specific options are excluded from clang-cl builds.

Catch2 3.16.1 is downloaded and SHA-256 verified during the first test-enabled configure. Subsequent builds reuse the dependency in their build directory. GameInput and controller hardware are not required for the current tests. Dependencies are pinned; builds do not silently follow upstream releases.

The scripts declare PowerShell 7.6 through `#Requires` and run in the process used to invoke them. Use `pwsh -NoProfile -File ./tools/verify.ps1` from an appropriate Developer shell when selecting an installed PowerShell executable explicitly.

## Build and test

From an x64 Developer PowerShell:

```powershell
./tools/verify.ps1 -Architecture x64
```

For x86, open an x86 Developer PowerShell and run:

```powershell
./tools/verify.ps1 -Architecture x86
```

The script configures, builds, and runs CTest. Any failed step returns an error. Logs and a CTest JUnit report are saved under `build/windows-<architecture>/verification/<run>/`. CTest output for each discovered Catch2 case is stored in its test target's build directory under `results/`.

Run the quality gates as well:

```powershell
./tools/verify.ps1 -Architecture x64 -CheckQuality
```

This first checks PowerShell analysis and formatting and C++ formatting, then enables clang-tidy for the Default native profile while compiling project targets, builds, and runs CTest. clang-tidy uses the same target architecture and exception model as the build. Analysis warnings fail the build. Third-party targets are excluded from the project analysis policy. Unchanged objects reuse their previous analysis; changes to `.clang-tidy` invalidate project objects.

Without `-CheckQuality`, the script explicitly disables clang-tidy for the build. Quality checks never rewrite sources or install tools.

Headers must compile on their own. Include directly used declarations rather than relying on unrelated headers, and keep a source file's corresponding header first. Preserve required SDK include order. Use narrow exceptions for external umbrella headers and IWYU annotations for intentional project re-exports.

### Native diagnostics

Select a native profile:

| Profile | Architecture | Compiler | Checks |
| --- | --- | --- | --- |
| `Default` | x64, x86 | MSVC-compatible | Ordinary build; clang-tidy with `-CheckQuality` |
| `Analyze` | x64 | MSVC | `/analyze` for production and test targets |
| `Address` | x64, x86 | MSVC | AddressSanitizer |
| `Undefined` | x64 | clang-cl | UndefinedBehaviorSanitizer, with recovery disabled |

```powershell
./tools/verify.ps1 -Architecture x64 -NativeProfile Analyze
./tools/verify.ps1 -Architecture x64 -NativeProfile Address
./tools/verify.ps1 -Architecture x86 -NativeProfile Address
./tools/verify.ps1 -Architecture x64 -NativeProfile Undefined
```

Profiles use separate build directories with `-analyze`, `-asan`, or `-ubsan` suffixes and retain verification logs there. `Analyze` also builds a separate production-only tree. Diagnostic profiles keep compiler warnings as errors; `-CheckQuality` checks formatting and PowerShell without adding clang-tidy to their compilation.

The Address profile requires the MSVC AddressSanitizer component. It disables incompatible runtime checks and incremental linking. Undefined uses the static release CRT and checks C++ sanitizer runtime linkage during configuration. These profiles instrument project targets and Catch2; prebuilt SDK binaries are unchanged.

Diagnostic test configurations also have matching build and test presets, for example:

```powershell
cmake --preset windows-x64-asan
cmake --build --preset windows-x64-asan
ctest --preset windows-x64-asan
```

Stress tests are optional and are built only when explicitly enabled:

```powershell
./tools/verify.ps1 -Architecture x64 -CheckQuality -IncludeStress
```

Use `-IncludeStress -TestRegex '^devices\.stress\.'` to run only the device stress cases. The script explicitly disables their build option on ordinary runs, so a previous stress run does not change the next run's selection.

The equivalent native commands are:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

CTest requires the test executables to be built first. List cases or select one by its full name:

```powershell
ctest --preset windows-x64 --show-only
./tools/verify.ps1 -Architecture x64 -TestRegex '^harness\.cpp23_is_enabled$'
```

Catch2 tags also provide CTest labels. Run one test level after building:

```powershell
ctest --preset windows-x64 -L '^unit$'
ctest --preset windows-x64 -L '^component$'
```

An empty selection is an error. The harness checks C++23 support and verifies that an intentional assertion failure returns a nonzero status and releases its fixture resource. The intentional failure runs only inside its parent check; it is excluded from ordinary Catch2 discovery.

Configure without tests to omit Catch2 entirely:

```powershell
cmake --preset windows-x64 -B build/windows-x64-no-tests -DBUILD_TESTING=OFF
cmake --build build/windows-x64-no-tests
```

This builds the production input and device libraries without downloading Catch2. The libraries require no GameInput package or controller hardware.

## Formatting

The C++ style starts from LLVM with four-space indentation, a 100-column limit, left-aligned pointers and references, and LF line endings. Wrapped parameter and argument lists use one item per line. Includes are sorted within existing blocks; comment text is preserved.

Use the repository's `.clang-format` with clang-format 23.1.3. Check the current C++ source without changing it:

```powershell
clang-format --dry-run --Werror tests/harness/runner_checks.cpp
```

Check PowerShell analysis, PowerShell formatting, and C++ formatting independently:

```powershell
./tools/check_quality.ps1
```

PowerShell uses the built-in `CodeFormattingOTBS` profile: four-space indentation and opening braces on the same line. Static analysis uses PSScriptAnalyzer's default rules. To apply formatting explicitly, run the module's formatter and review the result:

```powershell
Import-Module PSScriptAnalyzer -MinimumVersion 1.25.0
$path = './tools/verify.ps1'
$source = Get-Content -LiteralPath $path -Raw
$formatted = Invoke-Formatter -ScriptDefinition $source -Settings CodeFormattingOTBS
Set-Content -LiteralPath $path -Value $formatted -NoNewline -Encoding utf8NoBOM
```

The formatting check compares normalized line endings. Both formatting and static analysis must pass.

## Test organization

Catch2 is the C++ test framework. CTest discovers cases individually, and Catch2 tags become CTest labels. Use a descriptive case name and classify tests by their actual dependency boundary:

- `tests/cpp/unit/input` checks gamepad value validation.
- `tests/cpp/component/input` checks coherent publication through the production input library.
- `tests/cpp/component/devices` checks connection ownership, retirement, and concurrency through the production device library. Stress cases have a separate executable and build option.
- `tests/harness` checks the test runner and its failure cleanup.

Concurrency is a test property, not a separate integration level. Workers collect outcomes; assertions run after cleanup and join. Tests use explicit synchronization to reach the required interleaving, with bounded timeouts for failure isolation.
