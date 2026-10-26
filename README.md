# InputNext

InputNext is a Windows controller runtime under development. Its native input components provide coherent gamepad samples and exact connection lifetimes.

See [native input components](docs/input.md) for publication, ownership, and shutdown contracts.

The [GameInput runtime](docs/gameinput.md) provides native controller discovery, latest state, [independent history readers](docs/history.md), metadata queries, [native feedback](docs/gameinput-output.md), and supervised diagnostics.

The C++ [XInput presentation](docs/xinput.md) maps connections to four positions with packet tracking, [focus observation](docs/focus.md), logical Enable, metadata queries, and [keystrokes and Guide waits](docs/xinput-events.md). Native x86 and x64 [XInput DLL bundles](docs/xinput-dll.md) expose three flavors through one shared runtime per process.

The C++ [output authority](docs/output.md) owns effect contexts, arbitration, expiry, and acknowledged closure. Its [native executor](docs/output-execution.md) schedules qualified attempts and manages retry and sink lifetime. [Body PCM](docs/body-pcm.md) provides owned samples and offline playback with explicit render admission.

[Per-game profiles](docs/profiles.md) configure persistent adaptive-trigger effects from `inputnext.toml` beside the game executable.

[HD haptics](docs/hd-haptics.md) connects qualified USB DualSense audio endpoints to body PCM playback.

The [Core C SDK](docs/core-sdk.md) provides device discovery, exact targets, latest input and independent history readers for x86 and x64 callers.

See [development setup](docs/development.md) for toolchain requirements and verification commands.

Licensed under the [MIT License](LICENSE). Third-party dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
