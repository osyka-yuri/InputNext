# InputNext

InputNext is a Windows controller runtime for x86 and x64 games. It exposes GameInput controllers through XInput DLLs and provides C SDKs for input, history and feedback. The project is under development.

DualSense and DualSense Edge support USB rumble, adaptive triggers, LEDs and HD haptics. Bluetooth support for these controllers is input-only.

## Documentation

| Topic | Guides |
|---|---|
| Installation | [XInput DLL deployment](docs/xinput-dll.md), [distribution packages](docs/distribution.md) |
| Configuration and feedback | [Per-game profiles](docs/profiles.md), [GameInput output](docs/gameinput-output.md), [DualSense USB](docs/dualsense-usb.md), [HD haptics](docs/hd-haptics.md) |
| C SDKs | [Core input SDK](docs/core-sdk.md), [Effects SDK](docs/effects-sdk.md) |
| Development | [Builds and tests](docs/development.md), [continuous integration](docs/ci.md) |
| Input components | [Publication and ownership](docs/input.md), [GameInput](docs/gameinput.md), [history](docs/history.md), [focus](docs/focus.md) |
| XInput components | [Presentation](docs/xinput.md), [keystrokes and Guide waits](docs/xinput-events.md) |
| Output components | [Authority](docs/output.md), [native execution](docs/output-execution.md), [body leases](docs/body-output.md), [PCM playback](docs/body-pcm.md) |

Licensed under the [MIT License](LICENSE). Third-party dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
