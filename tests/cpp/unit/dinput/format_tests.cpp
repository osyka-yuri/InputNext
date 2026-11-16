#include "dinput/format.hpp"
#include "dinput/profile.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <dinput.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace inputnext::dinput;

namespace {

constexpr DWORD optional_object_flag = 0x80000000U;
constexpr GUID unknown_guid{.Data1 = 0xDEADBEEFU,
    .Data2 = 0x1234U,
    .Data3 = 0x5678U,
    .Data4 = {0x90U, 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U, 0x45U, 0x67U}};

[[nodiscard]] DIDATAFORMAT data_format(DWORD data_size,
    DWORD object_count,
    DIOBJECTDATAFORMAT* object_data,
    DWORD flags = DIDF_ABSAXIS) noexcept {
    return DIDATAFORMAT{.dwSize = static_cast<DWORD>(sizeof(DIDATAFORMAT)),
        .dwObjSize = static_cast<DWORD>(sizeof(DIOBJECTDATAFORMAT)),
        .dwFlags = flags,
        .dwDataSize = data_size,
        .dwNumObjs = object_count,
        .rgodf = object_data};
}

template <typename Value>
[[nodiscard]] Value read_value(const std::array<std::byte, sizeof(DIJOYSTATE)>& bytes,
    std::size_t offset) {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

template <typename Value>
[[nodiscard]] Value read_value(const std::vector<std::byte>& bytes, std::size_t offset) {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

} // namespace

TEST_CASE("dinput.format.accepts_sdk_joystick_and_extended_joystick_formats", "[unit][dinput]") {
    compiled_format compiled{};
    REQUIRE(compile_format(c_dfDIJoystick, compiled) == DI_OK);
    CHECK(compiled.data_size == sizeof(DIJOYSTATE));

    profile_values values{};
    values.axes = {10, 20, 30, 40, 50, 60};
    values.buttons.fill(0x80U);
    values.pov = 18000U;
    std::vector<std::byte> joystick_state(c_dfDIJoystick.dwDataSize);
    REQUIRE(write_state(compiled, values, joystick_state) == DI_OK);
    CHECK(read_value<LONG>(joystick_state, DIJOFS_X) == 10);
    CHECK(read_value<DWORD>(joystick_state, DIJOFS_POV(0)) == 18000U);
    CHECK(read_value<BYTE>(joystick_state, DIJOFS_BUTTON(14)) == 0x80U);

    REQUIRE(compile_format(c_dfDIJoystick2, compiled) == DI_OK);
    CHECK(compiled.data_size == sizeof(DIJOYSTATE2));
    std::vector<std::byte> joystick2_state(c_dfDIJoystick2.dwDataSize, std::byte{0x7fU});
    REQUIRE(write_state(compiled, values, joystick2_state) == DI_OK);
    CHECK(read_value<LONG>(joystick2_state, offsetof(DIJOYSTATE2, lX)) == 10);
    CHECK(read_value<LONG>(joystick2_state, offsetof(DIJOYSTATE2, lVX)) == 0);
    CHECK(read_value<LONG>(joystick2_state, offsetof(DIJOYSTATE2, lAX)) == 0);
    CHECK(read_value<LONG>(joystick2_state, offsetof(DIJOYSTATE2, lFX)) == 0);
    CHECK(read_value<DWORD>(joystick2_state, DIJOFS_POV(1)) == 0xffffffffU);
    CHECK(read_value<BYTE>(joystick2_state, offsetof(DIJOYSTATE2, rgbButtons) + 127U) == 0U);
}

TEST_CASE("dinput.format.copies_custom_descriptors_and_resolves_object_selectors",
    "[unit][dinput]") {
    const auto profile = objects();
    std::array<DIOBJECTDATAFORMAT, 3> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &profile[0].guid,
            .dwOfs = 0U,
            .dwType = static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U)),
            .dwFlags = DIDOI_ASPECTPOSITION},
        DIOBJECTDATAFORMAT{.pguid = &profile[6].guid,
            .dwOfs = 4U,
            .dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_ANYINSTANCE),
            .dwFlags = 0U},
        DIOBJECTDATAFORMAT{.pguid = &profile[21].guid,
            .dwOfs = 8U,
            .dwType = static_cast<DWORD>(DIDFT_POV | DIDFT_ANYINSTANCE),
            .dwFlags = 0U},
    };
    auto format = data_format(12U, static_cast<DWORD>(descriptors.size()), descriptors.data());
    compiled_format compiled{};
    REQUIRE(compile_format(format, compiled) == DI_OK);
    descriptors[0].dwOfs = 12U;
    format.rgodf = nullptr;

    CHECK(find_object(profile[0].id, DIPH_BYID, nullptr) == 0U);
    CHECK(find_object(profile[0].id | DIDFT_FFACTUATOR, DIPH_BYID, nullptr) == 0U);
    CHECK(find_object(profile[1].id | DIDFT_FFACTUATOR, DIPH_BYID, nullptr) == 1U);
    CHECK_FALSE(find_object(profile[2].id | DIDFT_FFACTUATOR, DIPH_BYID, nullptr).has_value());
    CHECK_FALSE(find_object(profile[6].id | DIDFT_FFACTUATOR, DIPH_BYID, nullptr).has_value());
    CHECK(find_object(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0U), DIPH_BYID, nullptr) == 6U);
    CHECK(find_object(4U, DIPH_BYOFFSET, &compiled) == 6U);
    CHECK(find_object(DIMAKEUSAGEDWORD(0x09U, 1U), DIPH_BYUSAGE, nullptr) == 6U);
    CHECK_FALSE(find_object(12U, DIPH_BYOFFSET, &compiled).has_value());
    CHECK_FALSE(find_object(0U, DIPH_DEVICE, &compiled).has_value());

    profile_values values{};
    values.axes[0] = -123;
    values.buttons[0] = 0x80U;
    values.pov = 27000U;
    std::array<std::byte, 12> state{};
    REQUIRE(write_state(compiled, values, state) == DI_OK);
    LONG axis{};
    std::memcpy(&axis, state.data(), sizeof(axis));
    CHECK(axis == -123);
    CHECK(state[4] == std::byte{0x80U});
    DWORD pov{};
    std::memcpy(&pov, state.data() + 8U, sizeof(pov));
    CHECK(pov == 27000U);

    std::array<std::byte, 11> short_state{};
    short_state.fill(std::byte{0x77U});
    CHECK(write_state(compiled, values, short_state) == DIERR_INVALIDPARAM);
    CHECK(std::ranges::all_of(short_state,
        [](std::byte value) { return value == std::byte{0x77U}; }));
}

