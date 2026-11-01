#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/abi/xinput/api.hpp"
#include "runtime/bootstrap/bootstrap_owner.hpp"
#include "runtime_api_test_composition.hpp"
#include "support/xinput/service_test_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <thread>

namespace {

struct release_prepare_on_exit final {
    explicit release_prepare_on_exit(
        inputnext::runtime_detail::runtime_api_test_fixture& fixture_value) noexcept
        : fixture{fixture_value} {}

    ~release_prepare_on_exit() { fixture.release_prepare(); }

    release_prepare_on_exit(const release_prepare_on_exit&) = delete;
    release_prepare_on_exit& operator=(const release_prepare_on_exit&) = delete;
    release_prepare_on_exit(release_prepare_on_exit&&) = delete;
    release_prepare_on_exit& operator=(release_prepare_on_exit&&) = delete;

    inputnext::runtime_detail::runtime_api_test_fixture& fixture;
};

struct release_publication_on_exit final {
    release_publication_on_exit(inputnext::runtime_detail::runtime_api_test_fixture& fixture_value,
        std::optional<inputnext::xinput::detail::service_test_access::presentation_guard>&
            guard_value)
        : fixture{fixture_value},
          presentation_guard{guard_value} {}

    ~release_publication_on_exit() {
        presentation_guard.reset();
        fixture.release_publication_factory();
    }

    release_publication_on_exit(const release_publication_on_exit&) = delete;
    release_publication_on_exit& operator=(const release_publication_on_exit&) = delete;
    release_publication_on_exit(release_publication_on_exit&&) = delete;
    release_publication_on_exit& operator=(release_publication_on_exit&&) = delete;

    inputnext::runtime_detail::runtime_api_test_fixture& fixture;
    std::optional<inputnext::xinput::detail::service_test_access::presentation_guard>&
        presentation_guard;
};

[[nodiscard]] bool wait_for_control_gate(inputnext::runtime_detail::bootstrap_owner& owner,
    const bool available,
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (inputnext::runtime_detail::bootstrap_owner_test_access::control_gate_available(owner) ==
            available) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

[[nodiscard]] bool wait_for_flag(const std::atomic<bool>& flag,
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

[[nodiscard]] bool wait_for_state(const input_next_runtime_api_v1& api,
    const input_next_xinput_flavor flavor,
    const WORD expected_buttons,
    XINPUT_STATE& observed) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (api.get_state(flavor, 0U, &observed) == ERROR_SUCCESS &&
            observed.Gamepad.wButtons == expected_buttons) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return api.get_state(flavor, 0U, &observed) == ERROR_SUCCESS &&
           observed.Gamepad.wButtons == expected_buttons;
}

[[nodiscard]] bool wait_for_rumble_value_after(
    const inputnext::runtime_detail::runtime_api_test_fixture& fixture,
    const std::size_t previous_write_count,
    const float expected_low_frequency,
    const float expected_high_frequency) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto value = fixture.last_rumble_value();
        if (!value.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        if (fixture.rumble_writes() > previous_write_count &&
            std::abs(value.value().low_frequency - expected_low_frequency) < 0.001F &&
            std::abs(value.value().high_frequency - expected_high_frequency) < 0.001F) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto value = fixture.last_rumble_value();
    if (!value.has_value()) {
        return false;
    }
    return fixture.rumble_writes() > previous_write_count &&
           std::abs(value.value().low_frequency - expected_low_frequency) < 0.001F &&
           std::abs(value.value().high_frequency - expected_high_frequency) < 0.001F;
}

} // namespace

TEST_CASE("runtime.bootstrap.actual_abi_enable_waits_for_publication_first",
    "[integration][runtime][bootstrap][abi][concurrency]") {
    using namespace inputnext::runtime_detail;

    runtime_api_test_fixture fixture;
    fixture.block_before_publication();
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    REQUIRE(is_valid_input_next_runtime_api_v1(api));

    std::atomic<DWORD> bootstrap_status{ERROR_GEN_FAILURE};
    std::atomic<bool> late_enable_started;
    std::atomic<bool> late_enable_completed;
    std::jthread bootstrap_thread;
    std::jthread late_enable_thread;
    std::optional<inputnext::xinput::detail::service_test_access::presentation_guard>
        presentation_guard;
    // This cleanup guard releases both gates before the jthread owners join during unwinding.
    const release_publication_on_exit release{fixture, presentation_guard};

    bootstrap_thread = std::jthread([&] {
        XINPUT_STATE state{};
        bootstrap_status.store(api->get_state(INPUT_NEXT_FLAVOR_1_3, 0U, &state),
            std::memory_order_release);
    });

    auto* const prepared_presentation =
        fixture.wait_for_prepared_presentation(std::chrono::seconds{2});
    REQUIRE(prepared_presentation != nullptr);
    presentation_guard.emplace(
        inputnext::xinput::detail::service_test_access::lock_presentation(*prepared_presentation));

    auto* const owner = runtime_bootstrap_owner();
    REQUIRE(owner != nullptr);
    REQUIRE(wait_for_control_gate(*owner, true, std::chrono::seconds{2}));

    fixture.release_publication_factory();
    REQUIRE(fixture.wait_for_publication_factory_resume(std::chrono::seconds{2}));
    // The held presentation and factory barriers force the writer into the competing path. Check
    // both before and after the actual ABI caller starts because try_lock can fail spuriously.
    REQUIRE(wait_for_control_gate(*owner, false, std::chrono::seconds{2}));

    late_enable_thread = std::jthread([&] {
        late_enable_started.store(true, std::memory_order_release);
        api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
        late_enable_completed.store(true, std::memory_order_release);
    });
    REQUIRE(wait_for_flag(late_enable_started, std::chrono::seconds{2}));
    CHECK_FALSE(late_enable_completed.load(std::memory_order_acquire));
    CHECK_FALSE(bootstrap_owner_test_access::control_gate_available(*owner));

    presentation_guard.reset();
    bootstrap_thread.join();
    late_enable_thread.join();

    CHECK(bootstrap_status.load(std::memory_order_acquire) == ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.owner_is_ready());
    CHECK(late_enable_completed.load(std::memory_order_acquire));

    fixture.connect_native_device();
    XINPUT_STATE state{};
    REQUIRE(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_3, 0U, state));
    CHECK(fixture.owner_is_ready());
}

