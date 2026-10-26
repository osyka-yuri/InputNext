# HD haptics

USB DualSense and DualSense Edge use a WASAPI render stream for HD body vibration. The
[body lease](body-output.md) selects the route; [prepared PCM](body-pcm.md) supplies the samples.
Bluetooth remains input-only.

Native audio workers start only for managed backends that advertise HD route support.
Classic-only backends remain eligible for ordinary rumble without opening an audio stream.

## Device and format qualification

The audio service uses the exact haptic endpoint ID supplied by GameInput. It verifies the
active render endpoint's ID and container against current controller metadata. Missing or
conflicting evidence leaves HD unavailable. It never chooses a default audio device, a headset
endpoint or a device by its display name.

The stream uses shared, event-driven WASAPI with four channels, 48,000 Hz and 32-bit float
samples. The required channel mask is front left/right and rear left/right (`0x33`). Both the
endpoint mix format and exact format-support query must accept this layout. Other rates,
integer samples and closest-format suggestions are rejected; there is no implicit resampling.
Front channels remain silent; the two rear channels carry the haptic samples.

The qualified USB output sink selects the controller's HD mode. An accepted mode-setting
report establishes the route acknowledgement; it does not establish that audio samples reached
the actuators. Trigger and LED commands retain their independent behavior.

## Admission and delivery

Each audio incarnation is bound to one exact managed output backend. A control worker selects
the current clip and admits a block against fresh authority state. It hands one move-only
permit to the native worker. WASAPI buffer processing performs no authority queries, control
locking or sample allocation. Requests notify the control worker directly; coalesced notifications
remain pending until consumed. The worker retains the [PCM submission token](body-pcm.md#render-admission-and-drain)
through `ReleaseBuffer`.

Accepted native buffers, consumed source frames and user-observed effects are distinct facts.
If submission fails after mixing, consumed samples are not rewound or replayed. A canceled
offer produces silence without advancing the clip. An already admitted block may finish across
logical revocation; new blocks require fresh admission.

## Loss, recovery and shutdown

Endpoint failure, stale qualification or backend replacement closes the audio incarnation.
Control withdraws expired readiness even when native work is blocked. Temporary metadata
contention retries within the existing qualification window; it cannot extend that window.
Readiness loss and detachment retire its clips, including clips that have not started. A
recreated stream receives a fresh audio identity. An existing lease never transfers to it;
playback requires a new acquisition and explicit replacement.

Prefer-HD falls back to classic rumble when HD is unavailable. The lease's fallback amplitudes
determine that output. A stored route acknowledgement cannot override current audio readiness.

All opening, running, retiring and unjoined audio workers share a limit of 16 resident slots.
Each accepts a native ring of at most 4,800 quad frames. A retained endpoint or container blocks
another owner of the same native resource until cleanup and join finish, even after a controller
reconnect receives a different runtime device key.

Port creation failures remain visible in per-target progress during retry backoff without holding
a resident slot.

The native worker creates, uses and releases its COM stream objects on the same thread. Audio
shutdown closes admission, cancels unclaimed permits, finishes admitted transactions and joins
native workers before the output executor or GameInput is destroyed. A blocked native call can
delay shutdown indefinitely; a timeout does not authorize releasing objects still in use.

## Manual diagnostic

Build with `INPUTNEXT_BUILD_DIAGNOSTICS=ON`, then run:

```powershell
./build/windows-x64/tools/gameinput_probe/inputnext_gameinput_probe.exe --dualsense-hd
```

Close Steam and other controller tools for this diagnostic to isolate its output. Connect the
controller by USB, hold it and keep the diagnostic window active throughout the sequence:

1. Click **Start**. Expect one half-second HD pulse, or classic fallback if the window reports HD
   unavailable. Choose **Works - Next** if you felt it.
2. Expect brief ordinary vibration. Choose **Works - Next** if you felt it.
3. Expect no vibration. Choose **Works - Next** if the controller is quiet.
4. Unplug the USB cable, reconnect the same controller and click **Start** again. Expect one new
   pulse, then confirm whether you felt it.

Choose **Does not work - Next** whenever the expected result is absent. Clips play once and
never replay automatically. The window closes after the final confirmation.
Effects are finite; confirmation remains available after they end. **Abort** ends the run.
There is no overall time limit unless `--duration-ms` is supplied.

The diagnostic starts output only after Start and reports native delivery separately from user
observations. Automated tests use controlled native ports and do not establish physical vibration
or latency.
