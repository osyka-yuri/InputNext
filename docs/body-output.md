# Body output

A body lease gives one public output context exclusive semantic rumble ownership for an exact controller connection. It carries a finite lifetime, classic fallback amplitudes and a classic or prefer-HD route preference. Trigger and LED arbitration remains independent.

The native models are declared in [output_body.hpp](../include/inputnext/output_body.hpp). Lease operations belong to the [output authority](output.md); native route completion belongs to the [executor](output-execution.md).

[Body PCM](body-pcm.md) adds owned clips and offline rendering to the same lease.

## Acquire, renew and clear

Call `authority::submit_body` with an open public context. Acquisition uses lease ID zero and body sequence one. TTL ranges from 1 to 60,000 milliseconds and starts at API entry, before waiting for the authority gate. Fallback amplitudes must be finite values in `[0, 1]`.

Retain the returned lease and complete audio identity. Renewal supplies those exact identities and a strictly greater body sequence. Body sequences are independent of ordinary channel submission sequences. Renewal may update lifetime, amplitudes and preference; it cannot change the audio binding. Another active body owner, including a duplicate acquisition by the same context, returns `busy`.

`clear_body` requires the active lease, exact audio identity and a greater body sequence. It removes the body contribution while preserving ordinary rumble, triggers and LEDs. A cleared, expired or revoked lease cannot be renewed. A new acquisition receives a fresh lease ID.

Owned zero rumble still masks ordinary rumble. Clearing or expiring the lease reveals the latest eligible ordinary contribution. Context close, foreground revocation and connection retirement also remove the body contribution under the authority's existing lifetime rules.

## Audio identity and route intent

The trusted composition's `audio_routes()` capability manages scalar audio attachments. Each attachment has an authority-scoped identity, an exact connection target, immutable route support and mutable readiness. It owns no native audio object. Its callers must quiesce before the composition is destroyed.

An all-zero audio identity is unbound. A bound acquisition names the current exact attachment. Detaching and attaching again creates a new identity; an existing lease never transfers to it. An old bound lease may continue with classic fallback but needs a fresh acquisition to bind the replacement.

Prefer-HD selects classic fallback when audio is unbound, unsupported, unavailable or lost. Diagnostic rumble overrides body output and forces classic. Readiness loss, diagnostic preemption and a qualified failed HD attempt latch the lease to classic. Restoring readiness or removing diagnostics does not restore HD automatically; an explicit renewal may request it again when the original audio binding still exists.

The receipt reports accepted logical intent. An HD intent authorizes an attempt; it does not establish native completion or audio playback. Qualified USB DualSense backends support HD mode; the [WASAPI adapter](hd-haptics.md) owns their audio attachments. Other GameInput backends retain classic output. Controlled sinks can exercise the route contract without sending hardware reports.

## Completion and resources

The executor acknowledges an HD route only after an accepted native call with a freshly qualified backend, admitted attempt, lease, audio identity and route revision. Its publication and attempt identify the call that established the proof. The acknowledgement does not change the desired command or cause another write by itself.

An unchanged HD route retains the same acknowledgement across trigger and LED publications, including while their native writes are pending or in flight. Those writes still deliver their requested values. The route proof does not confirm delivery of a newer full command; `last_completed_attempt` reports each completed call separately. Masked ordinary rumble changes leave the effective publication unchanged.

`backend_progress::acknowledged_body_route` is stored observation. `executor::qualifies_route` freshly checks that exact acknowledgement against current authority and execution state. Route changes, expiry, close, audio loss, preemption, explicit retry or backend replacement can invalidate it before the next coordinator refresh. Renewal preserves the route revision when its exact identity, selected route and fallback reason are unchanged. A reported native failure clears the proof and latches its still-current HD route to classic even if trigger or LED changes have superseded the completed publication. A retry requested during a write requires a later accepted attempt. Neither operation grants an audio render permit or measures a physical effect.

The authority has 32 public and 32 internal context records, with at most one active body lease per target and 16 scalar audio attachments. Lease and audio IDs never repeat within an authority incarnation. Admission reserves logical-version capacity for expiry, route invalidation, readiness loss, detach and context close. Rejected admission does not consume an identity or partially install a lease.
