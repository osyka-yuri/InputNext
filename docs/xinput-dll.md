# XInput DLL bundle

The x86 and x64 bundles provide `xinput1_3.dll`, `xinput1_4.dll` and `xinput9_1_0.dll` through one app-local `inputnext_runtime.dll`. Each uses the [XInput presentation](xinput.md) for GameInput state, events and rumble. Select the architecture of the game executable. Keep all InputNext DLLs in a process from the same bundle.

## Build

From an x64 Developer PowerShell:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release --target inputnext_xinput_bundle
```

The files are written to `build/windows-x64-release/xinput/`. This directory contains the three XInput DLLs, `inputnext_runtime.dll`, `GameInputRedist.dll` and `GameInputRawInputProxy.exe`. Debug and sanitizer builds use separate build directories; use the ordinary Release bundle for a game.

For a 32-bit game, run the equivalent commands from an x86 Developer PowerShell:

```powershell
cmake --preset windows-x86-release
cmake --build --preset windows-x86-release --target inputnext_xinput_bundle
```

The x86 files are written to `build/windows-x86-release/xinput/`. The architecture follows the game process, even on 64-bit Windows.

See [development setup](development.md) for the toolchain and automated verification commands.

## Manual game check

Close the game before changing its DLLs. Preserve any existing XInput or InputNext files so that the original setup can be restored. Copy the XInput flavor imported by the game, `inputnext_runtime.dll`, `GameInputRedist.dll` and `GameInputRawInputProxy.exe` beside the game's executable. For `xinput1_3.dll` or `xinput9_1_0.dll`, also copy `xinput1_4.dll` from the same bundle. Do not rename DLLs to select a different flavor. Selecting a different working directory does not change the runtime location.

Launch the game through its usual launcher. Check sticks, triggers and buttons, then rumble, focus loss and return, and controller reconnect. Confirm physical vibration separately from successful API calls. After closing the game, restore the original files if this was a temporary test. Automated loading and ABI tests do not establish game compatibility, physical output or latency.

## Native input check

Build and refresh the staged diagnostic, then run it:

```powershell
cmake --build --preset windows-x64-release --target inputnext_xinput_manual_native_stage
./build/windows-x64-release/tests/cpp/integration/xinput/dll/manual-native/inputnext_xinput_import_probe.exe --native-manual
```

Activate the probe's window with a controller connected during its 120-second observation window. Native startup can extend that time if GameInput does not return. The check requires matching local bundle files and a successful connected-state read through the normally imported DLL while the window is foreground. Missing prerequisites cause failure. It does not test physical rumble or replace the game check.

Use `windows-x86-release` and its build directory for the same check through the x86 DLLs.

## Lifetime and compatibility

The 1.3 and 9.1.0 facades have a normal import dependency on 1.4. With the complete bundle beside the executable and no XInput 1.4 already loaded, Windows loads the local companion before the importing facade initializes. Loading a facade does not start GameInput.

This load order does not override a previously loaded XInput 1.4 or prevent another component from loading a system copy by absolute path. Omitting the local companion can silently select the system DLL. For game replacement, deploy beside the executable. Hosts loading a bundle from another directory must set a dependency search policy, such as `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`. These rules do not guarantee compatibility with every third-party XInput interceptor.

API discovery creates the shared control owner without starting a native session. `XInputEnable` stores the latest requested state across facades, including before startup. An ordinary call starts the session when foreground is confirmed; a cold background or unavailable observation leaves startup pending. The stored Enable state is applied before the service becomes visible. Native startup failure is terminal for that process; restart after correcting the dependency failure.

An accepted runtime provider remains loaded until process termination. Facades can be unloaded and reloaded after their callers finish. Callers must retain their module reference throughout a call and discard function pointers before releasing the last reference. Call XInput outside `DllMain`. Process termination performs no blocking runtime teardown through DLL detach or static destruction.

Argument checks precede runtime discovery. Invalid indices and required null outputs return `ERROR_BAD_ARGUMENTS`; `XInputGetKeystroke` also accepts `XUSER_INDEX_ANY`. Flavor-specific unsupported queries take precedence, and reserved Guide arguments remain ignored. Valid calls without an available service return their connection or unsupported outcome without modifying outputs.

The bundle preserves the established export names and ordinals. Native Guide wait/cancel and controller power-off retain their unsupported outcomes; the [typed C++ Guide waits](xinput-events.md) have a separate contract. Core and Effects SDK exports are not part of this XInput bundle.
