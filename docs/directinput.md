# DirectInput 8

InputNext provides an app-local `dinput8.dll` for x86 and x64 processes. It presents the
normalized GameInput gamepad profile through DirectInput 8 A/W interfaces, using the same
runtime, device connections and cached input as XInput and the Core SDK.

This version supports immediate and buffered gamepad input, event notification and
foreground and background exclusive acquisition. Action mapping and DirectInput
force feedback are unsupported. Devices forwarded to Windows retain their
native DirectInput behavior.

## Loading

Build the complete development bundle with `inputnext_dinput_bundle`, or use the
[production archives](distribution.md). Choose the game's architecture. Deploy
`dinput8.dll`, `inputnext_runtime.dll`, `GameInputRedist.dll` and
`GameInputRawInputProxy.exe` from the same package beside the executable. XInput DLLs from
that package can be deployed alongside them when the game uses both APIs.

Back up an existing `dinput8.dll` before replacing it; other mods may use that name.
App-local imports and explicit local loads reach the facade. Registered COM activation
through `CoCreateInstance` still selects the registered system implementation. The
registration exports return `E_NOTIMPL` and do not modify Windows registration.

API discovery creates the process-retained runtime owner without starting GameInput.
Keyboard and mouse creation does not require GameInput startup. Gamepad discovery can
initiate the existing foreground-gated startup; background gamepad input is available
after the shared session starts. Keep all InputNext DLLs in a process from one bundle.
Call DirectInput outside `DllMain` and the loader lock.

Roots initialized with the shared GameInput provider's module handle retain system
DirectInput behavior. This prevents the provider's own device enumeration from waiting
for normalized devices that it has not published yet. Application roots use the
application's module handle and retain normalized discovery.

The facade exports the six standard entry points and ordinals: `DirectInput8Create` (1),
`DllCanUnloadNow` (2), `DllGetClassObject` (3), `DllRegisterServer` (4),
`DllUnregisterServer` (5), and `GetdfDIJoystick` (6). DirectInput 8 A/W and `IUnknown` are
supported; older DirectInput interfaces and COM aggregation are rejected.

COM objects retain their parents and their exact input connection. The runtime remains
loaded until process termination. Facade callers retain a module reference through each
export call; a returned COM object's implementation remains in the shared runtime.
`DllCanUnloadNow` reports outstanding DI objects and server locks without draining them.

## Discovery and connections

Enumeration includes all supported GameInput gamepads independently of the four XInput
positions. `XInputEnable` does not control DirectInput. A controller may be used through
DirectInput, XInput and the SDK concurrently; API selection belongs to the application.

Windows supplies keyboards, mice and other DirectInput devices. An exact, one-to-one PnP
interface-path match replaces the native gamepad entry with its normalized InputNext
projection and preserves the system instance GUID, device type and subtype. VID/PID or
names alone never hide a device. When correlation cannot be established, the native entry
remains visible and the InputNext device has a separate identity. If correlation changes,
new enumeration and creation use the current evidence. Existing COM objects keep their
original identity.
`DIPROP_GUIDANDPATH` is available when a unique match supplies the system class GUID
and interface path.

Session startup can finish before GameInput discovers a connected controller. On a
creation miss with a unique native interface path, `CreateDevice` allows up to two
seconds for shared device admission, then repeats exact-path qualification. Concurrent
discovery requests share that deadline. A session admits up to 32 discovery waiters.
A later request can start a new discovery window. Capacity exhaustion, expiry,
shutdown, ambiguous evidence or message-reentrant discovery retains native forwarding.
This wait does not run in `GetDeviceState`, and an existing object
never switches between native and normalized input.

An unmatched unique platform fingerprint gives a stable InputNext instance GUID. Missing
or ambiguous identity gives a connection-scoped GUID. A created InputNext device never
follows a reconnect: create a new device object for the new connection, even when its
instance GUID is unchanged. System devices retain the system implementation's lifecycle.