TEST_CASE("runtime.bootstrap.actual_abi_query_and_enable_share_cold_publication_control",
    "[integration][runtime][bootstrap][abi][concurrency]") {
    using namespace inputnext::runtime_detail;

    runtime_api_test_fixture fixture;
    CHECK(input_next_runtime_query_api(2U) == nullptr);
    CHECK(fixture.pin_calls() == 0U);
    CHECK(fixture.owner_creations() == 0U);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);

    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    REQUIRE(is_valid_input_next_runtime_api_v1(api));
    CHECK(input_next_runtime_query_api(1U) == api);
    CHECK(fixture.pin_calls() == 1U);
    CHECK(fixture.owner_creations() == 1U);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);

    api->enable(INPUT_NEXT_FLAVOR_1_3, TRUE);
    api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());

    fixture.block_next_prepare();
    std::atomic<DWORD> first_status{ERROR_GEN_FAILURE};
    std::atomic<DWORD> waiting_status{ERROR_GEN_FAILURE};
    XINPUT_STATE first_state{};
    XINPUT_STATE waiting_state{};
    std::jthread first;
    std::jthread waiting;
    const release_prepare_on_exit release{fixture};
    first = std::jthread([&] {
        first_status.store(api->get_state(INPUT_NEXT_FLAVOR_1_3, 0U, &first_state),
            std::memory_order_release);
    });
    const bool preparation_entered = fixture.wait_for_prepare(std::chrono::seconds{2});
    if (!preparation_entered) {
        fixture.release_prepare();
        first.join();
        FAIL("bootstrap entered the controlled preparation gate");
    }
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.focus_observations() == 1U);
    CHECK_FALSE(fixture.native_initialized());

    waiting = std::jthread([&] {
        waiting_status.store(api->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &waiting_state),
            std::memory_order_release);
    });
    api->enable(INPUT_NEXT_FLAVOR_1_3, TRUE);
    api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    CHECK(fixture.focus_observations() == 1U);
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK_FALSE(fixture.native_initialized());
    fixture.release_prepare();
    first.join();
    waiting.join();
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();

    CHECK((first_status.load(std::memory_order_acquire) == ERROR_SUCCESS ||
           first_status.load(std::memory_order_acquire) == ERROR_DEVICE_NOT_CONNECTED));
    CHECK((waiting_status.load(std::memory_order_acquire) == ERROR_SUCCESS ||
           waiting_status.load(std::memory_order_acquire) == ERROR_DEVICE_NOT_CONNECTED));
    REQUIRE(fixture.native_initialized());
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.focus_observations() == 1U);

    XINPUT_STATE state{};
    REQUIRE(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_4, 0U, state));
    XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 20'000U, .wRightMotorSpeed = 50'000U};
    CHECK(api->set_state(INPUT_NEXT_FLAVOR_1_4, 0U, &vibration) == ERROR_SUCCESS);
    REQUIRE(wait_for_rumble_value_after(fixture, 0U, 0.0F, 0.0F));
    const auto writes_before_enable = fixture.rumble_writes();

    const auto expected_low_frequency = 20'000.0F / 65'535.0F;
    const auto expected_high_frequency = 50'000.0F / 65'535.0F;
    api->enable(INPUT_NEXT_FLAVOR_1_3, TRUE);
    REQUIRE(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_4, XINPUT_GAMEPAD_A, state));
    REQUIRE(wait_for_rumble_value_after(fixture,
        writes_before_enable,
        expected_low_frequency,
        expected_high_frequency));
    const auto rumble = fixture.last_rumble_value();
    if (!rumble.has_value()) {
        FAIL("re-enabled output has no observed restored value");
        return;
    }
    CHECK(std::abs(rumble->low_frequency - expected_low_frequency) < 0.001F);
    CHECK(std::abs(rumble->high_frequency - expected_high_frequency) < 0.001F);

    const auto observations = fixture.focus_observations();
    const auto preparations = fixture.prepare_attempts();
    XINPUT_STATE hot_state{};
    CHECK(api->get_state(INPUT_NEXT_FLAVOR_1_3, 0U, &hot_state) == ERROR_SUCCESS);
    api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    CHECK(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_3, 0U, hot_state));
    CHECK(fixture.focus_observations() == observations);
    CHECK(fixture.prepare_attempts() == preparations);
    CHECK(fixture.rumble_writes() > 0U);
}

