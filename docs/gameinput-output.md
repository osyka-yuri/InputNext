# GameInput rumble

The native output composition connects exact GameInput connections to the [output executor](output-execution.md). It supports low- and high-frequency rumble. Capability reports retain the two motor facts independently; GameInput may adapt a request to the motors available.

[XInput rumble requests](xinput.md#rumble-and-controls) participate in normal output arbitration. The GameInput setter returns no status; executor acceptance records that the synchronous call returned. Neither logical acceptance nor setter return establishes physical vibration.

## Ownership

Output-enabled runtimes activate native cleanup before starting. The connection owner creates endpoints and manages backend registration, retirement and acknowledgement on its control worker. Native releases run outside input maintenance. The ordinary input-only runtime remains caller-driven.

Creating the executor starts its coordinator; registering backends starts their delivery workers. The coordinator observes logical changes at a nominal four-millisecond period. `request_drive` is an optional wake-up hint; the XInput service does not own or call the executor.

A reconnect receives a fresh registry identity. If GameInput retains the same native device object, its new output endpoint waits until the previous endpoint and output lease have finished cleanup. Input admission remains independent; the connection owner retries deferred output creation automatically.

For best-effort neutralization, submit zero while the assignment and backend remain live and observe a matching completed attempt where possible. During shutdown, stop new calls, close connection admission and presentation, and finish outstanding callers. Shut down the connection owner before draining the runtime. It joins its control worker and shuts down the borrowed executor; runtime drain then accounts for device and API cleanup. Keep the registry, authority and focus observer alive throughout cleanup.

Native calls and final releases can block indefinitely. A timeout does not establish drain. GameInput applies feedback while its process is in the foreground and can replay its last requested feedback when focus returns. Already admitted calls can overlap logical revocation; immediate physical neutralization is not guaranteed.

## USB diagnostic

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --rumble --duration-ms 60000
```

Bring **InputNext GameInput Rumble Diagnostic** to the foreground and click **Start**. The sequence exercises each available motor, zero, disable, a disabled cached request and re-enable, ending with zero and cleanup reports. A device with one supported motor uses that motor throughout; a device without rumble cannot start. **Abort** ends the sequence with best-effort cleanup.

Reports distinguish logical requests, matching native-command completions, foreground evidence and connection/runtime drainage. Native matches use the actual completed payload and identify newer attempts or reuse of earlier evidence. Confirm perceived vibration separately. A focus loss, disconnect or timeout can leave the sequence incomplete; successful initialization or a returned setter alone does not pass the physical check.
