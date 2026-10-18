# Third-party notices

## Catch2

- Version: 3.16.1
- Source: https://github.com/catchorg/Catch2/tree/v3.16.1
- License: Boost Software License 1.0
- Use: build-time test dependency; not linked into runtime components.

The upstream archive contains the complete license in `LICENSE.txt`. The archive URL and SHA-256 are pinned in `tests/CMakeLists.txt`.

## Microsoft GameInput

- Version: 4.0.300
- Source: https://www.nuget.org/packages/Microsoft.GameInput/4.0.300
- Use: native API headers and the architecture-matched app-local runtime and Raw Input proxy.

The headers use the [MIT License](licenses/third-party/MIT-GameInput.txt). The runtime binaries retain the [Microsoft license terms](licenses/third-party/Microsoft-GameInput.txt) and [third-party notices](licenses/third-party/NOTICE-GameInput.txt) supplied with the package. The package URL and SHA-256 are pinned in `cmake/GameInput.cmake`.

