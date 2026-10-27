#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "effects_api_test_support.hpp"
#include "effects_demo.h"
#include "runtime_api_test_composition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <input_next_core.h>
#include <input_next_effects.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <span>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::effects_abi_test_support;
using inputnext::audio_detail::native_operation;
using inputnext::runtime_detail::runtime_api_test_fixture;
using inputnext::runtime_detail::runtime_api_test_options;

[[nodiscard]] bool wait_for_example_target(const input_next_effects_api_v1& api,
    const bool require_hd) {
    input_next_device_selector_v1 selector{};
    selector.struct_size = sizeof(selector);
    selector.kind = INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT;
    selector.target.struct_size = sizeof(selector.target);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        input_next_device_target_v1 target{};
        target.struct_size = sizeof(target);
        input_next_effects_capabilities_v1 capabilities{};
        initialize_capabilities_output(capabilities);
        if (api.resolve_target(&selector, &target, &capabilities) == INPUT_NEXT_STATUS_OK &&
            (!require_hd || (capabilities.feature_flags & INPUT_NEXT_EFFECTS_CAP_HD_READY) != 0U)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST_CASE("runtime.effects.example_rumble_runs_through_the_actual_effects_table",
    "[integration][runtime][effects][abi][example]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);
    REQUIRE(core != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(core->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();
    REQUIRE(wait_for_device(*core).has_value());
    REQUIRE(wait_for_example_target(*api, false));

    const auto writes_before = fixture.rumble_writes();
    CHECK(inputnext_example_effect(api, 0) == INPUT_NEXT_STATUS_OK);
    CHECK(fixture.rumble_writes() > writes_before);
    CHECK(fixture.prepare_attempts() == 1U);
}

TEST_CASE("runtime.effects.example_body_pcm_runs_and_renders_through_the_actual_table",
    "[integration][runtime][effects][abi][example][pcm]") {
    runtime_api_test_fixture fixture{runtime_api_test_options{.dualsense_hd_audio = true}};
    const auto* const api = input_next_runtime_query_effects_api(INPUT_NEXT_EFFECTS_ABI_VERSION);
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);
    REQUIRE(core != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(core->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();
    REQUIRE(wait_for_device(*core).has_value());
    REQUIRE(wait_for_example_target(*api, true));

    const auto audio = fixture.controlled_audio_state();
    REQUIRE(audio != nullptr);
    const auto releases_before = audio->count_calls(native_operation::release_buffer);
    std::atomic<input_next_status> result{INPUT_NEXT_STATUS_UNAVAILABLE};
    std::jthread example{
        [&] { result.store(inputnext_example_effect(api, 1), std::memory_order_release); }};

    bool rendered_pcm{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!rendered_pcm && std::chrono::steady_clock::now() < deadline) {
        audio->signal_buffer_event();
        std::this_thread::sleep_for(1ms);

        if (audio->count_calls(native_operation::release_buffer) <= releases_before) {
            continue;
        }
        std::scoped_lock lock{audio->mutex};
        rendered_pcm = std::ranges::any_of(std::span{audio->render_samples}.first(32U),
            [](const float sample) { return std::abs(sample) > 0.00001F; });
    }
    example.join();

    CHECK(rendered_pcm);
    CHECK(result.load(std::memory_order_acquire) == INPUT_NEXT_STATUS_OK);
    CHECK(audio->count_calls(native_operation::start) != 0U);
    CHECK(fixture.prepare_attempts() == 1U);
}

} // namespace
