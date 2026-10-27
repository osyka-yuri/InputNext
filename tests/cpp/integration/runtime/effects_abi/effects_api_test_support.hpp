#pragma once

#include "runtime/abi/runtime_api.hpp"
#include "runtime_api_test_composition.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

namespace inputnext::effects_abi_test_support {

using namespace std::chrono_literals;

struct target_options final {
    std::uint64_t device_key{2U};
    std::uint64_t connection_id{3U};
};

[[nodiscard]] inline std::optional<input_next_device_target_v1> wait_for_device(
    const input_next_core_api_v1& api) {
    std::array<input_next_device_info_v1, 16U> devices{};
    for (auto& device : devices) {
        device.struct_size = sizeof(device);
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        std::uint32_t count{};
        std::uint64_t revision{};
        if (api.enumerate_devices(devices.data(),
                static_cast<std::uint32_t>(devices.size()),
                &count,
                &revision) == INPUT_NEXT_STATUS_OK &&
            count == 1U && revision != 0U) {
            return devices[0U].target;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

[[nodiscard]] inline input_next_effects_context_v1 test_context(std::uint64_t id = 7U) {
    input_next_effects_context_v1 context{};
    context.struct_size = sizeof(context);
    context.runtime_id[0U] = 0x51U;
    context.id = id;
    return context;
}

[[nodiscard]] inline input_next_device_target_v1 test_target(const target_options options = {}) {
    input_next_device_target_v1 target{};
    target.struct_size = sizeof(target);
    target.runtime_id[0U] = 0x51U;
    target.device_key = options.device_key;
    target.connection_id = options.connection_id;
    return target;
}

[[nodiscard]] inline input_next_device_selector_v1 selector_for(
    const input_next_device_target_v1& target) {
    input_next_device_selector_v1 selector{};
    selector.struct_size = sizeof(selector);
    selector.kind = INPUT_NEXT_CORE_SELECTOR_EXACT_TARGET;
    selector.target = target;
    return selector;
}

[[nodiscard]] inline input_next_effects_request_v1
rumble_request(const input_next_effects_context_v1& context, const std::uint64_t sequence = 1U) {
    input_next_effects_request_v1 request{};
    request.struct_size = sizeof(request);
    request.channel_mask = INPUT_NEXT_EFFECTS_RUMBLE;
    request.ttl_ms = 1000U;
    request.context = context;
    request.mutation_sequence = sequence;
    request.rumble = {.low_frequency = 0.25F, .high_frequency = 0.5F};
    return request;
}

[[nodiscard]] inline input_next_effects_body_request_v1
body_keep_request(const input_next_effects_context_v1& context, const std::uint64_t sequence = 1U) {
    input_next_effects_body_request_v1 request{};
    request.struct_size = sizeof(request);
    request.ttl_ms = 1000U;
    request.route_preference = INPUT_NEXT_EFFECTS_BODY_ROUTE_CLASSIC;
    request.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_KEEP;
    request.context = context;
    request.sequence = sequence;
    request.fallback_rumble = {.low_frequency = 0.125F, .high_frequency = 0.25F};
    request.pcm.struct_size = sizeof(request.pcm);
    return request;
}

inline void initialize_context_output(input_next_effects_context_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
}

inline void initialize_context_info_output(input_next_effects_context_info_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
    value.context.struct_size = sizeof(value.context);
}

inline void initialize_receipt_output(input_next_effects_receipt_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
    value.context.struct_size = sizeof(value.context);
}

inline void initialize_body_receipt_output(input_next_effects_body_receipt_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
    value.context.struct_size = sizeof(value.context);
    value.target.struct_size = sizeof(value.target);
}

inline void initialize_close_receipt_output(input_next_effects_close_receipt_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
    value.context.struct_size = sizeof(value.context);
    value.target.struct_size = sizeof(value.target);
}

inline void initialize_progress_output(input_next_effects_progress_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
    value.target.struct_size = sizeof(value.target);
}

inline void initialize_capabilities_output(input_next_effects_capabilities_v1& value) noexcept {
    value = {};
    value.struct_size = sizeof(value);
}

[[nodiscard]] inline bool wait_for_rumble_value(
    const runtime_detail::runtime_api_test_fixture& fixture,
    const std::size_t writes_before,
    const float expected_low,
    const float expected_high) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        const auto value = fixture.last_rumble_value();
        if (value.has_value() && fixture.rumble_writes() > writes_before &&
            std::abs(value->low_frequency - expected_low) < 0.001F &&
            std::abs(value->high_frequency - expected_high) < 0.001F) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

} // namespace inputnext::effects_abi_test_support
