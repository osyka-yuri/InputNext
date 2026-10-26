# GameInput output

The native output composition connects exact GameInput connections to the [output executor](output-execution.md). Generic devices use low- and high-frequency GameInput rumble. DualSense and DualSense Edge with confirmed USB transport use one [raw-report backend](dualsense-usb.md) for actuator mode, classic rumble, adaptive triggers and LEDs. The [audio service](hd-haptics.md) delivers HD samples separately. InputNext supports only input over Bluetooth for these Sony models.

[XInput rumble requests](xinput.md#rumble-and-controls) participate in normal output arbitration. Generic capability reports retain the two motor facts independently; GameInput may adapt a request to the motors available. Its rumble setter returns no status, so executor acceptance records that the synchronous call returned. Raw output instead records the send result. Neither establishes a measured hardware effect.

## Backend selection

DualSense output requires a known DualSense or Edge model, USB transport from a native bus type or enumerator, and no conflicting identity evidence. A Sony connection without that qualification receives no output backend, including native GameInput rumble: it can activate enhanced Bluetooth reports and interrupt input. Advertised motor bits remain hardware facts and do not override this policy. Other devices retain generic rumble when at least one GameInput motor is available.

Known Sony models require a blocking metadata query outside control and runtime locks. The connection owner refreshes qualification at a nominal 64 ms interval, including negative results. A positive observation permits new native attempts for 256 ms from the query's start. The USB sink checks that permission before entering native code; an expired permission cannot be revived. A negative result or query failure revokes it immediately. Failures leave selection pending for retry. Generic devices need no metadata query for selection.

Qualification belongs to an exact target and catalogue generation. Changed facts invalidate it. Recovery after revocation or expiry creates a new permission; a replacement backend waits for retirement, native completion, sink destruction and acknowledgement of its predecessor. The two routes never write concurrently. Closing admission preserves qualification refresh for existing backends while prohibiting new registrations; stopping revokes their permissions.

## Ownership

Output-enabled runtimes activate native cleanup before starting. The connection owner creates sinks and manages backend registration, retirement and acknowledgement on its control worker. Native releases run outside input maintenance. The ordinary input-only runtime remains caller-driven.

`backend_progress(target)` returns cached monitoring data. `query_backend_progress(target)` reads the executor's current record for the exact managed backend; use it when checking a completed native attempt. Neither operation requalifies delivery, and a returned completion remains a historical observation.

Creating the executor starts its coordinator; registering backends starts their delivery workers. The coordinator observes logical changes at a nominal four-millisecond period. `request_drive` is an optional wake-up hint; the XInput service does not own or call the executor.

A reconnect receives a fresh registry identity. If GameInput retains the same native device object, its new output sink waits until the previous sink and output lease have finished cleanup. Input admission remains independent; the connection owner retries deferred output creation automatically.

For best-effort neutralization, submit zero while the assignment and backend remain live and observe a matching completed attempt where possible. During shutdown, stop new calls, close audio and connection admission and presentation, and finish outstanding callers. Drain the audio service before shutting down the connection owner. The connection owner joins its control worker and shuts down the borrowed executor; runtime drain then accounts for device and API cleanup. Keep the registry, authority and focus observer alive throughout cleanup.

Native calls and final releases can block indefinitely. A timeout does not establish drain. GameInput applies feedback while its process is in the foreground and can replay its last requested feedback when focus returns. Already admitted calls can overlap logical revocation; immediate physical neutralization is not guaranteed.

## Rumble diagnostic

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --rumble
```

Build with `INPUTNEXT_BUILD_DIAGNOSTICS=ON`, as in the `windows-x64` development preset. Bring **InputNext GameInput Rumble Diagnostic** to the foreground and click **Start**. The sequence exercises each available motor, zero, disable, a disabled cached request and re-enable, ending with zero. A device with one supported motor uses that motor throughout. A device without rumble, or a DualSense/Edge without qualified USB transport, cannot start.

After delivery is confirmed, check the displayed effect and choose **Works - Next** or **Does not work - Next**. Each choice records an observation and advances one step; a failed observation does not stop the remaining checks. No step advances automatically. There is no overall time limit unless `--duration-ms` is supplied.

**Abort** requests best-effort cleanup. Focus loss or disconnect also ends the run and closes the window. Press **Abort** before switching to another application. Synchronous native calls and shutdown can exceed the polling deadlines. An explicit duration enables a parent watchdog; forced termination cannot confirm neutralization or native drain.

Reports distinguish logical requests, matching native completions, user observations, foreground evidence and connection/runtime drainage. Native matches identify newer attempts or reuse of earlier evidence. Exit code zero requires every user check to pass and shutdown to be confirmed. A returned setter alone does not pass the physical check. For adaptive triggers and LEDs, use the [DualSense USB diagnostic](dualsense-usb.md#diagnostic).