TEST_CASE("dinput.format.wildcard_button_instances_are_distinct", "[unit][dinput]") {
    const auto profile = objects();
    std::array<DIOBJECTDATAFORMAT, 2> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &profile[6].guid,
            .dwOfs = 0U,
            .dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_ANYINSTANCE),
            .dwFlags = 0U},
        DIOBJECTDATAFORMAT{.pguid = &profile[6].guid,
            .dwOfs = 1U,
            .dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_ANYINSTANCE),
            .dwFlags = 0U},
    };
    const auto format = data_format(4U, static_cast<DWORD>(descriptors.size()), descriptors.data());
    compiled_format compiled{};
    REQUIRE(compile_format(format, compiled) == DI_OK);
    CHECK(compiled.bindings[0].profile_object_index == 6U);
    CHECK(compiled.bindings[1].profile_object_index == 7U);

    descriptors[0].dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_MAKEINSTANCE(0U));
    descriptors[1].dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_MAKEINSTANCE(0U));
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    CHECK(compiled.bindings[0].profile_object_index == 6U);
}

TEST_CASE("dinput.format.push_and_toggle_button_types_are_distinct", "[unit][dinput]") {
    const auto profile = objects();
    DIOBJECTDATAFORMAT descriptor{.pguid = &profile[6].guid,
        .dwOfs = 0U,
        .dwType = static_cast<DWORD>(DIDFT_TGLBUTTON | DIDFT_MAKEINSTANCE(0U)),
        .dwFlags = 0U};
    const auto format = data_format(4U, 1U, &descriptor);
    compiled_format compiled{};
    CHECK(compile_format(format, compiled) == DIERR_UNSUPPORTED);
    descriptor.dwType |= optional_object_flag;
    REQUIRE(compile_format(format, compiled) == DI_OK);
    CHECK(compiled.binding_count == 0U);
    CHECK_FALSE(
        find_object(DIDFT_TGLBUTTON | DIDFT_MAKEINSTANCE(0U), DIPH_BYID, nullptr).has_value());
    CHECK(find_object(DIDFT_BUTTON | DIDFT_MAKEINSTANCE(0U), DIPH_BYID, nullptr) == 6U);
}

