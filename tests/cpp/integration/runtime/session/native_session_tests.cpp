#include "native_session_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <new>
#include <system_error>
#include <thread>

namespace inputnext::runtime_session_test_support {
namespace {

std::thread fail_worker_allocation([[maybe_unused]] void* launcher_context,
    [[maybe_unused]] const worker_entry entry,
    [[maybe_unused]] void* worker_context) {
    throw std::bad_alloc{};
}

std::thread fail_worker_creation([[maybe_unused]] void* launcher_context,
    [[maybe_unused]] const worker_entry entry,
    [[maybe_unused]] void* worker_context) {
    throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again),
        "injected session thread-start failure"};
}

} // namespace

TEST_CASE("runtime.session.preparation_failures_precede_native_startup",
    "[integration][runtime][session][prepare]") {
    SECTION("worker allocation failure") {
        session_fixture fixture{&fail_worker_allocation};
        REQUIRE(fixture.preparation_error != nullptr);
        CHECK(fixture.session == nullptr);
        CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 0U);

        bool observed_bad_alloc{};
        try {
            std::rethrow_exception(fixture.preparation_error);
        } catch (const std::bad_alloc&) {
            observed_bad_alloc = true;
        }
        CHECK(observed_bad_alloc);
    }

    SECTION("worker thread creation failure") {
        session_fixture fixture{&fail_worker_creation};
        REQUIRE(fixture.preparation_error != nullptr);
        CHECK(fixture.session == nullptr);
        CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 0U);

        bool observed_system_error{};
        try {
            std::rethrow_exception(fixture.preparation_error);
        } catch (const std::system_error&) {
            observed_system_error = true;
        }
        CHECK(observed_system_error);
    }
}

TEST_CASE("runtime.session.startup_failure_is_terminal_and_drains_the_partial_native_owner",
    "[integration][runtime][session][degraded]") {
    session_fixture fixture;
    fixture.native->fake->fail_next_watch_registration_with_partial_token();

    const auto startup = fixture.session->start();
    REQUIRE_FALSE(startup.has_value());
    const auto retained_failure = startup.error();
    CHECK(retained_failure.code == gameinput_error::registration_failed);
    CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 1U);

    fixture.session->activate();
    REQUIRE(wait_until([&] { return fixture.rumble_controls->native_cleanup_complete(); }));
    CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 1U);

    const auto core_info = fixture.session->core().info();
    CHECK(core_info.runtime.valid());
    const auto inventory = fixture.session->core().enumerate();
    REQUIRE_FALSE(inventory.has_value());
    CHECK(inventory.error() == core::status::unavailable);
    CHECK(retained_failure.code == gameinput_error::registration_failed);

    XINPUT_STATE state{};
    CHECK(fixture.session->presentation().get_state(xinput::flavor::v1_4, 0U, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    fixture.session->shutdown();
}

TEST_CASE("runtime.session.unexpected_maintenance_failure_drains_without_rebuilding",
    "[integration][runtime][session][degraded]") {
    session_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    fixture.configure_device(history);
    REQUIRE(fixture.start().has_value());
    fixture.session->activate();
    REQUIRE(fixture.wait_for_presentation());

    fixture.native->throw_next_facts.store(true, std::memory_order_release);
    fixture.native->fake->emit(fixture.device, native_device_event::haptic_info_updated);
    REQUIRE(wait_until([&] { return fixture.rumble_controls->native_cleanup_complete(); }));
    CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 1U);

    XINPUT_STATE state{};
    CHECK(fixture.session->presentation().get_state(xinput::flavor::v1_4, 0U, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    fixture.session->shutdown();
}

TEST_CASE("runtime.session.explicit_shutdown_drains_owners_in_dependency_order",
    "[integration][runtime][session][shutdown]") {
    session_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    fixture.configure_device(history);
    REQUIRE(fixture.start().has_value());
    fixture.session->activate();
    REQUIRE(fixture.wait_for_presentation());

    fixture.session->shutdown();
    CHECK(fixture.rumble_controls->native_cleanup_complete());
    CHECK(fixture.initialize_calls->load(std::memory_order_relaxed) == 1U);
}

} // namespace inputnext::runtime_session_test_support
