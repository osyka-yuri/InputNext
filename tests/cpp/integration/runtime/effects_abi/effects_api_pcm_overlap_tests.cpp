#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "effects_api_test_support.hpp"
#include "runtime_api_test_composition.hpp"
#include "values/required_value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <input_next_core.h>
#include <input_next_effects.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::effects_abi_test_support;
using inputnext::runtime_detail::runtime_api_test_fixture;
using inputnext::test::required_value;
using inputnext::test::audio::controlled_native_port_state;

struct hd_resolution final {
    input_next_device_target_v1 target{};
    input_next_effects_capabilities_v1 capabilities{};
};

[[nodiscard]] std::optional<hd_resolution>
wait_for_hd_resolution(const input_next_effects_api_v1& api, const input_next_core_api_v1& core) {
    const auto discovered = wait_for_device(core);
    if (!discovered.has_value()) {
        return std::nullopt;
    }
    const auto selector = selector_for(*discovered);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        hd_resolution result{};
        result.target = *discovered;
        initialize_capabilities_output(result.capabilities);
        const auto status = api.resolve_target(&selector, &result.target, &result.capabilities);
        if (status == INPUT_NEXT_STATUS_OK &&
            (result.capabilities.feature_flags & INPUT_NEXT_EFFECTS_CAP_HD_READY) != 0U &&
            result.capabilities.audio_instance_id != 0U) {
            return result;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

[[nodiscard]] std::size_t release_buffer_calls_locked(
    const controlled_native_port_state& state) noexcept {
    std::size_t count{};
    for (std::size_t index = 0U; index < state.stored_call_count; ++index) {
        count += state.calls[index].operation ==
                         inputnext::audio_detail::native_operation::release_buffer
                     ? 1U
                     : 0U;
    }
    return count;
}

[[nodiscard]] float mixed(float value) noexcept {
    return value - ((value * value * value) / 6.0F);
}

[[nodiscard]] bool wait_for_rendered_samples(controlled_native_port_state& state,
    const std::array<float, 8U>& expected) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t previous_releases{};
        {
            std::scoped_lock lock{state.mutex};
            previous_releases = release_buffer_calls_locked(state);
        }
        state.signal_buffer_event();

        for (;;) {
            std::array<float, 32U> rendered{};
            std::size_t releases{};
            {
                std::scoped_lock lock{state.mutex};
                releases = release_buffer_calls_locked(state);
                std::copy_n(state.render_samples.begin(), rendered.size(), rendered.begin());
            }
            if (releases > previous_releases) {
                bool matches = true;
                for (std::size_t frame = 0U; frame < expected.size(); ++frame) {
                    const auto rear_offset = frame * 4U;
                    matches =
                        matches && std::abs(rendered[rear_offset] - 0.0F) < 0.00001F &&
                        std::abs(rendered[rear_offset + 1U] - 0.0F) < 0.00001F &&
                        std::abs(rendered[rear_offset + 2U] - mixed(expected[frame])) < 0.00001F &&
                        std::abs(rendered[rear_offset + 3U] - mixed(expected[frame])) < 0.00001F;
                }
                if (matches) {
                    return true;
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    return false;
}

TEST_CASE("runtime.effects.body_pcm_is_owned_before_overlapping_receipt_copyout",
    "[integration][runtime][effects][abi][pcm]") {
    runtime_api_test_fixture fixture{
        inputnext::runtime_detail::runtime_api_test_options{.dualsense_hd_audio = true}};
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);
    REQUIRE(core != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(core->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();

    const auto resolved = required_value(wait_for_hd_resolution(*api, *core));
    const auto audio = fixture.controlled_audio_state();
    REQUIRE(audio != nullptr);

    input_next_effects_open_v1 open{};
    open.struct_size = sizeof(open);
    open.target = resolved.target;
    input_next_effects_context_v1 context{};
    initialize_context_output(context);
    REQUIRE(api->open_context(&open, &context) == INPUT_NEXT_STATUS_OK);

    alignas(input_next_effects_body_receipt_v1)
        std::array<std::byte, sizeof(input_next_effects_body_receipt_v1)>
            overlapping_storage{};
    input_next_effects_body_receipt_v1 capacity{};
    initialize_body_receipt_output(capacity);
    std::memcpy(overlapping_storage.data(), &capacity, sizeof(capacity));

    constexpr std::array samples{0.25F, -0.5F, 0.75F, -0.25F, 0.125F, -0.75F, 0.5F, -1.0F};
    constexpr auto sample_offset = offsetof(input_next_effects_body_receipt_v1, lease_id);
    std::memcpy(overlapping_storage.data() + sample_offset, samples.data(), sizeof(samples));

    input_next_effects_body_request_v1 request{};
    request.struct_size = sizeof(request);
    request.ttl_ms = 1000U;
    request.route_preference = INPUT_NEXT_EFFECTS_BODY_ROUTE_PREFER_HD;
    request.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_REPLACE;
    request.context = context;
    request.audio_instance_id = resolved.capabilities.audio_instance_id;
    request.sequence = 1U;
    request.fallback_rumble = {.low_frequency = 0.0F, .high_frequency = 0.0F};
    request.pcm = {.struct_size = sizeof(request.pcm),
        .frame_count = static_cast<std::uint32_t>(samples.size()),
        .channels = 1U,
        .sample_rate = 48000U,
        .gain = 1.0F,
        .reserved = 0U,
        .samples = reinterpret_cast<const float*>(overlapping_storage.data() + sample_offset)};

    auto* const overlapping_receipt =
        reinterpret_cast<input_next_effects_body_receipt_v1*>(overlapping_storage.data());
    REQUIRE(api->submit_body(&request, overlapping_receipt) == INPUT_NEXT_STATUS_OK);

    input_next_effects_body_receipt_v1 receipt{};
    std::memcpy(&receipt, overlapping_storage.data(), sizeof(receipt));
    CHECK(receipt.effective_route == INPUT_NEXT_EFFECTS_BODY_EFFECTIVE_HD);
    CHECK(receipt.audio_instance_id == resolved.capabilities.audio_instance_id);
    CHECK(receipt.sequence == request.sequence);
    CHECK(receipt.publication_revision != 0U);

    std::array<float, samples.size()> overwritten_samples{};
    std::memcpy(overwritten_samples.data(),
        overlapping_storage.data() + sample_offset,
        sizeof(overwritten_samples));
    CHECK(overwritten_samples != samples);
    REQUIRE(wait_for_rendered_samples(*audio, samples));

    input_next_effects_body_clear_v1 clear{};
    clear.struct_size = sizeof(clear);
    clear.context = context;
    clear.lease_id = receipt.lease_id;
    clear.audio_instance_id = receipt.audio_instance_id;
    clear.sequence = 2U;
    CHECK(api->clear_body(&clear) == INPUT_NEXT_STATUS_OK);

    input_next_effects_close_v1 close{};
    close.struct_size = sizeof(close);
    close.context = context;
    close.close_request_id = context.id;
    input_next_effects_close_receipt_v1 close_receipt{};
    initialize_close_receipt_output(close_receipt);
    CHECK(api->close_context(&close, &close_receipt) == INPUT_NEXT_STATUS_REVOKED);
    CHECK(api->ack_close(&close_receipt) == INPUT_NEXT_STATUS_OK);
}

} // namespace
