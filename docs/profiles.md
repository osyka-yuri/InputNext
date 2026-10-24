# Per-game profiles

Place `inputnext.toml` beside the game executable to configure persistent adaptive-trigger effects. The native runtime reads it once during initialization, independently of the working directory and XInput DLL location. Restart the game after editing.

The profile applies to all [assigned XInput controllers](xinput.md#positions-and-state). Higher-priority effects override it until cleared or expired. Effects are reapplied after foreground restoration or controller reassignment. Physical effects require a [supported output backend](gameinput-output.md#backend-selection); DualSense Bluetooth remains input-only.

## Configuration

Start with the [example profile](../examples/inputnext.toml):

```toml
[effects.left_trigger]
mode = "feedback"
start = 0.20
strength = 0.60

[effects.right_trigger]
mode = "weapon"
start = 0.25
end = 0.75
strength = 0.80
```

Only `effects.left_trigger` and `effects.right_trigger` configure trigger policies. Omitted or empty trigger sections contribute no effect. A nonempty section requires `mode`.

| Mode | Required fields besides `mode` | Optional fields | Effect |
| --- | --- | --- | --- |
| `off` | None | None | Neutral trigger |
| `feedback` | None | `start`, `strength` | Resistance from a selected position |
| `weapon` | `start`, `end` | `strength` | Resistance between two supported detents |
| `vibration` | `frequency` | `start`, `strength` | Vibration from a selected position |
| `curve` | `strengths` | None | Resistance levels for ten zones |
| `slope` | `start`, `end`, `start_strength`, `end_strength` | None | Interpolated resistance levels |
| `vibration_curve` | `amplitudes`, `frequency` | None | Vibration amplitudes for ten zones |

Positions and scalar strengths must be finite numbers in `[0, 1]`. Optional `start` defaults to `0`; optional `strength` defaults to `0.5`. `frequency` must be a whole number from 1 through 255, expressed as an integer or float such as `120.0`. It is a controller parameter, not a calibrated frequency in Hz. `strengths` and `amplitudes` require exactly ten integers from 0 through 8.

Uniform effects round positions to the nearest of ten hardware positions (`round(position * 9)`) and strengths to levels 0–8. Weapon start must quantize to zone 2–7; end must quantize to a later zone through 8. Its strength must quantize to a nonzero level. Both weapon and slope require `start < end`.

For slope, boundaries use ten travel intervals: `min(floor(position * 10), 9)`. Endpoint strengths are quantized before linear interpolation. Zones before start are zero; zones after end retain the end strength. If both boundaries fall in one zone, that zone receives the start strength and later zones receive the end strength.

For example, this slope produces `[0, 0, 1, 2, 3, 5, 6, 7, 8, 8]`:

```toml
[effects.left_trigger]
mode = "slope"
start = 0.2
end = 0.8
start_strength = 0.1
end_strength = 1.0
```

An all-zero curve or slope becomes `off`. Feedback and vibration with a strength that rounds to zero also become `off`.

## Validation and errors

Profiles use TOML 1.0 encoded as UTF-8. Files are limited to 64 KiB, nested value depth to 32, and the complete document to 128 tables including implicit tables and the root. Duplicate keys or tables and malformed TOML reject the entire document. A missing, unreadable or rejected document uses neutral defaults.

The loader denies shared write access while reading. An open writer makes loading fail; finish saving before starting the game.

Semantic errors disable only the affected trigger and preserve a valid policy on the other trigger. Unknown effect names, unknown or mode-inappropriate parameters, wrong types and invalid ranges are diagnosed. Unrelated top-level configuration sections are ignored by this trigger reader. Runtime diagnostics are sent to the Windows debugger output.

The optional validator uses the same parser and semantic checks as the runtime. Build it explicitly:

```powershell
cmake --preset windows-x64 -DINPUTNEXT_BUILD_PROFILE_VALIDATOR=ON
cmake --build --preset windows-x64 --target inputnext_profile_validator
./build/windows-x64/tools/profile_validator/inputnext_profile_validator.exe ./examples/inputnext.toml
```

Exit code `0` means the profile is valid; `1` reports a file or configuration error; `2` reports invalid command-line usage. Validation does not measure controller effects.

Shutdown closes profile contexts before output backends; [output cleanup](gameinput-output.md#ownership) describes the native completion and neutralization limits.
