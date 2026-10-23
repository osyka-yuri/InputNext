#include "runtime/abi/runtime_api.hpp"

#include "runtime/abi/xinput_argument_policy.hpp"
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

namespace {

using namespace inputnext::runtime_detail::xinput_argument_policy;

template <typename Callback>
struct unsupported_callback;

template <typename Result, typename... Arguments>
struct unsupported_callback<Result(WINAPI*)(Arguments...) noexcept> final {
    [[nodiscard]] static Result WINAPI invoke(Arguments... arguments) noexcept {
        (static_cast<void>(arguments), ...);
        return static_cast<Result>(ERROR_NOT_SUPPORTED);
    }
};

template <typename... Arguments>
struct unsupported_callback<void(WINAPI*)(Arguments...) noexcept> final {
    static void WINAPI invoke(Arguments... arguments) noexcept {
        (static_cast<void>(arguments), ...);
    }
};

constexpr input_next_runtime_api_v1 complete_test_api{.struct_size =
                                                          sizeof(input_next_runtime_api_v1),
    .abi_version = 1U,
    .get_state = &unsupported_callback<input_next_runtime_get_state_fn>::invoke,
    .set_state = &unsupported_callback<input_next_runtime_set_state_fn>::invoke,
    .get_capabilities = &unsupported_callback<input_next_runtime_get_capabilities_fn>::invoke,
    .enable = &unsupported_callback<input_next_runtime_enable_fn>::invoke,
    .get_battery_information =
        &unsupported_callback<input_next_runtime_get_battery_information_fn>::invoke,
    .get_keystroke = &unsupported_callback<input_next_runtime_get_keystroke_fn>::invoke,
    .get_audio_device_ids =
        &unsupported_callback<input_next_runtime_get_audio_device_ids_fn>::invoke,
    .get_dsound_audio_device_guids =
        &unsupported_callback<input_next_runtime_get_dsound_audio_device_guids_fn>::invoke,
    .get_state_ex = &unsupported_callback<input_next_runtime_get_state_ex_fn>::invoke,
    .get_capabilities_ex = &unsupported_callback<input_next_runtime_get_capabilities_ex_fn>::invoke,
    .wait_for_guide_button =
        &unsupported_callback<input_next_runtime_wait_for_guide_button_fn>::invoke,
    .cancel_guide_button_wait =
        &unsupported_callback<input_next_runtime_cancel_guide_button_wait_fn>::invoke,
    .power_off_controller =
        &unsupported_callback<input_next_runtime_power_off_controller_fn>::invoke};

} // namespace

static_assert(
    std::is_same_v<decltype(&input_next_runtime_query_api), input_next_runtime_query_api_fn>);

TEST_CASE("runtime.abi.v1_validation_requires_a_complete_immutable_table", "[unit][runtime][abi]") {
    const auto* const api = &complete_test_api;
    REQUIRE(api != nullptr);
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(nullptr));
    CHECK(api->struct_size == sizeof(input_next_runtime_api_v1));
    CHECK(api->abi_version == 1U);
    CHECK(is_valid_input_next_runtime_api_v1(api));

    auto invalid = *api;
    invalid.struct_size = static_cast<std::uint32_t>(sizeof(input_next_runtime_api_v1) - 1U);
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.abi_version = 2U;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));

    invalid = *api;
    invalid.get_state = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.set_state = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_capabilities = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.enable = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_battery_information = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_keystroke = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_audio_device_ids = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_dsound_audio_device_guids = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_state_ex = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.get_capabilities_ex = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.wait_for_guide_button = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.cancel_guide_button_wait = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
    invalid = *api;
    invalid.power_off_controller = nullptr;
    CHECK_FALSE(is_valid_input_next_runtime_api_v1(&invalid));
}

TEST_CASE("runtime.abi.flavors_keep_the_frozen_32_bit_values", "[unit][runtime][abi]") {
    CHECK(static_cast<std::uint32_t>(INPUT_NEXT_FLAVOR_1_3) == 0U);
    CHECK(static_cast<std::uint32_t>(INPUT_NEXT_FLAVOR_1_4) == 1U);
    CHECK(static_cast<std::uint32_t>(INPUT_NEXT_FLAVOR_9_1_0) == 2U);
    CHECK(sizeof(input_next_xinput_flavor) == sizeof(std::uint32_t));
}