## Objects and formats

The profile exposes six absolute axes, fifteen buttons and one POV:

| Object | Source |
| --- | --- |
| X, Y | Left stick; Y increases downward |
| Rx, Ry | Right stick; Y increases downward |
| Z, Rz | Independent left and right triggers |
| Buttons 0–3 | South, East, West, North |
| Buttons 4–5 | Left shoulder, right shoulder |
| Buttons 6–7 | View, Menu |
| Buttons 8–10 | Left stick, right stick, Guide |
| Buttons 11–14 | D-pad Up, Down, Left, Right |
| POV 0 | D-pad direction; opposing directions cancel |

`c_dfDIJoystick`, `c_dfDIJoystick2` and custom absolute-position formats use the same
semantic compiler. Object descriptions are copied; caller-owned format memory is not
retained. The limits are 256 object descriptions and 4096 state bytes. Required objects
must exist; absent optional fields are neutral. Unused POVs contain `0xffffffff`.
Relative axes and velocity, acceleration or force aspects are unsupported.

Custom state packets must have a size divisible by four, including button-only formats.
Button offsets may be byte-aligned; axes and POVs require DWORD alignment.
`EnumObjects` and `GetObjectInfo` report native object offsets. A `DIPH_BYOFFSET`
query selects an object by its offset in the current data format.

Capabilities and device and object information accept the DirectInput 8 structures and
their documented `*_DX3` sizes. ANSI device names use the process code page and end at
a complete character when shortened. A retired connection clears `DIDC_ATTACHED` on its
original device object.

Range defaults to 0–65535, deadzone to 0 and saturation to 10000. Stick deadzones are
centered; trigger deadzones start at the idle endpoint. Properties can address axes by
ID, usage or an offset in the selected format; device-level setters apply to all axes.
Input properties and the data format can be changed only while unacquired. These
settings affect this DI object and do not modify shared samples or XInput translation.

## Acquisition and state

Set a data format and a cooperative level before `Acquire`. Supply a valid top-level
window belonging to the process and keep it alive while acquired. Both cooperative
modes accept `DISCL_FOREGROUND` or `DISCL_BACKGROUND`. Exclusive mode requires a
qualified native match.

Foreground acquisition requires fresh evidence for the associated window. Losing that
window, including a switch to another window of the same process, or losing fresh
evidence makes the acquisition lost. Foreground transition notifications preserve
observed away-and-back changes between probes. Observation is asynchronous; no
instantaneous Windows event-delivery guarantee is implied. Restoring focus requires an
explicit `Acquire`. Background input remains eligible across focus changes.

Repeated `Acquire` on an eligible acquired device returns `S_FALSE`. `Unacquire` is
idempotent. `GetDeviceState` requires the exact selected format size and copies a
coherent cached sample; it performs no native input queries, history traversal or output
work. Before the first sample it returns `E_PENDING`. An explicitly unacquired object
returns `DIERR_NOTACQUIRED`; lost acquisition or a retired connection returns
`DIERR_INPUTLOST`. Failed state reads leave the caller's buffer unchanged.

The profile does not require polling. `Poll` checks acquisition eligibility and returns
`DI_NOEFFECT`.

## Exclusive ownership

Exclusive acquisition uses a private Windows DirectInput device for the exact matched
controller. `Acquire` rechecks the full PnP interface path and one-to-one correspondence;
missing or incomplete evidence returns an error. An attached controller whose path
cannot be inspected can make the match ambiguous. VID/PID and names are insufficient.
Windows receives the cooperative level and acquisition request. Its failures are
reported to the caller. State and buffered events still come from the shared InputNext
cache and history. This mode does not exclude XInput, GameInput or all nonexclusive
DirectInput access.

