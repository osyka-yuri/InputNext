# Input history

`gameinput_runtime` provides bounded history through `open_history`, `read_history`, and `close_history`. Readers share a history bank for an exact connection target and advance independently. Disconnect or replacement ends that connection; an old reader never follows the replacement.

## Reading

`open_history` returns a borrowed reader handle. Its first successful read returns a baseline with sequence 1. Opening an existing bank selects its latest retained state; it does not promise a fresh hardware query. Missing initial input remains explicit until a valid baseline is available.

Each batch owns up to 32 records. Process all `count` records before handling `failure`: a provider failure can follow a valid prefix in the same batch. Retrying does not replay that committed prefix. A batch contains one domain, identified by connection and stream. A recovery baseline starts a new stream and is returned alone.

Source timestamps use microseconds. Effective timestamps and checkpoints use QueryPerformanceCounter ticks. Equal timestamps retain distinct events; a late Guide observation can have an older source timestamp while its effective QPC is clamped to preserve order. `guide_known` distinguishes an observed Guide value from unavailable initial knowledge.

`source` describes acquisition mode. Each record's `origin` identifies its emitting operation: `baseline`, `native_pad`, `callback_pad`, or `callback_guide`. A native-mode batch can contain captured Guide observations; the composed state preserves the other lane's last known value. Every baseline has `baseline` origin, including one selected from a retained reading.

`caught_up` qualifies the returned checkpoint after all preceding records have been consumed. A zero checkpoint is unqualified. A native checkpoint also requires callback coverage and fresh Guide knowledge.

## Acquisition and recovery

The native history path traverses GameInput Pad readings and merges captured Guide observations. Pad precedes Guide at equal effective QPC. Native Pad history takes precedence over captured Pad samples. Callback fallback replays accepted captured state; its checkpoint advances only with observations. Returning to native history uses a recovery boundary.

Retention overrun resets the lagging reader to the latest retained state. `lost_count` counts omitted incremental deliveries before that replacement baseline, including exact source losses at recovery boundaries through the selected state. The latest reading is represented by the baseline and is excluded from the count. For example, 257 unread ordinary readings produce a baseline and a known loss of 256. Other readers continue independently.

Source or callback coverage loss inserts an ordered recovery boundary; earlier records remain available within retention capacity. A reset preserves unknown loss across both retained and evicted boundaries. Unknown loss also covers accounting overflow and callback fallback overwrite, and always has `lost_count = 0`.

History retains 256 entries per bank, including baselines. The runtime admits 28 public readers and holds at most 32 resident banks, including opening, retiring, and destroying banks. Each read processes at most 64 cursor steps. These limits bound local work and storage, without bounding synchronous native completion time.

## Ownership and concurrency

History calls may perform native work. Calls sharing a busy bank return `busy`; another bank and registry latest reads remain independent. Ready latest reads never pump history.

Close revokes the handle immediately. In-flight operations retain their bank and native references until safe cleanup. Stop reports active history calls and pending history owners separately; final drainage includes both. A timeout cannot release resources that GameInput may still use.

Keep the runtime and registry alive through every history call. Stop new calls and join their callers before destruction. A copied handle does not extend either lifetime. See [GameInput ownership](gameinput.md) and [development checks](development.md).
