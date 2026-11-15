#include "native_session_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/input_access.hpp"
#include "runtime/session/native_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace inputnext::runtime_session_test_support {

TEST_CASE("runtime.session.directinput_calls_drain_before_session_ports_are_destroyed",
    "[integration][runtime][session][dinput][concurrency]") {
    session_fixture fixture;
    REQUIRE(fixture.session != nullptr);
    const auto access = fixture.session->directinput();
    REQUIRE(access != nullptr);
    REQUIRE(access->open());

    std::promise<void> completed;
    const auto completion = completed.get_future();
    // Release admitted work before joining; keep completion storage through that join.
    std::jthread shutdown_worker;
    auto admitted = access->enter();
    REQUIRE(static_cast<bool>(admitted));
    shutdown_worker = std::jthread{[&] {
        fixture.session->shutdown();
        completed.set_value();
    }};
    CHECK(wait_until([&] { return !access->open(); }));
    CHECK_FALSE(static_cast<bool>(access->enter()));
    CHECK(completion.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
    CHECK(fixture.destruction_counts->native.load(std::memory_order_relaxed) == 0U);

    admitted = dinput::input_access::call{};
    CHECK(completion.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    shutdown_worker.join();
    CHECK(fixture.destruction_counts->native.load(std::memory_order_relaxed) == 1U);
    CHECK(fixture.session->directinput() == access);
    CHECK_FALSE(access->open());
    fixture.session.reset();
    CHECK_FALSE(static_cast<bool>(access->enter()));
    access->drain();
}

TEST_CASE("runtime.session.directinput_discovery_notifies_after_native_maintenance_admission",
    "[integration][runtime][session][dinput][concurrency]") {
    session_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    fixture.configure_device(history);
    REQUIRE(fixture.start().has_value());

    const auto access = fixture.session->directinput();
    REQUIRE(access != nullptr);
    auto waiter = access->discovery().begin();
    REQUIRE(waiter != nullptr);
    REQUIRE(waiter->prepare());

    fixture.native->fake->block_next_facts();
    scoped_facts_release facts_release{*fixture.native->fake};
    fixture.session->activate();

    REQUIRE(fixture.native->fake->wait_for_blocked_facts());
    CHECK(WaitForSingleObject(waiter->event(), 0U) == WAIT_TIMEOUT);

    facts_release.release();
    constexpr auto wait_timeout = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(wait_limit).count());
    CHECK(WaitForSingleObject(waiter->event(), wait_timeout) == WAIT_OBJECT_0);

    const auto catalogue_ready = wait_until([&fixture] {
        const auto catalogue = fixture.session->core().enumerate();
        return catalogue.has_value() && catalogue->count == 1U;
    });
    CHECK(catalogue_ready);
    if (catalogue_ready) {
        const auto catalogue = fixture.session->core().enumerate();
        REQUIRE(catalogue.has_value());
        REQUIRE(catalogue->count == 1U);
        CHECK(catalogue->entries[0U].vendor_id == 0x045EU);
        CHECK(catalogue->entries[0U].product_id == 0x02FDU);
    }

    fixture.session->shutdown();
}

TEST_CASE("runtime.session.directinput_discovery_close_wakes_retained_waiter_after_shutdown",
    "[integration][runtime][session][dinput][shutdown][concurrency]") {
    session_fixture fixture;
    REQUIRE(fixture.session != nullptr);

    auto access = fixture.session->directinput();
    REQUIRE(access != nullptr);
    auto waiter = access->discovery().begin();
    REQUIRE(waiter != nullptr);
    REQUIRE(waiter->prepare());
    const auto deadline = waiter->deadline();

    const HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (cancel_event == nullptr) {
        throw std::runtime_error{"failed to create discovery wait cancellation event"};
    }

    struct event_guard final {
        explicit event_guard(HANDLE value) noexcept : event(value) {}

        ~event_guard() {
            if (event != nullptr) {
                CloseHandle(event);
            }
        }

        event_guard(const event_guard&) = delete;
        event_guard& operator=(const event_guard&) = delete;
        event_guard(event_guard&&) = delete;
        event_guard& operator=(event_guard&&) = delete;

        HANDLE event;
    };

    const event_guard cancel_event_owner{cancel_event};

    std::promise<void> wait_started;
    const auto started = wait_started.get_future();
    std::promise<DWORD> wait_completed;
    auto completion = wait_completed.get_future();
    std::jthread wait_worker{
        [&wait_started, &wait_completed, event = waiter->event(), cancel_event] {
            const std::array events{event, cancel_event};
            wait_started.set_value();
            wait_completed.set_value(WaitForMultipleObjects(static_cast<DWORD>(events.size()),
                events.data(),
                FALSE,
                INFINITE));
        }};

    struct shutdown_guard final {
        shutdown_guard(session_fixture& owner, HANDLE event) noexcept
            : fixture(owner),
              cancel_event(event) {}

        shutdown_guard(const shutdown_guard&) = delete;
        shutdown_guard& operator=(const shutdown_guard&) = delete;
        shutdown_guard(shutdown_guard&&) = delete;
        shutdown_guard& operator=(shutdown_guard&&) = delete;

        session_fixture& fixture;
        HANDLE cancel_event;

        ~shutdown_guard() {
            if (fixture.session != nullptr) {
                fixture.session->shutdown();
            }
            static_cast<void>(SetEvent(cancel_event));
        }
    };

    const shutdown_guard shutdown_on_exit{fixture, cancel_event};

    CHECK(started.wait_for(wait_limit) == std::future_status::ready);
    fixture.session->shutdown();
    const auto wake_completed = completion.wait_for(wait_limit) == std::future_status::ready;
    CHECK(wake_completed);
    if (wake_completed) {
        CHECK(completion.get() == WAIT_OBJECT_0);
    } else {
        static_cast<void>(SetEvent(cancel_event));
        CHECK(completion.wait_for(wait_limit) == std::future_status::ready);
    }

    fixture.session.reset();
    access.reset();
    CHECK(waiter->deadline() == deadline);
    CHECK_FALSE(waiter->prepare());
    CHECK(WaitForSingleObject(waiter->event(), 0U) == WAIT_OBJECT_0);

    wait_worker.join();
    waiter.reset();
}

} // namespace inputnext::runtime_session_test_support