TEST_CASE("runtime.bootstrap.cold_false_then_true_is_applied_before_publication",
    "[integration][runtime][bootstrap][abi]") {
    using namespace inputnext::runtime_detail;

    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    REQUIRE(is_valid_input_next_runtime_api_v1(api));

    api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    api->enable(INPUT_NEXT_FLAVOR_1_3, TRUE);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());

    XINPUT_STATE state{};
    CHECK(api->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &state) == ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();
    REQUIRE(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_4, XINPUT_GAMEPAD_A, state));
    CHECK(fixture.owner_is_ready());
    CHECK(fixture.focus_observations() == 1U);
    CHECK(fixture.prepare_attempts() == 1U);
}

TEST_CASE("runtime.bootstrap.cold_false_without_prior_true_stays_disabled",
    "[integration][runtime][bootstrap][abi]") {
    using namespace inputnext::runtime_detail;

    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    REQUIRE(is_valid_input_next_runtime_api_v1(api));

    api->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);

    XINPUT_STATE state{};
    CHECK(api->get_state(INPUT_NEXT_FLAVOR_1_3, 0U, &state) == ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();
    REQUIRE(wait_for_state(*api, INPUT_NEXT_FLAVOR_1_3, 0U, state));
    CHECK(fixture.owner_is_ready());
    CHECK(fixture.focus_observations() == 1U);
    CHECK(fixture.prepare_attempts() == 1U);
}