TEST_CASE("runtime.abi.argument_policy_preserves_exact_precedence_and_permissive_fields",
    "[unit][runtime][abi]") {
    const XINPUT_STATE state{};
    const XINPUT_VIBRATION vibration{};
    XINPUT_CAPABILITIES capabilities{};
    inputnext::xinput::capabilities_ex capabilities_ex{};
    XINPUT_BATTERY_INFORMATION battery{};
    XINPUT_KEYSTROKE keystroke{};

    CHECK(validate_get_state_arguments(0U, &state) == ERROR_SUCCESS);
    CHECK(validate_get_state_arguments(4U, &state) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_get_state_arguments(0U, nullptr) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_set_state_arguments(0U, &vibration) == ERROR_SUCCESS);
    CHECK(validate_set_state_arguments(4U, &vibration) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_set_state_arguments(0U, nullptr) == ERROR_BAD_ARGUMENTS);

    capabilities_arguments capabilities_request{.user_index = 0U,
        .flags = 0U,
        .capabilities = &capabilities};
    CHECK(validate_capabilities_arguments(capabilities_request) == ERROR_SUCCESS);
    capabilities_request.flags = XINPUT_FLAG_GAMEPAD;
    CHECK(validate_capabilities_arguments(capabilities_request) == ERROR_SUCCESS);
    capabilities_request.flags = 2U;
    CHECK(validate_capabilities_arguments(capabilities_request) == ERROR_BAD_ARGUMENTS);
    capabilities_request.user_index = 4U;
    capabilities_request.capabilities = nullptr;
    CHECK(validate_capabilities_arguments(capabilities_request) == ERROR_BAD_ARGUMENTS);

    capabilities_ex_arguments capabilities_ex_request{.user_index = 0U,
        .flags = 0U,
        .capabilities = &capabilities_ex};
    CHECK(validate_capabilities_ex_arguments(capabilities_ex_request) == ERROR_SUCCESS);
    capabilities_ex_request.flags = XINPUT_FLAG_GAMEPAD;
    CHECK(validate_capabilities_ex_arguments(capabilities_ex_request) == ERROR_SUCCESS);
    capabilities_ex_request.flags = 2U;
    CHECK(validate_capabilities_ex_arguments(capabilities_ex_request) == ERROR_BAD_ARGUMENTS);
    capabilities_ex_request.user_index = 4U;
    capabilities_ex_request.capabilities = nullptr;
    CHECK(validate_capabilities_ex_arguments(capabilities_ex_request) == ERROR_BAD_ARGUMENTS);

    battery_arguments battery_request{.user_index = 0U,
        .device_type = BATTERY_DEVTYPE_GAMEPAD,
        .battery = &battery};
    CHECK(validate_battery_arguments(battery_request) == ERROR_SUCCESS);
    battery_request.device_type = BATTERY_DEVTYPE_HEADSET;
    CHECK(validate_battery_arguments(battery_request) == ERROR_SUCCESS);
    battery_request.user_index = 4U;
    battery_request.device_type = 0xFFU;
    battery_request.battery = nullptr;
    CHECK(validate_battery_arguments(battery_request) == ERROR_BAD_ARGUMENTS);
    battery_request.user_index = 0U;
    battery_request.battery = &battery;
    CHECK(validate_battery_arguments(battery_request) == ERROR_BAD_ARGUMENTS);
    battery_request.device_type = BATTERY_DEVTYPE_GAMEPAD;
    battery_request.battery = nullptr;
    CHECK(validate_battery_arguments(battery_request) == ERROR_BAD_ARGUMENTS);

    keystroke_arguments keystroke_request{.flavor = INPUT_NEXT_FLAVOR_1_3,
        .user_index = 0U,
        .keystroke = &keystroke};
    CHECK(validate_keystroke_arguments(keystroke_request) == ERROR_SUCCESS);
    keystroke_request.flavor = INPUT_NEXT_FLAVOR_1_4;
    keystroke_request.user_index = 0xFFU;
    CHECK(validate_keystroke_arguments(keystroke_request) == ERROR_SUCCESS);
    keystroke_request.flavor = INPUT_NEXT_FLAVOR_1_3;
    keystroke_request.keystroke = nullptr;
    CHECK(validate_keystroke_arguments(keystroke_request) == ERROR_BAD_ARGUMENTS);
    keystroke_request.user_index = 4U;
    keystroke_request.keystroke = &keystroke;
    CHECK(validate_keystroke_arguments(keystroke_request) == ERROR_BAD_ARGUMENTS);
    keystroke_request.flavor = INPUT_NEXT_FLAVOR_9_1_0;
    keystroke_request.user_index = 0xFFU;
    keystroke_request.keystroke = nullptr;
    CHECK(validate_keystroke_arguments(keystroke_request) == ERROR_NOT_SUPPORTED);

    audio_device_ids_arguments audio_ids_request{.flavor = INPUT_NEXT_FLAVOR_1_4, .user_index = 0U};
    CHECK(validate_audio_device_ids_arguments(audio_ids_request) == ERROR_SUCCESS);
    audio_ids_request.user_index = 4U;
    CHECK(validate_audio_device_ids_arguments(audio_ids_request) == ERROR_BAD_ARGUMENTS);
    audio_ids_request.flavor = INPUT_NEXT_FLAVOR_1_3;
    CHECK(validate_audio_device_ids_arguments(audio_ids_request) == ERROR_NOT_SUPPORTED);

    dsound_audio_device_guids_arguments dsound_request{.flavor = INPUT_NEXT_FLAVOR_1_3,
        .user_index = 0U};
    CHECK(validate_dsound_audio_device_guids_arguments(dsound_request) == ERROR_SUCCESS);
    dsound_request.flavor = INPUT_NEXT_FLAVOR_9_1_0;
    dsound_request.user_index = 3U;
    CHECK(validate_dsound_audio_device_guids_arguments(dsound_request) == ERROR_SUCCESS);
    dsound_request.flavor = INPUT_NEXT_FLAVOR_1_3;
    dsound_request.user_index = 4U;
    CHECK(validate_dsound_audio_device_guids_arguments(dsound_request) == ERROR_BAD_ARGUMENTS);
    dsound_request.flavor = INPUT_NEXT_FLAVOR_1_4;
    CHECK(validate_dsound_audio_device_guids_arguments(dsound_request) == ERROR_NOT_SUPPORTED);

    CHECK(validate_guide_wait_arguments(0U) == ERROR_SUCCESS);
    CHECK(validate_guide_wait_arguments(4U) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_cancel_guide_wait_arguments(4U) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_power_off_arguments(4U) == ERROR_BAD_ARGUMENTS);
    CHECK(validate_enable_arguments(FALSE) == ERROR_SUCCESS);
    CHECK(validate_enable_arguments(TRUE) == ERROR_SUCCESS);
    CHECK(validate_enable_arguments(12) == ERROR_SUCCESS);
}
