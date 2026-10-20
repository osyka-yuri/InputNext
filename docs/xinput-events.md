# XInput events

`xinput::service` consumes an independent history reader for each assigned position. `gameinput_xinput` supplies these reserved readers and the system QPC frequency. Public history readers retain their own cursors. See [input history](history.md) for acquisition and retention limits.

## Keystrokes

`get_keystroke` supports XInput 1.3 and 1.4. Version 9.1.0 returns `ERROR_NOT_SUPPORTED` before argument validation. A null output or invalid position returns `ERROR_BAD_ARGUMENTS`; the reserved argument is ignored.

`get_keystroke` retrieves prepared events without opening, reading, or closing history. Call `pump_events`, or the composition's `maintain_events`, separately to prepare events. Retrieval does not advance repeats or process Guide transitions, even when source history is pending.

A specific disconnected position returns `ERROR_DEVICE_NOT_CONNECTED`. A connected position with no deliverable event returns `ERROR_EMPTY`, including while disabled, background, recovering, or busy. Results use the guarded dequeue checks from that invocation. Only success writes the output. `XUSER_INDEX_ANY` rotates after the last successful position; empty calls preserve that order. If no key is available, it returns `ERROR_EMPTY` when at least one scanned position was connected, otherwise `ERROR_DEVICE_NOT_CONNECTED`.

Buttons, triggers, and stick directions produce key-down and key-up events. Trigger activation is strictly above 30. Stick activation is outside the corresponding XInput deadzone, with eight directional sectors. Guide remains available in extended state and history but produces no keystroke.

Initial, recovery, reassignment, focus-return, and re-enable baselines seed held input silently. A later key-up can therefore appear without a preceding key-down. Held keys repeat after 500 ms, then every 100 ms. Missed intervals coalesce into one repeat per key per pump.

Only consumed event timestamps and qualified history checkpoints advance repeats. Wall time and `get_state` calls do not advance them. A transition at a repeat deadline is processed before that deadline's repeat. Late, newly sequenced events retain their transitions and use at least the previously accepted consumer time.

Each position holds at most 64 generated events. Transition and repeat groups fit completely or trigger recovery. History gaps, malformed batches, provider failures, and queue overflow clear pending events and require a new silent baseline. If a batch has a valid prefix followed by a failure, the pump processes that prefix, then clears pending events during recovery.

## Qualification and pumping

Every delivery requires the exact assignment, a live connection, fresh foreground evidence, and the same logical Enable generation. Focus loss, expiry, disable, replacement, or close invalidates pending output. Continuously refreshed foreground evidence preserves queues and Guide waits beyond 250 ms when its identity and revision remain unchanged.

Each in-flight pump additionally retains its original focus snapshot's 250 ms lease. A blocked operation cannot renew that lease with later observations. This protects its commit independently of the lifetime of already qualified consumer state.

`pump_events` processes one position and at most one history batch per call. History open, read, close, and native destruction run outside the presentation gate. Another caller can retrieve qualified prepared keys, operate another position, or read cached state while that work is blocked. Keep source maintenance separate from time-sensitive input reads: a sequential coordinator can still wait for a synchronous native call to finish.

## Typed Guide waits

`begin_guide_wait` registers a service-scoped ticket for one position. `wait_for_guide_button` awaits that ticket without pumping history; another caller must maintain events. Completion requires a committed, qualified, known released-to-pressed Guide edge after registration. Previously unread history may supply that edge; registration does not establish its physical occurrence time.

An initially held or unknown Guide state seeds silently. Registration before the first baseline is allowed. A recovery reset invalidates an existing wait rather than carrying it across an uncertain interval.

There is one outstanding ticket per position, including completed or cancelled tickets. For a valid ticket, `cancel_guide_wait` returns `true` only when it cancels a still-eligible pending wait; otherwise it returns `false`. Cancellation neither consumes the ticket nor waits for native history work.

Only one call may await a ticket. A second await or a release during an active await returns `busy`. The active await consumes the ticket when it completes. Otherwise, `release_guide_wait` frees it, cancelling a pending wait first. Unknown or consumed tickets return `invalid_ticket`; malformed tickets return `invalid_argument`. Ticket IDs never wrap or repeat within a service instance.

Pending waits and signalled successes remain subject to connection, focus, Enable, history, and close qualification until await returns. Failures report `not_connected`, `ineligible`, `history_reset`, or `closed` as appropriate. Other terminal failures remain terminal. Periodic qualification checks detect retirement and focus expiry even without another pump.

These are typed C++ operations. Undocumented native Guide wait/cancel exports require a separate ABI contract.

## Composition and shutdown

The coordinator orders runtime maintenance, inventory synchronization, `maintain_events`, and consumer observation. The event-enabled service borrows its source and uses an immutable nonzero QPC frequency matching that source. The presentation-only constructor remains available: connected keystroke reads are empty and Guide registration is unsupported.

Stop admitting calls, close presentation to revoke output and wake waiters, then join service callers. Release remaining tickets before stopping and draining the runtime. Keep the focus observer and registry alive through that drain. Destructors close idempotently; callers must already be quiescent before destruction.

Logical cancellation does not cancel synchronous native I/O or free its executing context. A native operation that never returns can delay cleanup indefinitely. See [GameInput ownership](gameinput.md).
