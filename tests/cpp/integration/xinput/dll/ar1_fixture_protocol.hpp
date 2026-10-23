#pragma once

#include <Windows.h>

#include <cstdint>

namespace inputnext::xinput_dll_fixture {

struct ar1_fixture_snapshot final {
    std::uint32_t pin_attempts{};
    std::uint32_t owner_factory_calls{};
    std::uint32_t stable_api_calls{};
    std::uint32_t dependency_constructions{};
    std::uint32_t graph_constructions{};
    std::uint32_t native_starts{};
    std::uint32_t operator_new_calls{};
};

using ar1_snapshot_fn = BOOL(__cdecl*)(ar1_fixture_snapshot* result) noexcept;
using ar1_reset_fn = void(__cdecl*)() noexcept;

} // namespace inputnext::xinput_dll_fixture
