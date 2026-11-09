# Install in a game

Install InputNext separately for each Windows game that uses XInput.

## Installation

1. Extract `InputNext-<version>-windows-x64.zip` for a 64-bit game or `InputNext-<version>-windows-x86.zip` for a 32-bit game. Choose by the game's architecture, even on 64-bit Windows.
2. Close the game and back up any existing files with the names listed below. Replacing another mod's XInput DLL replaces its controller implementation.
3. Copy all six files from the archive's `bin/` directly into the folder containing the game's EXE. Keep their original names and use files from the same package:

   ```text
   xinput1_3.dll
   xinput1_4.dll
   xinput9_1_0.dll
   inputnext_runtime.dll
   GameInputRedist.dll
   GameInputRawInputProxy.exe
   ```

4. Connect the controller and launch the game normally. Check buttons, sticks and triggers. For DualSense and DualSense Edge, use USB for feedback; Bluetooth supports input only.

## Optional trigger effects

Copy `examples/inputnext.toml` beside the game EXE to enable persistent trigger effects. See [per-game profiles](profiles.md) to customize them. Restart the game after editing.

## Uninstall

Close the game, remove the files installed from this package and restore the backed-up originals.
