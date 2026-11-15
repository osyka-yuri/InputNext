# Install in a game

Install InputNext separately for each Windows game that uses XInput or the supported [DirectInput 8 input contract](directinput.md).

## Installation

1. Extract `InputNext-<version>-windows-x64.zip` for a 64-bit game or `InputNext-<version>-windows-x86.zip` for a 32-bit game. Choose by the game's architecture, even on 64-bit Windows.
2. Close the game and back up any existing files with the names listed below. Replacing another mod's XInput DLL replaces its controller implementation.
3. For XInput, copy these six files from the archive's `bin/` directly into the folder containing the game's EXE. Keep their original names and use files from the same package:

   ```text
   xinput1_3.dll
   xinput1_4.dll
   xinput9_1_0.dll
   inputnext_runtime.dll
   GameInputRedist.dll
   GameInputRawInputProxy.exe
   ```

   To enable DirectInput 8, also copy `dinput8.dll`. For a DirectInput-only installation,
   use `dinput8.dll` and the three shared files `inputnext_runtime.dll`,
   `GameInputRedist.dll` and `GameInputRawInputProxy.exe`. Back up an existing
   `dinput8.dll`; replacing it replaces that mod's entry point.

4. Connect the controller and launch the game normally. Check buttons, sticks and triggers. For DualSense and DualSense Edge, use USB for feedback; Bluetooth supports input only.

## Optional per-game profile

Place `inputnext.toml` beside the game EXE to customize controls or trigger effects. The included [example profile](../examples/inputnext.toml) enables two trigger effects. Without a file, InputNext uses the default assignments, including touchpad clicks on supported controllers. See [per-game profiles](profiles.md) for the format and controller support.

## Uninstall

Close the game, remove the files installed from this package and restore the backed-up originals.
