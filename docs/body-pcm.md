# Body PCM

Body PCM supplies immutable samples for one nonlooping body voice per controller connection.
It extends the [body lease](body-output.md); it does not open an audio device or establish physical
HD output.

## Preparation and limits

Obtain an authority's `pcm_preparer()` and call `prepare` with interleaved float samples. The
prepared format is mono or stereo at 48,000 Hz, with at least one frame, finite samples in
`[-1, 1]` and finite gain in `[0, 1]`. The span must contain exactly `frame_count * channels`
samples. Other sample rates are rejected; preparation does not resample.

Preparation copies and validates the samples before returning. Keep the input readable and
unchanged until the call returns; afterward it can be modified or freed. The prepared handle is
immutable and can be copied without copying the sample array. It belongs to the authority's
store and cannot be submitted to another authority.

Each store allows 64 MiB of sample arrays and 64 distinct buffers, including pending allocations
and buffers retained by callers or admitted rendering. There is no separate duration limit; a
single clip can use the entire byte budget. Checked arithmetic rejects malformed sizes. Byte or
buffer exhaustion returns `capacity_exceeded`; allocation failure returns `resources`.

The store reserves capacity before allocation and releases it after actual sample deallocation.
Stopping playback does not release a caller's retained handle. A preparer or prepared handle may
outlive its authority; accounting remains attached to the storage. Preparation and reclamation
run outside authority and execution control locks.

## Playback commands

The three-argument `submit_body` combines lease submission with a `body_playback_update`:

| Command | Effect |
| --- | --- |
| `keep` | Preserve the current clip, position and terminal state. Supply no replacement. |
| `stop` | Retire the clip and retain intentional PCM silence. Supply no replacement. |
| `replace` | Install a same-store prepared clip at position zero with a fresh clip revision. |

The two-argument lease submission uses `keep`. Replacement commits atomically; rejection
consumes neither its sequence nor a clip revision. Expiry and revocation maintenance may still
retire earlier playback. Replacement may need another resident voice while an earlier block is
still in flight; the fixed bank holds at most 64 resident voices.

Completion, stop and revocation never replay through `keep`. Replaying the same prepared handle
requires an explicit `replace`. An unchanged route keeps its revision and accepted route proof
across lease renewal and PCM replacement; the clip revision independently identifies playback.
`stop` specifies PCM silence. Full actuator silence also requires zero classic fallback rumble.

Clear, expiry, context or connection loss, audio loss and an active voice leaving HD retire the
affected playback. A new audio or backend incarnation cannot inherit it. An unchanged backend's
explicit retry suspends admission until its route is acknowledged again, then may resume the
remaining clip. A clip that has never been admitted can wait for its original audio route to
become eligible; this does not revive retired playback.

## Render admission and drain

The trusted execution path combines fresh authority qualification with the exact accepted route,
backend and clip revision to admit one block. A stored route acknowledgement alone is insufficient.
Only one block per exact target can be in flight, including a block from a replaced voice.

A render permit is move-only and single-use. It borrows its authority and execution owners;
release every permit and quiesce callers before destroying those owners. Revocation closes new
admission while an already admitted block may finish. Backend cleanup waits for that block before
releasing native resources. Render and permit release do not allocate, take control locks or
destroy sample owners. Control collection reclaims retired or completed buffers outside those locks.

Rendering fills complete quad frames up to the admitted count and output capacity. Front left
and right are zero. Mono is duplicated to rear left and right; stereo maps to those channels
directly. Clip gain precedes the soft limiter `x - x*x*x/6` within `[-1, 1]`, with saturation outside
that range. The clip ends without looping and the remaining processed frames are silent. Any
trailing incomplete quad frame is untouched.
