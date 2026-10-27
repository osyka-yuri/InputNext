# Core C SDK

The Core SDK exposes controller discovery, coherent latest input and independent history readers through [input_next_core.h](../sdk/input_next_core.h). It shares the native session used by the [XInput DLLs](xinput-dll.md). Use an x86 or x64 runtime matching the caller.

## Loading and startup

Load `inputnext_runtime.dll` by absolute path and resolve `input_next_runtime_query_core_api`, exported at ordinal 3. Pass `INPUT_NEXT_CORE_ABI_VERSION` and validate the returned table's size, version and callbacks. Unsupported versions return null. Querying the table pins the shared runtime owner without starting GameInput. Call SDK functions outside `DllMain` and the loader lock. Keep your module reference until all calls have finished.

Runtime information, enumeration, target resolution and reader opening can initiate foreground-gated startup. They may wait for startup or native queries. A background reader does not bypass the initial foreground requirement.

`read_latest`, `read_events` and `close_reader` use the already published session. They never initiate startup. Ready latest reads copy cached input without metadata queries or history traversal. History operations may wait for native history work; keep them off latency-sensitive threads.

Runtime information reports limits of 16 live devices, 28 public readers and 32 public [Effects contexts](effects-sdk.md). Four additional history readers are reserved for XInput. A retained degraded session still exposes its identity and QPC frequency; live operations are unavailable.

## Targets and discovery

A target contains the runtime, device and connection identities. Reconnection or replacement invalidates the old target. Resolve an XInput position, an exact target or a unique hardware fingerprint; resolution never transfers a stale target to a replacement.

Enumeration supports a zero-capacity size probe. `BUFFER_TOO_SMALL` returns the required count and discovery revision without writing device records. Initialize each supplied record's `struct_size` and reserved fields before a successful call. The revision changes when a successfully observed projected catalogue changes, independently of input sample sequences. Enumeration checks for connection churn; it is not an atomic snapshot of all external metadata.

Platform fingerprints preserve SHA-256 over the 32-byte GameInput device ID followed by its 32-byte root ID. Missing halves are zero. Missing identity or hash failure produces no fingerprint; duplicate identities are marked ambiguous. Display names are empty when no authoritative name is available.

Output channel bits are left trigger `0x01`, right trigger `0x02`, rumble `0x04` and LEDs `0x08`. They describe implemented, transport-qualified families rather than output admission or measured delivery. DualSense Bluetooth output is disabled.

## Latest and history

Latest snapshots use stream zero and the coherent publication revision as their sequence. Reading does not advance the publication. `NO_DATA` for a live target returns its stream-zero domain without a sample. Latest and background readers are independent of XInput Enable.

Stick axes are normalized to `[-1, 1]` and triggers to `[0, 1]`. Button bits have these meanings, in ascending order within each range:

| Bits | Buttons |
| --- | --- |
| 0–3 | D-pad up, down, left, right |
| 4–5 | Menu, View |
| 6–7 | Left stick, right stick |
| 8–9 | Left shoulder, right shoulder |
| 10–13 | South, East, West, North |
| 14 | Guide |

Each reader has its own cursor. Opening is ordered against retirement by its final exact-target check. Retirement before that check rejects the open; retirement afterward may invalidate a successfully opened reader. Foreground readers require fresh foreground evidence when opening and delivering data. Delivery pauses with `NOT_ADMITTED` during focus loss or expiry and resumes under the same stream. Unread events may have occurred before foreground restoration. Focus changes do not manufacture a history reset. Background readers omit this delivery qualification.

Opening may return `BUSY` during competing history work. That attempt publishes no reader handle and may be retried.

`read_events` accepts 1–32 initialized snapshot records. Use 32 for guaranteed batch capacity. If the batch does not fit, `BUFFER_TOO_SMALL` reports the required count in `out_result` and writes no snapshots. The complete batch is retained for retry; another read does not fetch past it. Focus suppression also retains an already fetched batch. A retired connection cannot replay retained input.

For other outcomes, process the counted valid prefix before handling the returned status. A traversal failure may accompany useful records. Preserve the result's domain, baseline/gap flags, loss, reset and checkpoint information; see [history](history.md) for their meaning. `NO_DATA` is an empty successful observation, not proof of a new input sample.

Close every opened reader. Close does not require focus or a live connection and does not join an in-flight read. Keep that read's buffers alive until it returns. Concurrent operations on the same reader can return `BUSY`; independent readers progress separately. After completed close, the handle is stale. History traversal may wait indefinitely for the OS; a timeout does not cancel it or end its buffer lifetime.

## C and C++ consumers

All structures use fixed-width values with packing 8. Initialize exact `struct_size` values and zero request flags and reserved fields. Caller-provided buffers are borrowed only for the call. Do not share writable buffers between concurrent calls. Snapshot flags are outputs and may be reused without clearing a previous baseline flag.

Build the optional [C reader routine](../examples/core/core_reader.c) and [C++ DLL loader](../examples/core/core_example.cpp):

```powershell
cmake --preset windows-x64 -DINPUTNEXT_BUILD_SDK_EXAMPLES=ON
cmake --build --preset windows-x64 --target inputnext_core_example
./build/windows-x64/examples/core/inputnext_core_example.exe C:/path/to/inputnext_runtime.dll
```

The default invocation validates the table without native startup. `--read-once` also enumerates and reads one controller and one history batch. Those operations require the host process to meet the foreground policy; a console hosted by another process may not qualify. The C routine can be embedded in a foreground application. Examples are not included in the production DLL bundle.
