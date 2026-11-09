# GetState performance

This benchmark measures sustained InputNext runtime polling on one Windows x64 machine. One or four threads call `get_state` continuously, without pacing, for the same controller in slot 0.

The workload stresses contention on one connection. It measures call duration and throughput under saturation, not controller-to-screen latency or frame-rate impact. It does not establish a worst-case bound or performance at a game's normal polling cadence.

## Conditions

- Intel Core i7-12700H, 14 cores / 20 logical processors, 32 GiB RAM.
- DualSense over USB; Steam was running.
- MSVC 19.51.36260, x64 Release, static CRT, `/O2 /Ob3 /GL /Gw /Gy` and full LTCG. Sanitizers and PGO are off.
- InputNext source `21b561e` (v1.0.0), GameInput SDK / bundled redist 4.0.306.
- The benchmark calls the runtime's ABI-1 table directly. XInput facade resolution is excluded.
- Each scenario and timing mode was measured once, using a fresh process, 2 seconds of warmup and 20 seconds of measurement.
- Idle means no benchmark output submissions. Rumble alternates both motor values between 2000 and 3500 at a requested 4 ms cadence. There is no HD workload.

Individual-call timing uses QueryPerformanceCounter (QPC) around each callback. QPC runs at 10 MHz on this machine; histogram bins are 100 ns (0.1 µs). Reported percentiles are bin lower bounds: 0.2 µs denotes the range from 0.2 µs up to, but excluding, 0.3 µs. Timer overhead is retained.

Batch timing places two QPC reads around 1000 calls. Its mean is total time across all thread batches divided by total calls, including loop and return-code checks. It has no per-call timer or histogram updates. Decimal places do not imply equivalent measurement accuracy.

Throughput is successful calls across all threads divided by the shared measurement duration. Batch mean includes time spent waiting inside each call; with four threads, it is not the reciprocal of aggregate throughput. The two timing modes are measured separately.

## Results

Batch timing:

| Threads | Load | Mean, ns/call | Throughput, million calls/s |
|---|---|---|---|
| 1 | Idle | 150.7 | 6.635 |
| 1 | Rumble | 154.0 | 6.492 |
| 4 | Idle | 1094.4 | 3.655 |
| 4 | Rumble | 1077.9 | 3.711 |

Individual-call latency:

| Threads | Load | p50, µs | p99, µs | p99.9, µs |
|---|---|---|---|---|
| 1 | Idle | 0.2 | 0.2 | 0.4 |
| 1 | Rumble | 0.2 | 0.2 | 0.4 |
| 4 | Idle | 0.5 | 13.9 | 91.2 |
| 4 | Rumble | 0.5 | 14.1 | 89.6 |

The eight reported runs had zero failed `get_state` calls. Only runs that retained foreground, matched the reported controller identifiers and completed output cleanup are included.
