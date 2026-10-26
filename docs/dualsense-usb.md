# DualSense USB feedback

DualSense and DualSense Edge use GameInput 4 raw output reports when [backend selection](gameinput-output.md#backend-selection) confirms USB. One backend represents both adaptive triggers, rumble and the LED group. The [WASAPI adapter](hd-haptics.md) supplies HD body samples; the USB report selects the actuator route. Bluetooth remains input-only.

## Report encoding

The portable `inputnext_dualsense_usb_codec` encodes a 47-byte feedback payload inside a fresh, zero-initialized 64-byte USB report. Offsets include report ID `0x02` at byte 0.

| Bytes | Contents |
| --- | --- |
| 1–2 | Feedback flags `0x0F` (classic) or `0x0C` (HD), LED control flags `0x15` |
| 3–4 | High-frequency/right and low-frequency/left rumble |
| 9 | Microphone LED |
| 11–21 | Right trigger mode and ten parameters |
| 22–32 | Left trigger mode and ten parameters |
| 44 | Player LED pattern |
| 45–47 | Lightbar red, green and blue |

Other bytes remain zero. HD mode zeros the classic motor bytes. LED writes enable microphone, lightbar and player-indicator control without requesting LED release/reset. Rumble rounds normalized amplitude to the nearest byte. Player indices 0–4 map to `00`, `04`, `0A`, `15`, `1B`.

Trigger modes are off (`05`), feedback (`21`), weapon (`25`) and vibration (`26`). Positions round to zones 0–9; strength rounds to levels 0–8. Feedback and vibration support uniform and per-zone layouts. Active zones use a ten-bit mask and three bits per nonzero strength level. Vibration adds a rounded frequency from 1–255. Weapon uses a uniform start zone 2–7 and a later end zone through 8. Invalid parameters used by the selected mode, unsupported geometry or zero effective strength encode as off with zero parameters. Public requests remain subject to [logical value validation](output.md#requests-and-arbitration).

## Native delivery

The sink retains one exact runtime output lease and lazily creates an owned raw report. Raw buffer sizes of 64, 63 and 48 bytes select the complete report, the report without its ID, or the first 48 bytes including the ID. Other sizes are unsupported. The Windows adapter uses `GetRawDataSize()` for the required length. Filling must match that length exactly and preserve it; the descriptor size does not resize or select the payload.

Every attempt fills the complete selected payload. Failed creation can retry without a cached report; failed filling prevents send. Filling and send failures retain the report for a later refill. HRESULTs distinguish accepted, disconnected, unsupported, failed and retryable outcomes; a boolean fill failure has no HRESULT. The executor owns retries, attempt identity and applied-state tracking.

The report and its retained device are released before the runtime lease. Native calls and releases run outside input/history and connection-control locks. Retirement and destructors release resources without sending output. Neutralization requires an ordinary full off/zero submission and observation of its accepted completion while the backend remains live; it cannot prove physical neutrality after disconnect or native failure.

## Diagnostic

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --dualsense-usb
```

Build with `INPUTNEXT_BUILD_DIAGNOSTICS=ON`, as in the `windows-x64` development preset. Bring **InputNext DualSense USB Diagnostic** to the foreground and click **Start**. Output begins only after that click with a USB-qualified controller. The run keeps the selected target and catalogue generation. Its 20 steps exercise LEDs, individual and combined motors, both triggers in each supported mode and layout, and final off/zero.

Follow the instruction for each step. Once native delivery is confirmed, choose **Works - Next** or **Does not work - Next** to record your observation and advance. A failed observation does not stop the remaining checks. No step advances automatically. The current effect is renewed with a five-second lifetime while the window remains foreground. There is no overall time limit unless `--duration-ms` is supplied.

`--start-step N` starts at a particular step (1–20); skipped checks are excluded from the result counts. For example, `--dualsense-usb --start-step 16` checks the remaining right-trigger modes and final neutral output. Each step replaces all diagnostic output channels. Step 1 sets the lightbar to orange; step 2 turns RGB off and lights three white player indicators below the touchpad. Steps 3 and 4 turn the amber microphone mute-button LED on and off. A new run submits its first selected step again.

**Abort** requests best-effort neutralization. Focus loss, changed metadata, disconnect or failure also end the run and close the window. Press **Abort** before switching to another application. Evidence polling uses deadlines, but synchronous native calls and shutdown can exceed them. With an explicit duration, the parent can terminate an overrun child; termination does not prove neutralization or native drain.

Reports distinguish user observations, matching native completions, reuse of earlier evidence, and cleanup/drain status. They include exact backend, publication, attempt and logical sequence identities. Completion checks use fresh executor snapshots after submission. Exit code zero requires all selected user checks to pass and cleanup to be confirmed. Native acceptance is not a physical measurement; user observations are reported separately.