For background exclusive mode, the native ownership worker checks that the window
and system acquisition remain valid. It discards the native state it reads for this
check. Checks start at least 25 ms apart and a successful check qualifies ownership for
at most 250 ms from its start. Loss or expiry stops state and buffered input delivery.
If the selected native data format requires polling, the worker calls native
`Poll` before reading its state. Time spent polling counts toward proof age.
An initial `E_PENDING` permits retries within 250 ms of that first pending
checkpoint's start. Further pending results keep the same deadline. Expiry returns
`E_PENDING` from `Acquire` and establishes no input authority.
If Windows reports `DIERR_NOTACQUIRED`, call `Unacquire` before trying `Acquire` again.
Other ownership losses return `DIERR_INPUTLOST` after acquisition.

`Unacquire` first closes local input delivery. For an active exclusive acquisition it
normally waits for native release and returns its result. Cancellation of an unfinished
`Acquire` returns promptly; a late native success is released without enabling input.
Cleanup still occupies its resource slot. A new exclusive attempt conflicting with
pending cleanup returns `HRESULT_FROM_WIN32(ERROR_BUSY)`.

Control waits dispatch sent window messages without consuming queued application input.
New exclusive attempts from sent-message callbacks return busy; an already pending
`Acquire` returns `DIERR_ACQUIRED`. If a sent-message callback cannot safely
wait for native cleanup, `Unacquire` returns `E_PENDING`; call it again outside that
callback to observe completion. These rules cover InputNext input controls. Forwarded
system COM operations and native provider destruction retain their existing behavior.

The session permits 16 resident native ownership attempts, including closing attempts,
independently of buffered history lanes. Exhaustion returns `DIERR_OUTOFMEMORY`.
Unresponsive native calls can delay release and shutdown. If shutdown waits five
seconds for a native ownership worker, it reports the thread ID in Windows debug
output and continues waiting for the worker to finish.

## Buffered input and notifications

Set `DIPROP_BUFFERSIZE` before acquisition. The default size is zero, which disables
buffering. Reading the property returns the requested size. For a positive request,
the queue holds one fewer event than the lesser of the request and 4096.

`GetDeviceData` reads projected changes at offsets from the selected format. It accepts
DirectInput 8 and DX3 event sizes. Use `DIGDD_PEEK` to leave events in the queue. A null
destination with count `INFINITE` counts events with peek and flushes them without it.
With a finite count, a null destination discards up to that many events unless peeking.
The caller must provide enough array entries for a nonnull destination; the API cannot
infer its allocation size.
Each acquired device object consumes its own queue. A shared counter orders publication
across InputNext DirectInput devices, and events from one source reading share a number.
Independent workers and forwarded Windows devices have no common source-time ordering.

An overflow preserves the events already queued. `DI_BUFFEROVERFLOW` also reports
uncertain source history, including ambiguous readings at the acquisition boundary or
gaps in coverage. The buffer resumes from a qualified state without inventing
transitions across the gap. Peek and zero-count queries preserve pending loss. A
consuming read or flush acknowledges resolved loss when it reports overflow.

Use `SetEventNotification` while unacquired to select an event or disable notification
with a null handle. InputNext owns a duplicate and signals projected changes, loss of
buffered history, or loss of acquisition. A zero buffer size can still use notifications.
The handle must identify a Windows event. Registration preserves its current state;
the application controls reset behavior for manual-reset events.

Buffered acquisition establishes a history boundary on a worker for the exact device
connection. Acquire waits for that boundary and can be canceled by Unacquire.
Native delays can extend acquisition and final session drainage. Each worker uses
the shared GameInput history store. Ready calls read local events or cached state.
Objects for the same connection share a worker through reader handoffs. Session capacity
is 16 resident workers and 64 resident subscriptions, including resources awaiting cleanup.
The capped queues keep their combined event payload below 8 MiB.

Games that require action mapping or force feedback may be
unable to use the normalized device. Force-feedback capabilities are absent and effect
enumeration is empty. XInput rumble and the Effects SDK remain available through their
own contracts. Sony Bluetooth remains input-only.