TEST_CASE("dinput.format.button_only_packets_require_dword_size_but_allow_byte_offsets",
    "[unit][dinput]") {
    DIOBJECTDATAFORMAT button{.pguid = &GUID_Button,
        .dwOfs = 0U,
        .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0U),
        .dwFlags = 0U};
    compiled_format compiled{};
    for (const auto size : {1U, 2U, 3U, 5U}) {
        CAPTURE(size);
        const auto format = data_format(size, 1U, &button);
        CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    }

    button.dwOfs = 3U;
    profile_values values{};
    values.buttons[0U] = 0x80U;
    for (const auto size : {4U, 8U}) {
        CAPTURE(size);
        const auto format = data_format(size, 1U, &button);
        REQUIRE(compile_format(format, compiled) == DI_OK);
        std::vector<std::byte> state(size, std::byte{0x7fU});
        REQUIRE(write_state(compiled, values, state) == DI_OK);
        for (std::size_t offset = 0U; offset < state.size(); ++offset) {
            CHECK(state[offset] == (offset == 3U ? std::byte{0x80U} : std::byte{}));
        }
    }
}

TEST_CASE("dinput.format.optional_nonposition_aspects_may_share_position_offsets",
    "[unit][dinput]") {
    const auto profile = objects();
    std::array<DIOBJECTDATAFORMAT, 2> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &profile[0].guid,
            .dwOfs = 0U,
            .dwType = static_cast<DWORD>(DIDFT_AXIS | DIDFT_MAKEINSTANCE(0U)),
            .dwFlags = DIDOI_ASPECTPOSITION},
        DIOBJECTDATAFORMAT{.pguid = &profile[0].guid,
            .dwOfs = 0U,
            .dwType =
                static_cast<DWORD>(DIDFT_AXIS | DIDFT_MAKEINSTANCE(0U) | optional_object_flag),
            .dwFlags = DIDOI_ASPECTVELOCITY},
    };
    const auto format = data_format(4U, static_cast<DWORD>(descriptors.size()), descriptors.data());
    compiled_format compiled{};
    REQUIRE(compile_format(format, compiled) == DI_OK);

    profile_values values{};
    values.axes[0] = 1234;
    std::array<std::byte, 4> state{};
    REQUIRE(write_state(compiled, values, state) == DI_OK);
    LONG axis{};
    std::memcpy(&axis, state.data(), sizeof(axis));
    CHECK(axis == 1234);
}

TEST_CASE("dinput.format.rejects_an_optional_pov_overlapping_an_axis_in_either_order",
    "[unit][dinput]") {
    std::array<DIOBJECTDATAFORMAT, 2> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &GUID_XAxis,
            .dwOfs = 0U,
            .dwType = DIDFT_ABSAXIS,
            .dwFlags = DIDOI_ASPECTPOSITION},
        DIOBJECTDATAFORMAT{.pguid = &GUID_POV,
            .dwOfs = 0U,
            .dwType = DIDFT_POV | optional_object_flag,
            .dwFlags = DIDOI_ASPECTVELOCITY},
    };
    const auto format = data_format(4U, static_cast<DWORD>(descriptors.size()), descriptors.data());
    compiled_format compiled{};
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    std::ranges::reverse(descriptors);
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
}

