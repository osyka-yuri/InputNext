# Native input components

`inputnext_input` stores coherent gamepad samples. `inputnext_devices` manages connection identities, source ownership, and retirement. Both are C++23 libraries; neither requires controller hardware to use or test.

## Published samples

An `input_update` contains the gamepad state, a source timestamp in microseconds, and a nonzero observation value in QPC ticks. The current profile has 15 button bits, four stick axes in `[-1, 1]`, and two triggers in `[0, 1]`. Invalid values, including NaN and infinity, are rejected without replacing the current sample.

`latest_value::publish` assigns a revision to each accepted update. Equal state or timestamps still produce distinct revisions. Reading copies the complete sample without changing its revision or timestamps. Before the first publication, reading returns `error::no_data`.

Closing the value rejects subsequent reads and publications with `error::not_connected`. Revision exhaustion rejects further publication and preserves the last sample. Reads perform no backend refresh or history processing.

## Connections and source ownership

A `connection_target` combines a runtime identity, device key, and connection ID. The composition root supplies a nonzero runtime identity that is unique among its runtime incarnations. Replacement preserves the device key and allocates a new connection ID. Counters never wrap or reset during retirement.

Admission and replacement accept a reference to `std::unique_ptr<source_lifetime>`. On success, ownership transfers to the registry and the caller's pointer becomes empty. On failure, the caller keeps its pointer and an existing live connection remains intact. The lifetime port is used only for resource destruction; input reads never invoke virtual source methods.

The registry admits up to 16 live connections and retains up to 128 resident connections. Retired connections and resources still being destroyed count toward the resident limit. Admission reclaims eligible retired resources when capacity is exhausted; retained pins can legitimately prevent admission.

Admission returns reader and producer bindings for that connection. Bindings are borrowed values: they require the registry's storage to remain alive. After retirement or replacement, an old binding cannot silently refer to the new connection, even when internal storage is reused.

Publication and revocation serialize at the connection gate. A publication committed before revocation may succeed for the old connection; later attempts return `error::not_connected`. A replacement starts its own revision sequence and never receives an old binding's publication.

## Reading and retirement

A `read_pin` protects a connection's sample storage until the pin is released. It borrows registry storage, so the registry must remain alive through its final release. Retirement closes new read and publication admission. A read completed before that close may return the old connection's sample; a later read is rejected. A retained pin never reads the replacement's state.

`read_binding::validate_capture` checks admission at that instant without acquiring a pin or extending residency.

Releasing the last pin does not destroy the source. `drain_retired`, admission recovery, or shutdown performs final destruction outside the locks used by readers. A slow source destructor can delay other lifecycle operations; reads of other live connections remain independent of it.

## Shutdown

Use explicit `shutdown` while the registry is still alive. It closes all connections, waits for outstanding pins, and destroys owned sources on the calling thread. The destructor also performs shutdown as a fallback.

- Stop new external binding calls and join their workers before destroying registry storage.
- Outstanding pins may be released concurrently with shutdown. Do not call blocking shutdown from a thread that can release its own pin only after shutdown returns.
- Source destructors must finish or join their native callbacks before returning. They may use publication bindings, which reject a retired connection, but must not reenter registry lifecycle operations.

Connection locks protect only bounded value work. Ready reads and publications do not acquire the mutex that serializes admission, retirement, and destruction. This is not a hard real-time or wait-free guarantee.

See [development setup](development.md) for builds, test selection, and quality checks.
