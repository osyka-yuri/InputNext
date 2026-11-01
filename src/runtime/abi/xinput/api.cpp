#include "api.hpp"

#include "argument_policy.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/bootstrap/bootstrap_owner.hpp"
#include "runtime/bootstrap/query_front.hpp"
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace inputnext::runtime_detail {
namespace {

[[nodiscard]] std::optional<xinput::flavor> translate_flavor(
    const input_next_xinput_flavor flavor) noexcept {
    switch (flavor) {
    case INPUT_NEXT_FLAVOR_1_3:
        return xinput::flavor::v1_3;
    case INPUT_NEXT_FLAVOR_1_4:
        return xinput::flavor::v1_4;
    case INPUT_NEXT_FLAVOR_9_1_0:
        return xinput::flavor::v9_1_0;
    default:
        return std::nullopt;
    }
}

struct state_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    XINPUT_STATE* state;
    bool include_guide;
};

struct set_state_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    XINPUT_VIBRATION* vibration;
};

struct capabilities_call final {
    input_next_xinput_flavor flavor;
    xinput_argument_policy::capabilities_arguments arguments;
};

struct enable_call final {
    input_next_xinput_flavor flavor;
    BOOL enabled;
};

struct battery_call final {
    input_next_xinput_flavor flavor;
    xinput_argument_policy::battery_arguments arguments;
};

struct keystroke_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    DWORD reserved;
    PXINPUT_KEYSTROKE keystroke;
};

struct audio_device_ids_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    LPWSTR render_device_id;
    UINT* render_count;
    LPWSTR capture_device_id;
    UINT* capture_count;
};

struct dsound_audio_device_guids_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    GUID* render_guid;
    GUID* capture_guid;
};

struct capabilities_ex_call final {
    input_next_xinput_flavor flavor;
    xinput_argument_policy::capabilities_ex_arguments arguments;
};

struct guide_wait_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    DWORD flags;
    LPVOID context;
};

struct indexed_call final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
};

