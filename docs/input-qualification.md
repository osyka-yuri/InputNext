# GameInput DualSense input qualification

`inputnext_input_qualification` captures passive GameInput readings for manual review. It leaves the controller mode and output untouched. The diagnostic does not grant production support.

## Build and capture

Build the probe from an x64 Visual Studio Developer PowerShell. The build deploys the pinned GameInput runtime beside the executable.

```powershell
cmake --preset windows-x64-analyze
cmake --build --preset windows-x64-analyze --parallel 16 --target inputnext_input_qualification

$report = Join-Path (Get-Location) 'dualsense-usb-qualification.json'
./build/windows-x64-analyze/tools/input_qualification/inputnext_input_qualification.exe `
  --seconds 20 --output $report
```

Connect one DualSense before starting the probe. It selects the first device with VID `054C` and PID `0CE6`. Capture duration ranges from 1 to 60 seconds. The output path must be absolute, and its parent directory must exist. Confirm USB using Windows device properties for the recorded PnP interface.

After capture starts, swipe without clicking, click and release each half, and move a finger across the midpoint during a held click. Test Create and the mechanical click separately, then overlap their presses and releases. To measure bootstrap, pass `--bootstrap-click-index N` with the observed click button index and hold a click for at least one second. Use `--dualsense-usb-raw` to include USB report coordinates for comparison.

| Option | What it records |
| --- | --- |
| Default capture | Gamepad state, controller buttons and touch surfaces from each reading. The report marks fields that exceed its limits. |
| `--dualsense-usb-raw` | Coordinates from the same reading's 64-byte input report with ID `0x01`. The decoder checks the 15-button layout and requires agreement between the raw click bit and button 13. |
| `--bootstrap-click-index N` | Starts the native history cursor about every 100 ms while the selected button is held and the resolver has a usable baseline. `N` is a zero-based index below the advertised count, up to 63. The cursor uses native coordinates when the device reports one usable touchpad. The report records attempt timing, transition counts and fallback reasons. |

## Reading the report

The JSON records the SDK and runtime versions, architecture, device descriptor, PnP path, capture limits and overlap checks. The callback and forward native streams each retain up to 8,192 readings. The forward walk follows consecutive readings from one seed. A separate backward walk samples up to 256 transitions after capture. Use forward coverage to assess continuity.

Capture integrity requires all required fields, nonempty callback and forward streams, zero callback drops, and a complete forward walk within the limit. Both streams must have valid identities and nondecreasing timestamps. The probe compares retained readings by canonical `IUnknown` identity and requires at least one matching pair with consistent payloads across all pairs. Identity values are local to the capture. The walk describes the sequence GameInput supplied.

The `qualification` field summarizes the capture. Exit code `0` confirms the report was written.

| Status | Meaning |
| --- | --- |
| `inconclusive_*` | The device, descriptor, capture coverage or matched interaction evidence was insufficient. |
| `touch_metadata_not_exposed_no_unlabeled_change_observed_manual_review_required` | The device returned all button labels but exposed neither a touch surface nor a semantic click label. The capture showed no unlabeled button transition. |
| `unlabeled_button_change_observed_manual_review_required` | An unlabeled button changed. Correlate its index and timing with the physical clicks. |
| `observations_captured_manual_review_required_no_auto_qualification` | A typed touch interaction matched by identity and payload. Check the recorded gestures. |
| `raw_usb_touch_unavailable_unverified_button_layout_or_report_descriptor` | The observed layout or report descriptor did not match the raw decoder. |
| `raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required` | Required capture data or raw reports were missing, incomplete or inconsistent. |
| `raw_usb_touch_observations_captured_manual_review_required_no_auto_qualification` | Both streams supplied consistent raw reports. An idle capture can return this result. Check each requested gesture before using it as qualification evidence. |

## Supervised USB qualification

### GameInput 4.1.314.0

A standard DualSense reported a 1920 × 1080 touchpad with room for two contacts on x64 and x86. The left and right click onsets carried native contact coordinates in the same reading as button 13. Swiping without clicking produced contacts without a button press.

The x86 runtime labeled button 13 as `GameInputLabelTouchpadButton`. The x64 runtime left it unlabeled in both the gesture capture and a separate metadata check. The callback and forward native captures matched for all 7,230 readings on x64 and 7,226 on x86, with no callback drops. Production checks the USB device and touch-surface descriptor before enabling this profile.

### Earlier measurements with GameInput 4.0.306.0

The same standard DualSense was tested on x64 and x86 with app-local GameInput `4.0.306.0`. It exposed 15 controller buttons and zero typed touch surfaces. The unlabeled button at index 13 tracked the mechanical click. The 64-byte report with ID `0x01` supplied two contact slots on a 1920 × 1080 surface through `GetRawReport()`. [DualSense driver layout](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)

| Capture | Architecture | Matching callback/native readings | Successful native starts | Attempts reaching 256 transitions | Total attempt time, p50 / p95 / max |
| --- | --- | ---: | ---: | ---: | ---: |
| Mechanical click | x64 | 3,486 | 11/47 | 36 | 353 / 630 / 746 µs |
| Mechanical click | x86 | 3,524 | 11/49 | 38 | 541 / 1,214 / 1,279 µs |
| Raw coordinates | x64 | 4,727 | 12/35 | 23 | 705 / 1,257 / 1,514 µs |
| Raw coordinates | x86 | 4,727 | 12/35 | 23 | 939 / 1,903 / 2,310 µs |

Every callback reading matched a forward native reading with the same payload. The raw capture matched decoded contacts and resolved sides at left and right click onsets. Swiping across the midpoint without clicking produced no mapped press.
