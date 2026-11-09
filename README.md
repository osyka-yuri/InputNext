# InputNext

[![CI](https://github.com/osyka-yuri/InputNext/actions/workflows/ci.yml/badge.svg?branch=main&event=push)](https://github.com/osyka-yuri/InputNext/actions/workflows/ci.yml)
[![Release](https://github.com/osyka-yuri/InputNext/actions/workflows/release.yml/badge.svg?event=push)](https://github.com/osyka-yuri/InputNext/actions/workflows/release.yml)
[![Latest Release](https://img.shields.io/github/v/release/osyka-yuri/InputNext?display_name=tag)](https://github.com/osyka-yuri/InputNext/releases/latest)
[![Attestations](https://img.shields.io/badge/attestations-Sigstore-blue)](https://github.com/osyka-yuri/InputNext/attestations)

InputNext brings Microsoft GameInput controller support to Windows games that use XInput. It provides replacement XInput DLLs for x86 and x64 games, plus C SDKs for input, history and feedback.

DualSense and DualSense Edge support USB rumble, adaptive triggers, LEDs and HD haptics. Game rumble works through XInput. Per-game profiles apply persistent trigger effects; game-driven trigger, LED and HD effects require an Effects SDK integration. Bluetooth support for these controllers is input-only.

The project is under development. Follow the [game installation guide](docs/installation.md) to get started.

## Documentation

| Topic | Guides |
|---|---|
| Installation | [Install in a game](docs/installation.md) |
| Configuration and feedback | [Per-game profiles](docs/profiles.md), [GameInput output](docs/gameinput-output.md), [DualSense USB](docs/dualsense-usb.md), [HD haptics](docs/hd-haptics.md) |
| C SDKs | [Core input SDK](docs/core-sdk.md), [Effects SDK](docs/effects-sdk.md) |
| Development | [Builds and tests](docs/development.md), [distribution packages](docs/distribution.md), [continuous integration](docs/ci.md) |
| Input components | [Publication and ownership](docs/input.md), [GameInput](docs/gameinput.md), [history](docs/history.md), [focus](docs/focus.md) |
| XInput components | [Presentation](docs/xinput.md), [keystrokes and Guide waits](docs/xinput-events.md) |
| Output components | [Authority](docs/output.md), [native execution](docs/output-execution.md), [body leases](docs/body-output.md), [PCM playback](docs/body-pcm.md) |
| Performance | [GetState measurements](docs/performance.md) |

Licensed under the [MIT License](LICENSE). Third-party dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
