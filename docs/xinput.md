# XInput presentation

The C++ `xinput::service` presents registry bindings as four XInput positions. `gameinput_xinput` reconciles GameInput connections with those positions. Use these libraries through C++ calls or the supervised diagnostic.

## Positions and state

Assignment uses the lowest free position. Assigning an already bound connection preserves its position and packet state. Release requires the exact assignment value; a stale value cannot release a replacement. Assignment values belong only to the service instance that created them.

Assignment IDs never wrap or repeat within that instance. Exhaustion rejects new assignments and preserves existing positions. A physical reconnect has a new connection target; assigning the same live connection again after release has a new assignment ID.

`get_state` captures a coherent assignment, Enable decision, and focus snapshot in one short guarded turn, then reads cached input outside the presentation gate. A final guarded turn revalidates the assignment, live connection, and eligibility before committing the result and packet state. It performs no GameInput query, history traversal, or focus probe. Invalid arguments, stale assignments, and disconnected sources leave the caller's output unchanged.

Disabled, background, unavailable, or expired focus produces neutral input without advancing the packet. A call that captured an ineligible decision stays neutral even if eligibility later returns. An eligible call rechecks focus freshness and Enable before committing. Focus loss or expiry followed by return invalidates an earlier focus capture.

The first eligible sample has packet 1; earlier neutral reads have packet 0. One canonical translated state, including Guide, owns the packet. Equal canonical fields preserve it; changes increment it modulo 2³². Standard responses omit Guide, while extended responses include it. Switching between those views preserves the packet.

Each assignment also tracks the greatest committed source revision. An overlapping older read returns the newer committed state and its packet instead of rolling tracking backward. Equal-state publications advance that revision without changing the packet. States skipped between reads are not counted.

`enable(FALSE)` disables logical input; any nonzero value enables it. Focus evidence comes from the independent [observer](focus.md).

Independent history consumers provide [keystrokes and typed Guide waits](xinput-events.md).

## Rumble and controls

With output configured, `set_state` submits rumble for the exact current assignment. Left and right motor strengths normalize independently from 0–65,535 to 0–1. Success means logical acceptance, not native delivery or physical vibration. Zero remains a present intent in normal [output arbitration](output.md).

Disabling XInput revokes that service's contribution and retains its latest qualified request. Requests made while disabled replace the cache. Enabling restores it, including zero, only for the same live assignment and eligible foreground epoch. Focus loss, expiry, reassignment and close invalidate stale rights and cached requests. Cleanup does not revoke independent output owners.

A known background request succeeds without being cached for replay. Unavailable focus or a stale captured assignment, Enable generation or foreground epoch cannot authorize a request. An input-only service returns `ERROR_NOT_SUPPORTED` for a live assignment regardless of focus; argument and connection errors take precedence.

`get_rumble_output_status` reports stored cache and restore status, not delivery. `power_off_controller` returns `ERROR_NOT_SUPPORTED` for a live assignment. The [GameInput rumble composition](gameinput-output.md) supplies native delivery and a foreground diagnostic.

## Queries

Queries project owned metadata into XInput values. Blocking metadata work and variable-length output copies run outside the presentation gate. Final assignment and connection validation determines which occupant the result belongs to. A metadata failure or mismatched target returns `ERROR_DEVICE_NOT_CONNECTED` without writing outputs.

| Query | Flavor behavior |
|---|---|
| Capabilities | Fixed values for 9.1.0; hardware-derived flags for 1.3 and 1.4. |
| Extended capabilities | Hardware-derived flags and available vendor, product, and revision IDs. |
| Audio device IDs | Supported for 1.4. |
| DirectSound GUIDs | Supported for 1.3 and 9.1.0. |

Dynamic capabilities report the low-frequency left motor and high-frequency right motor independently. The FFB flag is set when either is available. Ordinary 9.1.0 capabilities retain their fixed compatibility values.

Metadata sources validate headset association for the exact device and endpoint direction. Endpoint IDs and DirectSound GUIDs are independently available: a validated nonzero GUID remains usable when its endpoint ID is unavailable, and either identifier can establish voice support. Absent or ambiguous associations cannot supply available identifiers. Missing native IDs and GUIDs project zero; absent endpoint IDs are empty, and unsupported capability flags stay clear.

GameInput battery telemetry is unavailable; USB projects wired/empty battery status, while other transports project unknown/empty. Headset battery status is disconnected/empty.

Audio counts use UTF-16 code units and include the terminator for nonempty IDs. An absent endpoint has count zero. A null count skips that endpoint; a null buffer with a count requests its size. Render is processed before capture. A short buffer receives the required count without changing its string; earlier endpoint writes remain. Optional write flags describe the count and buffer writes, including partial failures.

## GameInput composition and diagnostic

`gameinput_xinput::synchronize` retains unchanged eligible connections, releases missing or ineligible ones, and fills free positions in inventory order. It presents at most four connections and can apply some changes before returning an error. The composition root serializes synchronization and keeps the runtime and focus observer alive through consumer calls.

For events, the coordinator runs runtime maintenance, synchronizes inventory, then calls `maintain_events` before consumers drain prepared keys. `get_keystroke` performs no history source work; its caller must arrange this explicit maintenance separately from time-sensitive reads.

Run the native diagnostic with:

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --duration-ms 10000 --require-device --require-reading --xinput-events
```

`--xinput` reports assignments, standard and extended state, packets, focus evidence, and capability queries. `--xinput-events` also enables bounded keystroke drainage and a typed Guide waiter, with cancellation and completion reports. The waiter runs separately from maintenance and is joined before runtime shutdown.

A background probe correctly reports neutral state and ineligible event consumption; foreground input and physical Guide transitions require an eligible process and the corresponding controller actions. The supervisor enforces a process deadline and verifies cleanup.

Stop admitting calls and close presentation to wake Guide waiters, then join consumer calls before destroying its borrowed ports. Drain the runtime while the observer is still alive, then stop the observer. The registry outlives the runtime and all bindings. See [GameInput](gameinput.md) for native shutdown limits.
