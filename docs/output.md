# Logical output

The C++ output components own effect contexts and arbitrate their desired state. A successful submission accepts logical state. The [native execution layer](output-execution.md) separately schedules and qualifies physical attempts; neither acceptance nor a setter result proves a measured hardware effect.

Values and requests are declared in [output.hpp](../include/inputnext/output.hpp); context operations and snapshots are declared in [output_authority.hpp](../include/inputnext/output_authority.hpp).

`inputnext_output_policy` provides values, validation, deadline arithmetic, and pure winner selection. `inputnext_output_authority` owns contexts and transactions and uses the existing device registry and focus observer. Input publication and XInput reads have no dependency on either output implementation.

## Ownership and lifetime

An authority borrows one registry and one focus observer. Stop calls and join callers before destroying the authority; both borrowed owners must remain alive through its destruction.

Each authority generates an opaque incarnation during construction. Construction fails if a valid incarnation cannot be generated. Independent authorities retain distinct identities when sharing a runtime, registry and observer, including after destruction and reconstruction.

A context identifies its authority incarnation, runtime, device, exact connection, context ID, and public or internal class. Internal identities also include their owner key. Replacement connections and authorities do not inherit it. Context IDs never repeat within an incarnation; IDs and channel revisions never wrap. Foreign identities are rejected before target maintenance.

Each authority has separate pools of 32 public and 32 internal records. Both open contexts and closed records awaiting acknowledgement consume capacity. Intent expiry does not free a context. Open is synchronous: failure creates no context and consumes no ID.

`inspect` returns stored information about an open record and services local TTL expiry. Its foreground field is the opening epoch. It does not freshly qualify the connection or foreground eligibility; success grants no execution permission. Focus or retirement cleanup can remain pending until a qualifying operation or `maintain` observes it. A retained closed identity returns `revoked`; an unknown or reclaimed identity returns `stale_context`. Close retrieves the retained receipt.

Public contexts have fixed `game_semantic` priority. [Trusted composition](../src/output/composition/composition.hpp) owns its authority and issues [internal producers](../src/output/composition/internal_producer.hpp) with an immutable owner key, priority, TTL permission and foreground policy. A borrowed public authority cannot issue producers.

Producer copies share the same grant and borrow the authority. Destroy them and join callers before composition destruction. These interfaces organize trusted native code and provide no process isolation.

Internal submit, clear, close and acknowledgement require the producer's complete grant to match the record before any maintenance. Internal open deduplicates an open exact-target owner key only when all grant properties match. Closed records do not deduplicate.

## Requests and arbitration

Four channels arbitrate independently: left trigger, right trigger, rumble, and the LED group. Each channel selects the highest eligible priority, then the latest accepted commit at that priority. The entire rumble value belongs to one winner.

A [body lease](body-output.md) supplies a separate coherent rumble overlay. It takes precedence over ordinary rumble, including when its fallback is zero; diagnostic rumble overrides it. The four discrete winners remain available in the logical snapshot. Native projection applies the body overlay to represented rumble and excludes masked ordinary rumble until the body lease is removed. Changes to that hidden contribution do not publish another native command.

All provided value fields are validated, including unselected channels. Trigger kinds and layouts must be known; floating values must be finite. Trigger positions and strengths and rumble amplitudes range from 0 to 1, trigger end is at least start, frequency ranges from 0 to 255, and each of ten zone levels ranges from 0 to 8. Player LEDs range from 0 to 4. RGB consists of three bytes, and the microphone LED is boolean.

OFF, zero rumble, and zero trigger strength are present intents. They can mask lower-priority owners. Clear and expiry remove a contribution and can reveal the next owner.

Submit and clear share a strictly increasing, nonzero sequence per context. Each accepted selected channel gets a new revision and commit order, even when its value is unchanged. A channel also stores the sequence of its own last accepted mutation. Updating another channel leaves that provenance unchanged.

Clear requires exact current revisions for every selected channel. One stale selected revision rejects the entire request. Unselected revision fields are ignored. An absent channel can be cleared when its revision matches, including initial revision zero; the accepted clear still advances its revision and the context sequence.

## Time and snapshots

Finite TTL ranges from 1 to 60,000 milliseconds for both public and internal contexts. Trusted internal contexts may also permit zero TTL for an indefinite intent. Indefinite intents have no deadline.

TTL starts at submission entry, before waiting for the authority mutex. Checked arithmetic rejects an unrepresentable deadline. A request already expired when admitted is rejected. Evaluation time never moves backward, expiry includes the exact deadline, and reads do not renew TTL.

`read_snapshot(target)` performs targeted cleanup and returns owned copies of the four optional channel winners and optional body overlay. It includes exact owner identities, per-channel provenance, the authority incarnation, logical version, evaluation time, and earliest finite expiry among eligible active contributions. Empty snapshots and maintenance reports also identify their authority. Expiry preserves the last accepted channel revision and sequence. A snapshot remains unchanged after later mutations or record reuse.

Logical versions order context admission, accepted mutations, expiry, and revocation within one authority incarnation. Compare versions only when their incarnations match. Acknowledgement reclaims capacity without advancing the logical version. A snapshot at or after a context's revocation version cannot include that context. Snapshot qualification describes its observation point; execution must qualify the target and authority again before sending output.

## Focus and connection changes

Public contexts are foreground-bound. Their `open`, `submit` and `clear` operations require fresh foreground evidence and retain the opening epoch. Refreshing evidence within that epoch preserves eligibility. Qualification closes bound contexts when evidence is lost or stale, or a later epoch is observed. Closed contexts never resume after foreground recovery.

The authority checks the stored epoch against fresh observations around exact-target capture. Focus guards are released before registry capture. Snapshot cleanup removes ineligible bound owners while still returning eligible unbound internal owners in the same call. Close and acknowledgement require no foreground evidence.

Each usable snapshot freshly captures its exact target. Successful capture is the logical liveness point; an overlapping retirement may order after it. A call starting after completed retirement cannot capture that connection. Pins are released during the operation and never retained in context records.

`maintain(target)` services expiry, foreground revocation, and observed retirement. Call it after registry retirement or replacement and when servicing deadlines or focus changes. Open also performs a bounded retirement sweep before reporting pool exhaustion. The authority starts no worker. An attached executor qualifies its registered targets automatically; standalone cleanup depends on another qualifying call or explicit maintenance.

## Close and acknowledgement

Close uses the exact context ID as its request key, revokes that owner's contribution, and retains a repeatable receipt. The receipt contains the context identity, close key, logical revocation version, and reason. Automatic foreground revocation creates the same kind of retained receipt, retrievable by repeating close.

Public acknowledgement accepts public receipts. Internal producers acknowledge internal receipts matching their complete grant. Both require the same authority incarnation and the stored receipt in full. Acknowledgement frees the record once without servicing TTL or focus; a duplicate acknowledgement is stale. Observed exact-target retirement is an alternate reclamation path, so an acknowledgement after reclamation is also stale. An old receipt cannot release a replacement connection or another owner's record.

Ordinary admission preserves enough logical-version headroom for cleanup. Full pools, exhausted mutation counters, and foreground loss cannot prevent an existing context from being closed and acknowledged. Counter or validation failure does not partially accept a request. Required expiry, foreground, or retirement cleanup may commit separately before that request is rejected.

Receipts describe logical revocation and are retained only in memory. They carry no physical dispatch or completion proof.
