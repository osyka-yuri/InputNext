# GameInput

The x86 and x64 GameInput runtimes discover physical controllers and provide latest state through the [device registry](input.md) and independent readers through [input history](history.md). They use Microsoft GameInput 4.0.300, API version 4, loaded from an absolute app-local path. The build copies the matching `GameInputRedist.dll` and `GameInputRawInputProxy.exe` beside the diagnostic executable.

## Ownership and maintenance

Create `gameinput_runtime` with a registry and `gameinput_options::runtime_library`. One caller serializes `start`, `maintain`, and `stop`. There is no internal worker. While running, provide maintenance opportunities at least every 4 ms and use `next_deadline()` to service recovery. Native calls can delay that caller; this interval is a scheduling requirement, not a native completion guarantee.

`connections()` returns exact connection targets, borrowed read bindings, capture health, and whether pad and Guide state have been observed. Ready reads use the registry's published state without calling GameInput. A disconnect ends that connection's identity; reconnecting creates a new target even when hardware metadata is identical.

Pad and Guide readings merge independently. A pad reading without system-button state preserves the last known Guide value. Guide can be observed before the first pad sample; that does not create a fabricated pad sample. Callback updates take precedence over an older in-progress seed, including readings with equal source timestamps. Invalid readings preserve the last valid publication.

Reading callback registration failure enables 4 ms polling with a registration retry after one second. Healthy callback capture polls only while the first pad sample is missing. Unknown initial Guide state remains explicit. Native ownership is limited to 128 pending, live, and retiring device records, with at most 16 admitted connections. GameInput can impose a lower callback limit; registration failure follows the same recovery path.

## Shutdown

Finish external calls, history calls, and metadata queries before destroying the runtime. The registry must outlive the runtime, its borrowed bindings, and its pins.

Retirement closes publication and removes the exact registry connection before unregistering callbacks. Callback contexts, device references, residency pins, and the loaded module remain alive until native unregister succeeds.

`stop()` may block inside synchronous GameInput calls. Its report separates pending callback owners, metadata queries, active history calls, and retained history owners. The destructor keeps draining until completion. Neither has a completion deadline; the one-second unregister retry interval does not bound native completion.

## Metadata

`query_metadata(target)` is an explicit blocking caller operation. It returns owned identity, transport, physical capabilities, battery availability, and audio endpoint identity. It performs Windows queries outside publication and lifecycle locks. At most four metadata queries are admitted concurrently. Retirement, shutdown, or a concurrent facts refresh invalidates an in-progress result instead of returning it for the wrong connection.

Container correlation uses the native container ID and an exact PnP interface lookup. Conflicting evidence rejects audio association. VID/PID similarity is insufficient. Active audio endpoints must match the exact container; multiple distinct candidates are ambiguous. DualSense and Edge headset endpoints are excluded, and the haptic endpoint remains distinct from a generic controller's headset endpoint. A missing optional DirectSound GUID preserves a valid endpoint ID. Audio queries return identifiers without starting playback.

Transport includes its evidence source. USB connectivity does not imply a charging state. Battery telemetry is unsupported through this provider. Field availability distinguishes absent, unsupported, unavailable, ambiguous, and capacity-limited observations. Audio endpoint IDs accept at most 32,767 UTF-16 code units, and haptic endpoint IDs at most 255; neither is truncated.

Native metadata notifications request a complete owned facts refresh on the coordinator. A transient failure preserves previous values and retries after one second. `query_metadata` reports `metadata_error::stale` while a refresh is pending. Counter exhaustion fails metadata queries explicitly without stopping input publication. Previously returned snapshots remain valid owned copies.

## Diagnostic

Build with the x64 preset and run:

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --duration-ms 10000 --require-device --require-reading
```

For the 32-bit diagnostic, use the `windows-x86` preset and its build directory in these commands.

The diagnostic accepts durations from 1 through 300,000 ms. `--require-device` requires observing an admitted connection during the run; `--require-reading` requires a pad sample. It reports connections, samples, Guide availability, metadata, and shutdown.

Add `--history` to open independent [history readers](history.md). `--require-history` also requires a native baseline followed by a native Pad record or a qualified native checkpoint:

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --duration-ms 10000 --require-device --require-reading --require-history
```

History output distinguishes record origins, baselines, checkpoints, resets, loss, and failures. `native_readings` counts native Pad readings and excludes baselines and captured Guide observations. A qualified checkpoint can satisfy `--require-history` while the controller is stationary. Readers close before shutdown, and the final report includes retained history owners. Empty inventory cannot satisfy `--require-history`.

Metadata queries run synchronously, at most once per live connection per second and one query per coordinator pass; they can delay that pass. Reconnected identities are queried immediately.

Available container GUIDs and endpoint IDs identify the reported associations. Endpoint text is quoted and escaped so control characters cannot create misleading output lines.

The parent supervises a fresh child in a kill-on-close job. A deadline failure terminates the child and reports failure with callback drainage unverified. Without requirement flags, an empty inventory passes the initialization-and-cleanup smoke check.

For physical validation, run the diagnostic long enough to exercise each action:

1. Move sticks and triggers and press ordinary buttons; verify changing samples and stable connection identity.
2. Press and release Guide; verify both transitions and observed Guide availability.
3. Disconnect and reconnect; verify retirement and a new exact connection identity.
4. Repeat over Bluetooth and, when available, with simultaneous controllers. Check transport and endpoint identity against the corresponding controller.

Hardware checks are separate from ordinary deterministic tests. See [development](development.md) for build and test selection.

The native callback-drain check requires a connected controller and runs explicitly:

```powershell
./build/windows-x64/tests/cpp/integration/gameinput/native/inputnext_gameinput_native_tests.exe '[hardware]'
```

The check holds a production-adapter reading callback while unregister begins, releases it, and requires callback exit and successful unregister. A parent supervises the helper with a finite deadline.
