# Per-game profiles

Place `inputnext.toml` beside the game executable to configure remapping, DirectInput
trigger buttons or adaptive-trigger effects. InputNext reads it when the session starts.
Restart the game after editing.

Without a readable file, buttons and triggers keep their usual assignments, supported touchpads use the defaults below, and trigger effects are off. Remapping applies inside the game through InputNext's XInput, DirectInput and Core interfaces.

## Input remapping

This profile swaps Cross and Circle and sends Options when you click the left half of the touchpad.

```toml
[input]
layout = "playstation"
remap = [
    { from = "cross", to = "circle" },
    { from = "circle", to = "cross" },
    { from = "touchpad_left", to = "options" },
]
```

`from` names the physical control you press. `to` names the final control the game receives. Unlisted sources keep their default assignments. Each source may appear once, even when it has several names. Use `to = "disabled"` to disable that source. For example, disabling physical `create` still lets a touchpad rule send logical `create`.

`layout` selects the names accepted in the file. Universal names work in all layouts and are the default. The connected controller determines which sources are available.

| Control | Universal | PlayStation | Xbox |
| --- | --- | --- | --- |
| Bottom face button | `face_bottom` | `cross` × | `a` |
| Right face button | `face_right` | `circle` ○ | `b` |
| Left face button | `face_left` | `square` □ | `x` |
| Top face button | `face_top` | `triangle` △ | `y` |
| Select and menu | `select`, `menu` | `create`, `options` | `view`, `menu` |
| D-pad ↑ ↓ ← → | `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | Same names | Same names |
| Left and right shoulders | `left_shoulder`, `right_shoulder` | `l1`, `r1` | `lb`, `rb` |
| Left and right triggers | `left_trigger`, `right_trigger` | `l2`, `r2` | `lt`, `rt` |
| Left and right stick clicks | `left_stick_click`, `right_stick_click` | `l3`, `r3` | `ls`, `rs` |

The table lists the available destinations. Stick movement, PS/Guide and Mute are outside the remap controls.

### Trigger thresholds

With `layout = "playstation"`, `{ from = "l2", to = "l1", threshold = 0.7 }` makes L2 send L1 from 70% travel. Only trigger-to-button rules accept a `threshold`. It defaults to `0.5` and must be finite, greater than `0`, and at most `1`.

A button mapped to a trigger sends full travel when pressed. A trigger mapped to another trigger keeps its analog value. If several sources send the same trigger, the highest value wins. A logical button stays pressed while any of its sources is active.

### Touchpad clicks

Touchpad names are available in `from` in every layout. These assignments also apply without a profile file.

| Source | Default game input |
| --- | --- |
| `touchpad_left` | Select (Create / View) |
| `touchpad_right` | Menu (Options / Menu) |
| `touchpad_unknown_side` | Select (Create / View) |

Touch mappings respond to a mechanical click. The side comes from the finger position at the start of the click and stays fixed until release. The midpoint counts as right. A click already held at connection, or a click without a usable position, uses `touchpad_unknown_side`. Set all three sources to `"disabled"` to turn off touchpad mapping.

InputNext verifies the standard DualSense USB descriptor on each connection with the bundled GameInput `4.1.314.0`. It reads coordinates from the native touch surface. Both verified button layouts use index 13 for the click. The button may carry the GameInput touchpad label or be unlabeled. Other connections use ordinary remapping. If a supported connection loses reliable click data, InputNext releases the touchpad's contribution and waits for a valid physical release before accepting another click. [Qualification details](input-qualification.md#supervised-usb-qualification)

## DirectInput configuration

```toml
[directinput]
digital_triggers = true
trigger_threshold = 0.5
```

- `digital_triggers` is a boolean that defaults to `false`. Set it to `true` to
  expose LT and RT as buttons.
- `trigger_threshold` sets the travel required to press either button. It accepts
  a fraction of full travel in `(0, 1]` and defaults to `0.5`.

See [DirectInput objects and formats](directinput.md#objects-and-formats) for
button IDs and threshold rounding.

## Trigger effects

The [example profile](../examples/inputnext.toml) enables the two trigger effects below. Effects apply to the physical triggers of all [assigned XInput controllers](xinput.md#positions-and-state) with a [supported output backend](gameinput-output.md#backend-selection). Higher-priority effects take precedence until cleared or expired. InputNext reapplies the profile after foreground restoration or controller reassignment.

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

Profiles use UTF-8 TOML 1.0. Limits are 64 KiB per file, 32 nesting levels and
128 tables, including the root and implicit tables. InputNext uses all profile
defaults when a file exceeds a limit or contains invalid TOML, such as duplicate
keys or tables.

The loader denies shared write access while reading. An open writer makes loading fail; finish saving before starting the game.

An invalid `input` or `directinput` section uses its defaults. An invalid trigger
policy disables that trigger's effect. Other valid settings still apply.

InputNext ignores unknown top-level sections and reports configuration errors through
Windows debugger output.

Build the optional validator from an x64 Visual Studio Developer PowerShell. It uses the runtime's configuration parser.

```powershell
cmake --preset windows-x64 -DINPUTNEXT_BUILD_PROFILE_VALIDATOR=ON
cmake --build --preset windows-x64 --parallel 16 --target inputnext_profile_validator
./build/windows-x64/tools/profile_validator/inputnext_profile_validator.exe ./examples/inputnext.toml
```

The validator returns `0` for a valid profile, `1` for a file or configuration error, and `2` for invalid command-line usage. `touchpad_routes=enabled` means the effective settings include touchpad assignments. Device support is checked at connection time.

Shutdown closes profile contexts before output backends; [output cleanup](gameinput-output.md#ownership) describes the native completion and neutralization limits.