template <typename Arguments, typename Validator, typename Operation>
[[nodiscard]] DWORD
dispatch_status(const Arguments& arguments, Validator&& validate, Operation&& operation) noexcept {
    const auto flavor = translate_flavor(arguments.flavor);
    if (!flavor) {
        return ERROR_BAD_ARGUMENTS;
    }
    const auto argument_status = std::invoke(std::forward<Validator>(validate), arguments);
    if (argument_status != ERROR_SUCCESS) {
        return argument_status;
    }

    try {
        auto* const owner = runtime_bootstrap_owner();
        auto* const service = owner != nullptr ? owner->bootstrap() : nullptr;
        if (service == nullptr) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        return std::invoke(std::forward<Operation>(operation), *service, *flavor, arguments);
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

[[nodiscard]] DWORD enable_status(const enable_call& arguments) noexcept {
    if (!translate_flavor(arguments.flavor)) {
        return ERROR_BAD_ARGUMENTS;
    }
    const auto argument_status =
        xinput_argument_policy::validate_enable_arguments(arguments.enabled);
    if (argument_status != ERROR_SUCCESS) {
        return argument_status;
    }

    try {
        if (auto* const owner = runtime_bootstrap_owner(); owner != nullptr) {
            owner->accept_enable(arguments.enabled);
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

[[nodiscard]] DWORD guide_wait_status(xinput::service& service,
    [[maybe_unused]] const xinput::flavor flavor,
    const DWORD user_index) {
    const auto status = service.connection_status(user_index);
    return status == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : status;
}

DWORD WINAPI api_get_state(const input_next_xinput_flavor version,
    const DWORD user_index,
    XINPUT_STATE* const state) noexcept {
    const state_call arguments{.flavor = version,
        .user_index = user_index,
        .state = state,
        .include_guide = false};
    return dispatch_status(
        arguments,
        [](const state_call& call) {
            return xinput_argument_policy::validate_get_state_arguments(call.user_index,
                call.state);
        },
        [](xinput::service& service, const xinput::flavor flavor, const state_call& call) {
            return service.get_state(flavor, call.user_index, call.state, call.include_guide);
        });
}

DWORD WINAPI api_set_state(const input_next_xinput_flavor version,
    const DWORD user_index,
    XINPUT_VIBRATION* const vibration) noexcept {
    const set_state_call arguments{.flavor = version,
        .user_index = user_index,
        .vibration = vibration};
    return dispatch_status(
        arguments,
        [](const set_state_call& call) {
            return xinput_argument_policy::validate_set_state_arguments(call.user_index,
                call.vibration);
        },
        [](xinput::service& service, const xinput::flavor flavor, const set_state_call& call) {
            return service.set_state(flavor, call.user_index, call.vibration);
        });
}

DWORD WINAPI api_get_capabilities(const input_next_xinput_flavor version,
    const DWORD user_index,
    const DWORD flags,
    XINPUT_CAPABILITIES* const capabilities) noexcept {
    const capabilities_call arguments{.flavor = version,
        .arguments = {.user_index = user_index, .flags = flags, .capabilities = capabilities}};
    return dispatch_status(
        arguments,
        [](const capabilities_call& call) {
            return xinput_argument_policy::validate_capabilities_arguments(call.arguments);
        },
        [](xinput::service& service, const xinput::flavor flavor, const capabilities_call& call) {
            return service.get_capabilities(flavor,
                call.arguments.user_index,
                call.arguments.flags,
                call.arguments.capabilities);
        });
}

void WINAPI api_enable(const input_next_xinput_flavor version, const BOOL enabled) noexcept {
    static_cast<void>(enable_status(enable_call{.flavor = version, .enabled = enabled}));
}

DWORD WINAPI api_get_battery_information(const input_next_xinput_flavor version,
    const DWORD user_index,
    const BYTE device_type,
    XINPUT_BATTERY_INFORMATION* const battery) noexcept {
    const battery_call arguments{.flavor = version,
        .arguments = {.user_index = user_index, .device_type = device_type, .battery = battery}};
    return dispatch_status(
        arguments,
        [](const battery_call& call) {
            return xinput_argument_policy::validate_battery_arguments(call.arguments);
        },
        [](xinput::service& service, const xinput::flavor flavor, const battery_call& call) {
            return service.get_battery_information(flavor,
                call.arguments.user_index,
                call.arguments.device_type,
                call.arguments.battery);
        });
}

DWORD WINAPI api_get_keystroke(const input_next_xinput_flavor version,
    const DWORD user_index,
    const DWORD reserved,
    PXINPUT_KEYSTROKE const keystroke) noexcept {
    const keystroke_call arguments{.flavor = version,
        .user_index = user_index,
        .reserved = reserved,
        .keystroke = keystroke};
    return dispatch_status(
        arguments,
        [](const keystroke_call& call) {
            return xinput_argument_policy::validate_keystroke_arguments({.flavor = call.flavor,
                .user_index = call.user_index,
                .keystroke = call.keystroke});
        },
        [](xinput::service& service, const xinput::flavor flavor, const keystroke_call& call) {
            return service.get_keystroke(flavor, call.user_index, call.reserved, call.keystroke);
        });
}

DWORD WINAPI api_get_audio_device_ids(const input_next_xinput_flavor version,
    const DWORD user_index,
    LPWSTR const render_device_id,
    UINT* const render_count,
    LPWSTR const capture_device_id,
    UINT* const capture_count) noexcept {
    const audio_device_ids_call arguments{.flavor = version,
        .user_index = user_index,
        .render_device_id = render_device_id,
        .render_count = render_count,
        .capture_device_id = capture_device_id,
        .capture_count = capture_count};
    return dispatch_status(
        arguments,
        [](const audio_device_ids_call& call) {
            return xinput_argument_policy::validate_audio_device_ids_arguments(
                {.flavor = call.flavor, .user_index = call.user_index});
        },
        [](xinput::service& service,
            const xinput::flavor flavor,
            const audio_device_ids_call& call) {
            return service.get_audio_device_ids(flavor,
                call.user_index,
                call.render_device_id,
                call.render_count,
                call.capture_device_id,
                call.capture_count,
                nullptr);
        });
}

DWORD WINAPI api_get_dsound_audio_device_guids(const input_next_xinput_flavor version,
    const DWORD user_index,
    GUID* const render_guid,
    GUID* const capture_guid) noexcept {
    const dsound_audio_device_guids_call arguments{.flavor = version,
        .user_index = user_index,
        .render_guid = render_guid,
        .capture_guid = capture_guid};
    return dispatch_status(
        arguments,
        [](const dsound_audio_device_guids_call& call) {
            return xinput_argument_policy::validate_dsound_audio_device_guids_arguments(
                {.flavor = call.flavor, .user_index = call.user_index});
        },
        [](xinput::service& service,
            const xinput::flavor flavor,
            const dsound_audio_device_guids_call& call) {
            return service.get_dsound_audio_device_guids(flavor,
                call.user_index,
                call.render_guid,
                call.capture_guid);
        });
}

DWORD WINAPI api_get_state_ex(const input_next_xinput_flavor version,
    const DWORD user_index,
    XINPUT_STATE* const state) noexcept {
    const state_call arguments{.flavor = version,
        .user_index = user_index,
        .state = state,
        .include_guide = true};
    return dispatch_status(
        arguments,
        [](const state_call& call) {
            return xinput_argument_policy::validate_get_state_arguments(call.user_index,
                call.state);
        },
        [](xinput::service& service, const xinput::flavor flavor, const state_call& call) {
            return service.get_state(flavor, call.user_index, call.state, call.include_guide);
        });
}

DWORD WINAPI api_get_capabilities_ex(const input_next_xinput_flavor version,
    const DWORD user_index,
    const DWORD flags,
    xinput::capabilities_ex* const capabilities) noexcept {
    const capabilities_ex_call arguments{.flavor = version,
        .arguments = {.user_index = user_index, .flags = flags, .capabilities = capabilities}};
    return dispatch_status(
        arguments,
        [](const capabilities_ex_call& call) {
            return xinput_argument_policy::validate_capabilities_ex_arguments(call.arguments);
        },
        [](xinput::service& service,
            const xinput::flavor flavor,
            const capabilities_ex_call& call) {
            return service.get_capabilities_ex(flavor,
                call.arguments.user_index,
                call.arguments.flags,
                call.arguments.capabilities);
        });
}

DWORD WINAPI api_wait_for_guide_button(const input_next_xinput_flavor version,
    const DWORD user_index,
    const DWORD flags,
    LPVOID const context) noexcept {
    const guide_wait_call arguments{.flavor = version,
        .user_index = user_index,
        .flags = flags,
        .context = context};
    return dispatch_status(
        arguments,
        [](const guide_wait_call& call) {
            return xinput_argument_policy::validate_guide_wait_arguments(call.user_index);
        },
        [](xinput::service& service, const xinput::flavor flavor, const guide_wait_call& call) {
            return guide_wait_status(service, flavor, call.user_index);
        });
}

DWORD WINAPI api_cancel_guide_button_wait(const input_next_xinput_flavor version,
    const DWORD user_index) noexcept {
    const indexed_call arguments{.flavor = version, .user_index = user_index};
    return dispatch_status(
        arguments,
        [](const indexed_call& call) {
            return xinput_argument_policy::validate_cancel_guide_wait_arguments(call.user_index);
        },
        [](xinput::service& service, const xinput::flavor flavor, const indexed_call& call) {
            return guide_wait_status(service, flavor, call.user_index);
        });
}

DWORD WINAPI api_power_off_controller(const input_next_xinput_flavor version,
    const DWORD user_index) noexcept {
    const indexed_call arguments{.flavor = version, .user_index = user_index};
    return dispatch_status(
        arguments,
        [](const indexed_call& call) {
            return xinput_argument_policy::validate_power_off_arguments(call.user_index);
        },
        [](xinput::service& service, const xinput::flavor flavor, const indexed_call& call) {
            return service.power_off_controller(flavor, call.user_index);
        });
}

constexpr input_next_runtime_api_v1 api_v1{.struct_size = sizeof(input_next_runtime_api_v1),
    .abi_version = 1U,
    .get_state = &api_get_state,
    .set_state = &api_set_state,
    .get_capabilities = &api_get_capabilities,
    .enable = &api_enable,
    .get_battery_information = &api_get_battery_information,
    .get_keystroke = &api_get_keystroke,
    .get_audio_device_ids = &api_get_audio_device_ids,
    .get_dsound_audio_device_guids = &api_get_dsound_audio_device_guids,
    .get_state_ex = &api_get_state_ex,
    .get_capabilities_ex = &api_get_capabilities_ex,
    .wait_for_guide_button = &api_wait_for_guide_button,
    .cancel_guide_button_wait = &api_cancel_guide_button_wait,
    .power_off_controller = &api_power_off_controller};

static_assert(api_v1.struct_size == sizeof(input_next_runtime_api_v1));
static_assert(api_v1.abi_version == 1U);
static_assert(api_v1.get_state != nullptr && api_v1.set_state != nullptr &&
              api_v1.get_capabilities != nullptr && api_v1.enable != nullptr &&
              api_v1.get_battery_information != nullptr && api_v1.get_keystroke != nullptr &&
              api_v1.get_audio_device_ids != nullptr &&
              api_v1.get_dsound_audio_device_guids != nullptr && api_v1.get_state_ex != nullptr &&
              api_v1.get_capabilities_ex != nullptr && api_v1.wait_for_guide_button != nullptr &&
              api_v1.cancel_guide_button_wait != nullptr && api_v1.power_off_controller != nullptr);

} // namespace

const input_next_runtime_api_v1* runtime_api_v1() noexcept {
    return &api_v1;
}

} // namespace inputnext::runtime_detail

extern "C" const input_next_runtime_api_v1* __cdecl input_next_runtime_query_api(
    const std::uint32_t requested_abi) noexcept {
    return inputnext::runtime_detail::query_runtime_api(
        inputnext::runtime_detail::runtime_query_front(),
        requested_abi);
}
