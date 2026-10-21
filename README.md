# InputNext

InputNext is a Windows controller runtime under development. Its native input components provide coherent gamepad samples and exact connection lifetimes.

See [native input components](docs/input.md) for publication, ownership, and shutdown contracts.

The [GameInput runtime](docs/gameinput.md) provides native controller discovery, latest state, [independent history readers](docs/history.md), metadata queries, and a supervised diagnostic.

The C++ [XInput presentation](docs/xinput.md) maps connections to four positions with packet tracking, [focus observation](docs/focus.md), logical Enable, metadata queries, and [keystrokes and Guide waits](docs/xinput-events.md).

The C++ [logical output authority](docs/output.md) owns effect contexts, desired-state arbitration, expiry, and acknowledged closure.

See [development setup](docs/development.md) for toolchain requirements and verification commands.

Licensed under the [MIT License](LICENSE). Third-party dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
