# Changelog

## [1.2.0]

### Added

- DirectInput 8 support for x86 and x64 games through `dinput8.dll`, including immediate and buffered gamepad input, event notifications, and foreground and background exclusive acquisition on supported devices.
- Software force feedback in foreground exclusive mode. Supported gamepads render constant, ramp, and periodic effects as rumble.
- Optional digital trigger buttons. Set `digital_triggers = true` in `inputnext.toml` to expose LT and RT as buttons alongside their analog axes.

### Changed

- Updated GameInput to 4.1.314. On supported DualSense USB connections, InputNext now uses native touch coordinates to determine which side of the touchpad was clicked.

## [1.1.0]

### Added

- Remap buttons and analog triggers per game through `inputnext.toml`. Profiles accept Universal, PlayStation, and Xbox control names. Mapped input is available through XInput and the Core SDK.
- On supported DualSense USB connections, mechanical touchpad clicks default to View on the left and Menu on the right. Both sides can be remapped or disabled.

### Fixed

- XInput delivers the required key releases when input history resets, preventing keys already reported to the game from getting stuck.

## [1.0.0]

- GameInput-backed XInput replacements (`xinput1_3.dll`, `xinput1_4.dll`, `xinput9_1_0.dll`) sharing a single runtime.
- Core and Effects C SDKs for controller input, input history, and feedback, with C/C++ examples.
- DualSense and DualSense Edge support for USB rumble, adaptive triggers, LEDs, and HD haptics. Bluetooth supports input only; HD haptics require a compatible audio endpoint.
- Per-game profiles for persistent trigger effects. Game-driven effects require an Effects SDK integration.
- Separate x86 and x64 packages with runtime binaries, SDKs, examples, debug symbols, checksums, and third-party notices.
