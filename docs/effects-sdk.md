# Effects C SDK

The Effects SDK exposes timed rumble, adaptive triggers, LEDs and body PCM through
[input_next_effects.h](../sdk/input_next_effects.h). It shares the native session and exact
device targets of the [Core SDK](core-sdk.md) and [XInput DLLs](xinput-dll.md). Use a runtime
matching the caller's x86 or x64 architecture.

## Loading and contexts

Load `inputnext_runtime.dll` by absolute path and resolve
`input_next_runtime_query_effects_api`, exported at ordinal 2. Request
`INPUT_NEXT_EFFECTS_ABI_VERSION` and validate the table's exact size, version and all ten
callbacks. Unsupported versions return null. Querying pins the shared runtime owner without
starting GameInput. Call SDK functions outside `DllMain` and the loader lock, and retain your
module reference until all calls finish.

Initialize every outer and nested `struct_size`; keep reserved fields and x86 PCM pointer
padding zero. `resolve_target` accepts the Core selectors and returns current capabilities.
Resolution can query native metadata. Resolution and opening may initiate foreground-gated
startup; mutations require a ready session. Public contexts use the
foreground-bound game priority.

Retain each returned context. Its runtime and opaque ID identify one exact native owner;
reconnect or foreground revocation requires a fresh context. The 32-context public limit
includes closed contexts awaiting acknowledgement. TTL expires an intent while its context
continues to occupy capacity. `query_context` reports stored state; subsequent operations
perform their own admission checks.

## Commands and buffers

`submit` uses a positive, increasing mutation sequence, a nonzero supported channel mask and
a TTL of 1–60,000 ms. Validate all numeric fields, including unused channels. Trigger
positions and strength are finite values in `[0,1]`, with end at least start; frequency is
a device parameter in `[0,255]`. Rumble amplitudes are in `[0,1]`. LED values use
`0x00RRGGBB`, player patterns 0–4 and microphone LED 0 or 1.

Keep the receipt's channel revisions for a conditional `clear` with a later sequence. A
timed zero or trigger OFF remains a present intent. Output arbitration selects
the represented output.

`submit_body` acquires a lease with `lease_id=0` and `sequence=1`. Updates use the returned
lease/audio identity and a greater body sequence. `PREFER_HD` can return classic fallback;
inspect the effective route and fallback reason. A new audio incarnation requires a fresh
lease. `clear_body` revokes the exact lease. Only one public body lease can hold a target at
a time; a competing acquisition returns `BUSY`.

PCM REPLACE supplies finite normalized float mono/stereo at 48 kHz with gain in `[0,1]`.
The caller supplies readable storage for the declared frame/channel count. The runtime
copies it before returning. The limits are 64 MiB of total sample storage and
64 distinct buffers, including retained owners; there is no separate duration cap. KEEP
preserves eligible playback without restarting it; STOP removes playback. Both require an
empty, correctly sized PCM descriptor. Playback is finite and does not replay after loss.

The original TTL includes validation, metadata queries, preparation and admission waits.
An expired request returns `NOT_ADMITTED` without installing the new intent. Successful
admission retains the original absolute expiry.

Inputs remain readable and stable during the call. An output may overlap an input descriptor
or replacement samples: output is written after input consumption. Distinct outputs of one
call must be disjoint. Failed operations preserve outputs. Meaningful output is returned
for OK and for REVOKED context information or close receipts.

## Capabilities, progress and cleanup

Capabilities combine available hardware facts with the exact registered backend. Motor
flags are independent. HD_READY and a nonzero audio instance identify qualified observed
readiness. `capabilities_revision` is a fresh observation serial; it grants no lasting right.
Sony Bluetooth remains input-only.

`publication_revision` is an opaque SDK token. `query_progress` distinguishes pending,
current completion, superseded, retired, unknown and undeliverable output. Software completion
describes synchronous setter or raw-report acceptance; PCM rendering and physical effects
have separate evidence. A nonzero `current_publication_revision` identifies the requested
token in the backend's current observed projection. It can be reported while dispatch is
pending; inspect `stage` for completion.

Only the latest discrete and body receipt per context remain queryable; overwritten or
acknowledged correlations return UNKNOWN. SUPERSEDED describes the current representation:
a surviving hidden intent can become represented later. Progress queries do not retry an
accepted mutation or restart a clip.

A native write failure applies only after the backend has observed the request's logical
version and its current projection represents that request. An earlier publication's failure
does not make a newer, unobserved request undeliverable.

Close with `close_request_id=context.id`. REVOKED returns an exact retained receipt; repeat
an uncertain close with the same key. Pass the complete receipt to `ack_close` to release
public capacity. Repeated exact ACK succeeds until its acknowledged slot is reused, then
returns STALE_CONTEXT. Native revocation, native drainage and public acknowledgement have
distinct completion boundaries. Cleanup remains available through a retained degraded
session. A failed cleanup preserves the obligation for an explicit retry.

## Example

Build `inputnext_effects_example` with `INPUTNEXT_BUILD_SDK_EXAMPLES=ON`. Its default mode
only loads and validates the table. `--rumble` or `--body` asks for confirmation before a
short effect, then performs bounded close/ACK cleanup. The example combines a C consumer
with a C++ module owner; both architectures use the same SDK contract.
