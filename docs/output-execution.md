# Native output execution

`inputnext_output_execution` drives the desired state held by the [output authority](output.md). It schedules qualified native calls and manages retries and sink lifetimes. It builds for x64 and x86 without GameInput. The [GameInput output adapters](gameinput-output.md) supply native output separately.

The public interfaces are [output_executor.hpp](../include/inputnext/output_executor.hpp), [native_output.hpp](../include/inputnext/native_output.hpp) and [output_execution.hpp](../include/inputnext/output_execution.hpp).

## Ownership

An executor borrows its authority. Shut down and destroy the executor before the authority; the authority's registry and focus observer must remain alive. One executor may attach to an authority at a time. Recreating it obtains a fresh execution identity.

Successful backend registration transfers ownership of a `native_sink`. A failed registration leaves the candidate with its caller. Backend handles include the exact connection, authority incarnation, execution identity and backend identity. Publication and attempt IDs belong to that backend and never wrap.

There is at most one current backend per runtime and device key. Replacement accepts the same connection or another live connection with the same runtime and device key. It creates a fresh backend identity. A stale expected handle cannot replace the winner of a competing operation.

Each runtime/device key has one serial delivery lane across connection and backend replacement. Delivery from a replacement waits for earlier writes and sink destruction on that lane. Registration, retirement and logical mutations do not wait for that physical work. Different keys have independent delivery lanes.

The limits are 16 current device lanes, 128 resident backends and 128 resident lanes. Reservations, retiring and cleaning backends, unacknowledged drained records and lanes awaiting worker shutdown count toward these limits. Blocked work retains capacity. Admission failure preserves existing registrations.

## Publication and admission

Registration declares an immutable, nonzero subset of the four supported channel bits. Publications retain the effective represented winners and their full provenance. A body lease masks ordinary rumble in this projection; the logical snapshot retains it for restoration after clear or expiry. Diagnostic rumble remains represented. Unsupported requested channels are reported separately. Native acceptance covers only the represented channels.

Registration also declares immutable body-route support, defaulting to classic-only. HD support requires represented rumble. A classic-only backend projects an HD body intent to its classic fallback with `unsupported_transport`; it retains the lease and route identity for qualification. Qualified USB DualSense backends support HD; other [GameInput backends](gameinput-output.md) use classic output.

Absent represented intents produce neutral values. A present zero or OFF intent remains distinct from absence for qualification. The initial publication requires a real native call even when it is neutral.

The coordinator refreshes registered targets at a nominal four-millisecond period. `request_drive` can expedite a refresh; correctness does not depend on that hint. Pending publications coalesce to the newest applicable projection. Input publication, ready XInput reads and focus observation do not wait for physical delivery.

Immediately before a native call, the engine checks the exact backend and publication, then freshly qualifies the represented state through the authority. This services TTL, foreground epochs and exact connection liveness using the authority's existing rules. A snapshot captured earlier grants no execution permission.

An admitted call may overlap a later logical close, focus loss, retirement or replacement. Its completion confirms delivery only if its backend, publication, attempt and freshly qualified projection still match. A late successful call can have affected hardware without confirming the current publication.

## Native sink contract

`native_sink::write` is synchronous and `noexcept`. The command is borrowed for that call only. The sink must finish all native work using it before returning; it must not retain command storage or schedule an unqualified later send. An adapter using asynchronous OS I/O must wait for its actual completion within this boundary.

A sink declaring HD support must preserve an accepted exact HD route while processing later commands carrying that same route intent, including during trigger and LED writes. If it cannot preserve the route, it must return a nonaccepted result. Setter acceptance does not establish physical audio playback.

`accepted` records setter acceptance without measuring hardware output. For classic output, `failed` and `unsupported` block automatic retries until a changed represented publication or explicit retry. `disconnected` retires the backend.

Consecutive qualifying `retryable_failure` results for classic output schedule automatic retries after nominal delays of 4, 8, 16, 32 and then 64 milliseconds. Further failures retain the 64-millisecond delay; there is no total retry budget. A changed represented publication starts a fresh cycle immediately.

`request_retry` expedites an attempt, including identical values, without resetting consecutive failures. Requests accepted before successful admission can coalesce into that attempt. Any request accepted after admission requires a later qualified attempt; multiple pending requests can coalesce. An explicit retry clears the route acknowledgement immediately, and refresh or unrelated publication changes preserve the pending retry. Retirement cancels future admission, and replacement starts without an old backend's pending retry.

`progress` reports stored facts without requalifying the connection or foreground. The newest admitted attempt and latest completed attempt are distinct. Each completion owns its attempt and publication identities, returned result and exact command payload, including failed or stale completions. That payload is independent of later logical changes; setter acceptance remains historical for its publication.

For [body output](body-output.md), progress also retains a historical accepted HD route acknowledgement. The same exact route preserves that proof across unrelated publication changes, admissions and accepted writes. `qualifies_route` freshly checks the stored acknowledgement, backend and authoritative body route independently of full-command delivery. A route proof can remain valid while the latest trigger or LED command is pending.

A different route, explicit retry, reported native failure, retirement or replacement clears the proof. A qualifying HD failure latches that lease to classic and schedules a fresh fallback publication even when an unrelated publication has superseded the failed command. A stale route cannot change a newer lease. A disconnected completion still retires its exact backend regardless of route qualification.

`consecutive_retryable_failures` counts qualifying transient failures for the observed publication and saturates. A qualifying accepted, failed or unsupported result resets it; retirement freezes it. `scheduled_retry_delay` reports the nominal automatic delay while that schedule applies, including after its due time. It is zero during a write, explicit retry, deferred refresh or retirement, and does not report remaining time.

## Retirement and shutdown

Retirement closes future admission and returns a repeatable causal receipt. It does not cancel an already admitted native call or prove that neutral output reached hardware. Logical context close and its acknowledgement remain separate from backend retirement.

The delivery lane destroys a retired sink after its admitted native call and any [PCM render block](body-pcm.md) finish. Destruction uses the same physical serialization as writes. The backend remains `cleaning` until its destructor returns; only then is it `drained`. Retirement acknowledgement requires the exact receipt and a drained backend before releasing its record. An old receipt cannot release a replacement.

Automatic retirement and replacement also retain records until acknowledgement. Keep every registration handle. Call `retire(handle)` to retrieve its original receipt, observe `progress(handle)` until `drained`, then call `acknowledge_retirement(receipt)` to release capacity.

Shutdown is terminal. It closes admission, stops the coordinator, finishes registration rollback, waits for native calls, admitted render blocks and sink destruction, joins delivery workers and detaches from the authority. It can wait indefinitely for an unreleased render permit, blocked native call or destructor. Return means quiescence, not successful neutralization or a bounded stopping time.

To request neutralization, first clear or close logical contributions and observe delivery on the live backend before retiring it.

Call `shutdown` and destroy the executor from a thread other than its delivery workers. Sink code may request retirement without waiting for its cleanup.