TEST_CASE("dinput.format.rejects_malformed_relative_oversized_and_unknown_required_formats",
    "[unit][dinput]") {
    DIOBJECTDATAFORMAT object{.pguid = &unknown_guid,
        .dwOfs = 0U,
        .dwType = static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U)),
        .dwFlags = 0U};
    auto format = data_format(4U, 1U, &object);
    compiled_format compiled{};
    CHECK(compile_format(format, compiled) == DIERR_UNSUPPORTED);

    object.dwType |= optional_object_flag;
    REQUIRE(compile_format(format, compiled) == DI_OK);
    const profile_values values{};
    std::array<std::byte, 4> state{std::byte{0x7fU},
        std::byte{0x7fU},
        std::byte{0x7fU},
        std::byte{0x7fU}};
    REQUIRE(write_state(compiled, values, state) == DI_OK);
    const std::array<std::byte, 4> neutral_state{};
    CHECK(state == neutral_state);

    object.dwType = static_cast<DWORD>(DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(0U));
    CHECK(compile_format(format, compiled) == DIERR_UNSUPPORTED);
    object.dwType = static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U));
    object.dwOfs = 1U;
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    object.dwOfs = 0U;
    format.dwDataSize = max_format_data_size + 4U;
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    format.dwDataSize = 4U;
    format.dwFlags = DIDF_RELAXIS;
    CHECK(compile_format(format, compiled) == DIERR_UNSUPPORTED);
    format.dwFlags = DIDF_ABSAXIS;
    format.dwObjSize = 0U;
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
}

TEST_CASE("dinput.format.rejects_overlaps_and_enforces_fixed_capacities", "[unit][dinput]") {
    const auto profile = objects();
    std::array<DIOBJECTDATAFORMAT, 2> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &profile[0].guid,
            .dwOfs = 0U,
            .dwType = static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U)),
            .dwFlags = 0U},
        DIOBJECTDATAFORMAT{.pguid = &profile[6].guid,
            .dwOfs = 2U,
            .dwType = static_cast<DWORD>(DIDFT_BUTTON | DIDFT_MAKEINSTANCE(0U)),
            .dwFlags = 0U},
    };
    auto format = data_format(4U, static_cast<DWORD>(descriptors.size()), descriptors.data());
    compiled_format compiled{};
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);

    std::array<DIOBJECTDATAFORMAT, max_format_objects + 1U> too_many{};
    format = data_format(4U, static_cast<DWORD>(too_many.size()), too_many.data());
    CHECK(compile_format(format, compiled) == DIERR_OUTOFMEMORY);

    format = data_format(4U, 1U, descriptors.data());
    format.dwDataSize = 0U;
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
    format.dwDataSize = 4U;
    format.rgodf = nullptr;
    CHECK(compile_format(format, compiled) == DIERR_INVALIDPARAM);
}

TEST_CASE("dinput.format.actuator_selectors_accept_exact_and_wildcard_x_y_axes",
    "[unit][dinput][force_feedback]") {
    std::array<DIOBJECTDATAFORMAT, 2U> descriptors{
        DIOBJECTDATAFORMAT{.pguid = &objects()[0U].guid,
            .dwOfs = 0U,
            .dwType = objects()[0U].id | DIDFT_FFACTUATOR,
            .dwFlags = 0U},
        DIOBJECTDATAFORMAT{.pguid = &objects()[1U].guid,
            .dwOfs = 4U,
            .dwType = objects()[1U].id | DIDFT_FFACTUATOR,
            .dwFlags = 0U},
    };
    const auto format = data_format(8U, 2U, descriptors.data());
    compiled_format compiled{};
    REQUIRE(compile_format(format, compiled, true) == DI_OK);
    CHECK(compiled.binding_count == 2U);
    CHECK(compiled.bindings[0U].profile_object_index == 0U);
    CHECK(compiled.bindings[1U].profile_object_index == 1U);
    CHECK(compile_format(format, compiled) == DIERR_UNSUPPORTED);
    for (auto& descriptor : descriptors) {
        descriptor.pguid = nullptr;
        descriptor.dwType = DIDFT_ABSAXIS | DIDFT_ANYINSTANCE | DIDFT_FFACTUATOR;
    }
    REQUIRE(compile_format(format, compiled, true) == DI_OK);
    CHECK(compiled.bindings[0U].profile_object_index == 0U);
    CHECK(compiled.bindings[1U].profile_object_index == 1U);
    descriptors[0U].dwType = objects()[2U].id | DIDFT_FFACTUATOR;
    CHECK(compile_format(format, compiled, true) == DIERR_UNSUPPORTED);
    descriptors[0U].dwType |= optional_object_flag;
    REQUIRE(compile_format(format, compiled, true) == DI_OK);
    CHECK(compiled.binding_count == 1U);
    CHECK(compiled.bindings[0U].profile_object_index == 0U);
    descriptors[1U].dwType |= optional_object_flag;
    REQUIRE(compile_format(format, compiled) == DI_OK);
    CHECK(compiled.binding_count == 0U);
}
