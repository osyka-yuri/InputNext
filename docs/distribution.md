# Distribution

Build from a Visual Studio Developer PowerShell matching the game executable:

```powershell
cmake --preset windows-x64-production
cmake --build --preset windows-x64-production --parallel
```

Use `windows-x86-production` from an x86 Developer shell for 32-bit games. Each build produces three files under `build/<preset>/packages/`:

- `InputNext-<version>-windows-<architecture>.zip`: runtime, public SDK, examples, documentation and licenses.
- `InputNext-<version>-windows-<architecture>-symbols.zip`: matching PDBs for the four InputNext DLLs.
- `InputNext-<version>-windows-<architecture>-SHA256SUMS.txt`: SHA-256 of both archives.

The runtime archive keeps the six deployment binaries in `bin/`. Follow the [game installation guide](installation.md) to place them beside the game executable. The sample `examples/inputnext.toml` is optional; copying it beside the executable enables its trigger effects. Tests, diagnostics, private C++ headers and build tools are excluded.

Every archive includes build metadata and a manifest of file hashes. Hashes detect changed or missing contents; they do not authenticate the publisher. Metadata records the compiler, architecture and dependency versions. Git fields describe the source tree at packaging time, not binary provenance. Build before packaging and keep sources unchanged during both operations. Runtime and symbol archives must come from the same build.

## SDK examples

From the extracted archive's root, build the C/C++ examples using an x64 Developer shell:

```powershell
cmake -S examples -B ../inputnext-examples-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../inputnext-examples-x64 --parallel
```

Use an x86 Developer shell and a separate build directory for x86 examples. See the [Core SDK](core-sdk.md) and [Effects SDK](effects-sdk.md) for usage and API contracts.

## Production settings

The production profile uses MSVC `/O2 /Ob3 /GL /Gw /Gy`, LTCG, the static CRT and precise floating-point semantics. Linking enables `/OPT:REF,ICF`, disables incremental linking and writes full PDBs separately. ASLR and DEP remain enabled; x64 additionally enables high-entropy ASLR. No AVX2 requirement or fast-math mode is imposed. Identical-code folding can give different functions the same address.

Production requires single-configuration Release with the Default native profile. Debug, analysis and sanitizer presets retain their own settings. Production artifacts require their own ABI and deployment verification; diagnostic builds do not qualify an optimized package.

## Verify an archive

The verifier checks ZIP contents and hashes without executing archive-supplied code:

```powershell
./tools/verify_package.ps1 -Architecture x64 -BuildDirectory ./build/windows-x64-production `
    -Package ./build/windows-x64-production/packages/InputNext-1.0.0-windows-x64.zip
```

For trusted builds, enable the integration targets and pass `-RunTrustedChecks`. These checks execute packaged CMake and DLLs:

```powershell
cmake --preset windows-x64-production -DBUILD_TESTING=ON -DINPUTNEXT_BUILD_SDK_EXAMPLES=ON
cmake --build --preset windows-x64-production --target inputnext_distribution_checks --parallel
./tools/verify_package.ps1 -Architecture x64 -BuildDirectory ./build/windows-x64-production `
    -Package ./build/windows-x64-production/packages/InputNext-1.0.0-windows-x64.zip -RunTrustedChecks
./tools/verify_package.ps1 -Architecture x64 -BuildDirectory ./build/windows-x64-production `
    -Package ./build/windows-x64-production/packages/InputNext-1.0.0-windows-x64-symbols.zip -Symbols
```

Use matching x86 arguments and paths for that architecture. Trusted checks verify exact public exports and binary architecture, build the packaged C/C++ SDK examples, query both public tables and run a normally imported XInput call from a separate deployment copy. The verified extraction stays unchanged. Runtime checks use an independent working directory and a system-only `PATH`. Logs and extracted files remain under `build/<preset>/package-verification/`.

The checks send no effects and require no controller. They establish deployment and ABI behavior, not connected-device startup, physical effects, game compatibility or performance.

## Profile-guided optimization

PGO is opt-in and applies to the process-retained runtime; the reloadable XInput facades retain LTCG. Keep each architecture and training campaign in its own build directory. Configuring Instrument starts a new campaign and invalidates previous training readiness. Instrument the actual runtime DLL:

```powershell
cmake --preset windows-x64-pgo
cmake --build --preset windows-x64-pgo --parallel
```

The training bundle is in `build/windows-x64-pgo/xinput/`, including the matching MSVC `pgort140.dll` counter runtime. Keep that file beside the instrumented runtime; it is removed when linking an ordinary or optimized runtime and is never packaged. `/GENPROFILE:EXACT` uses thread-safe counters. The runtime's `.pgd` is under that build's `pgo/` directory. Instrumented linking archives previous runtime counters under `pgo/retired-counters/`, so they cannot enter the new merge.

Exercise representative foreground/background, connected/disconnected, single/multiple-reader and output workloads through the required XInput flavors and SDKs. Exit the training process normally to flush `.pgc` files, or use Microsoft's `pgosweep` when the process cannot exit yet. When training outside the build directory, collect the matching counters back beside the build's runtime DLL. Do not use an instrumented build for performance measurements.

After training, merge the counters beside the runtime DLL into a separate `pgo/trained/` database, then reconfigure the same build and link against it:

```powershell
cmake --build ./build/windows-x64-pgo --target inputnext_pgo_merge
cmake --preset windows-x64-pgo -DINPUTNEXT_PGO_MODE=Optimize
cmake --build ./build/windows-x64-pgo --target inputnext_distribution --parallel
```

Optimization requires a completed instrumented link and successful merge for the current campaign. Configuration and linking check the campaign identity and raw/trained profile hashes. The linker receives a working copy because `/USEPROFILE` modifies its database. Instrumented relinking and failed merges invalidate readiness. Linker warnings fail the build; instrumented builds have no distribution target.

Keep source, compiler, architecture and compile settings unchanged between training and optimization. Start a fresh campaign after relevant changes and use only counters from its instrumented runtime. Repeat package verification and compare the final optimized DLLs against LTCG using workloads outside training before selecting PGO for distribution.

See Microsoft's [PGO workflow](https://learn.microsoft.com/en-us/cpp/build/profile-guided-optimizations) for collection and merging tools.
