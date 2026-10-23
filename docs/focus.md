# Focus observation

`focus::observer` owns an immutable source and one observation worker. `make_current_process_source` binds the calling process's PID and creation time. The composition root supplies a nonzero observer incarnation that is unique across its observer lifetimes.

## Published evidence

Snapshots report `foreground`, `background`, or `unavailable`, together with process identity, a revision, and the observation's monotonic start time. Evidence changes advance the revision; repeated fresh observations preserve it. `completed_observations` counts completed probes independently of those changes.

Initial evidence is unavailable. A missing foreground window, failed process lookup, or mismatched source identity cannot establish foreground eligibility. Revision exhaustion is permanent and makes the observer ineligible. Diagnostic counters saturate instead of wrapping.

Evidence expires 250 ms after the observation starts; an age of exactly 250 ms is expired. Readers project expiry onto their local snapshot, even while the source is blocked. Before accepting another result, the writer applies expiry to the stored evidence. Results aged 250 ms or more at publication cannot renew evidence. Recovery advances the revision, so it cannot authorize a read captured before expiry. A captured snapshot must itself remain fresh even when subsequent observations preserve the same revision.

`foreground_epoch` identifies the observer, process, and foreground revision without retaining an observation timestamp. Guard checks match it against current fresh evidence. Long-lived event consumers use this identity; their in-flight operations still retain the captured snapshot's freshness requirement.

The worker waits nominally 4 ms after each completed observation. The freshness budget allows scheduling margin over this interval; it is a presentation policy, not an OS scheduling or completion deadline.

## Synchronization and ownership

Source calls run outside the presentation gate. Readers and writers access stored evidence under that gate.

XInput captures initial focus and control state together, then reads and translates the input sample outside the gate. Its final guarded turn revalidates eligibility and commits packet state. Keep guarded work local and bounded; perform provider queries, waits, and variable-length copies outside the gate.

`stop()` closes publication, requests worker stop, and joins outside the gate. It is idempotent; serialize calls to it. An in-flight result cannot publish after close, and the owned source remains alive until the worker finishes. An external call that never returns can delay shutdown indefinitely.

Keep the observer alive through all consumer calls and guards. Stop new calls and join their callers before destruction. A successor observer receives a new incarnation; a copied snapshot does not authorize work through it.

The native source observes the current process without changing the foreground window. See [XInput presentation](xinput.md) for its consumer policy and [development](development.md) for test selection.
