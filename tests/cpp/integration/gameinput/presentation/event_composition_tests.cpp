#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/reading_parts.hpp"
#include "support/focus/controlled_source.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>
#include <inputnext/xinput_types.hpp>

#include "gameinput_probe/input/xinput/event_observer.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;
using namespace inputnext::gameinput_test_support;

constexpr std::uint64_t qpc_frequency = 1'000'000U;
constexpr std::size_t public_reader_capacity = 28U;
constexpr button_mask south_button = static_cast<button_mask>(gamepad_button::south);
constexpr button_mask guide_button = static_cast<button_mask>(gamepad_button::guide);

class event_composition_fixture final {
public:
    event_composition_fixture() : script(std::make_shared<native_history_script>()) {}

    ~event_composition_fixture() {
        if (presentation) {
            presentation->close();
        }
        if (backing.runtime) {
            for (std::size_t index = 0U; index < public_reader_count; ++index) {
                const auto closed = backing.runtime->close_history(public_readers[index]);
                CHECK(closed.has_value());
            }
        }
        if (backing.runtime) {
            const auto stopped = backing.runtime->stop(backing.now);
            CHECK(stopped.drained);
        }
        if (focus_observer) {
            focus_observer->stop();
        }
        presentation.reset();
        focus_observer.reset();
    }

    event_composition_fixture(const event_composition_fixture&) = delete;
    event_composition_fixture& operator=(const event_composition_fixture&) = delete;
    event_composition_fixture(event_composition_fixture&&) = delete;
    event_composition_fixture& operator=(event_composition_fixture&&) = delete;

    void start() {
        script->qpc_frequency = qpc_frequency;
        script->seed.qpc_frequency = qpc_frequency;
        script->seed.baseline = input_update{
            .state = test_pad(0U),
            .source_timestamp_us = 100U,
            .observation_qpc = 1'000U,
        };
        device = handle(backing.device_memory[0U]);
        backing.native->configure_history(script);
        backing.native->set_current(device,
            native_reading_lane::gamepad,
            pad_parts(test_pad(0U), 100U, 1'000U));
        backing.native->set_current(device,
            native_reading_lane::system_button,
            reading_parts{.guide = false, .source_timestamp_us = 100U, .observation_qpc = 1'000U});

        REQUIRE(backing.start().has_value());
        backing.connect(device);
        backing.maintain();
        REQUIRE(backing.connection_count() == 1U);
        target = backing.connection().target;

        const auto process = focus::process_instance{.pid = 321U, .creation_time_100ns = 987654U};
        auto source = std::make_unique<test::focus::controlled_source>(process,
            std::vector<focus::reading>{
                {.process = process, .state = focus::evidence::foreground},
            });
        focus_observer = std::make_unique<focus::observer>(focus_incarnation(), std::move(source));
        presentation =
            std::make_unique<gameinput_xinput>(*backing.runtime, *focus_observer, qpc_frequency);
        const auto synchronized = presentation->synchronize(backing.runtime->connections());
        REQUIRE(synchronized.has_value());
        REQUIRE(wait_for_foreground());
    }

    [[nodiscard]] bool open_public_readers_to_capacity() {
        for (auto& reader : public_readers) {
            auto opened = backing.runtime->open_history(target, backing.now);
            if (!opened.has_value()) {
                return false;
            }
            reader = *opened;
            ++public_reader_count;
        }
        return true;
    }

    void pump() {
        const auto now = backing.now;
        backing.runtime->maintain(now);
        const auto inventory = backing.runtime->connections();
        const auto synchronized = presentation->synchronize(inventory);
        REQUIRE(synchronized.has_value());
        presentation->maintain_events(now);
        backing.now += 4ms;
    }

    [[nodiscard]] bool wait_for_foreground() const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (focus_observer->published().state == focus::evidence::foreground) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] static runtime_id focus_incarnation() noexcept {
        runtime_id result{};
        result.bytes[0U] = std::byte{0x75};
        return result;
    }

    runtime_fixture backing;
    std::shared_ptr<native_history_script> script;
    std::unique_ptr<focus::observer> focus_observer;
    std::unique_ptr<gameinput_xinput> presentation;
    std::array<history_reader_handle, public_reader_capacity> public_readers{};
    std::size_t public_reader_count{};
    native_device_handle device{};
    connection_target target{};
};

class guide_wait_join_guard final {
public:
    guide_wait_join_guard(xinput::service& service,
        gameinput_xinput& presentation,
        xinput::guide_wait_ticket ticket,
        std::thread& waiter) noexcept
        : service_(service),
          presentation_(presentation),
          ticket_(ticket),
          waiter_(waiter) {}

    ~guide_wait_join_guard() {
        if (waiter_.joinable()) {
            try {
                const auto cancelled = service_.cancel_guide_wait(ticket_);
                CHECK(cancelled.has_value());
            } catch (...) {
                CHECK(false);
            }
            try {
                presentation_.close();
            } catch (...) {
                CHECK(false);
            }
            waiter_.join();
        }
    }

    guide_wait_join_guard(const guide_wait_join_guard&) = delete;
    guide_wait_join_guard& operator=(const guide_wait_join_guard&) = delete;
    guide_wait_join_guard(guide_wait_join_guard&&) = delete;
    guide_wait_join_guard& operator=(guide_wait_join_guard&&) = delete;

private:
    xinput::service& service_;
    gameinput_xinput& presentation_;
    xinput::guide_wait_ticket ticket_;
    std::thread& waiter_;
};

class history_call_release_guard final {
public:
    history_call_release_guard(native_history_script& script, std::thread& caller) noexcept
        : script_(script),
          caller_(caller) {}

    ~history_call_release_guard() {
        script_.release_blocked_call();
        if (caller_.joinable()) {
            caller_.join();
        }
    }

    history_call_release_guard(const history_call_release_guard&) = delete;
    history_call_release_guard& operator=(const history_call_release_guard&) = delete;
    history_call_release_guard(history_call_release_guard&&) = delete;
    history_call_release_guard& operator=(history_call_release_guard&&) = delete;

private:
    native_history_script& script_;
    std::thread& caller_;
};

class history_call_future_release_guard final {
public:
    // Declared after its future, this releases native work before the future waits on destruction.
    explicit history_call_future_release_guard(native_history_script& script) noexcept
        : script_(script) {}

    ~history_call_future_release_guard() { script_.release_blocked_call(); }

    history_call_future_release_guard(const history_call_future_release_guard&) = delete;
    history_call_future_release_guard& operator=(const history_call_future_release_guard&) = delete;
    history_call_future_release_guard(history_call_future_release_guard&&) = delete;
    history_call_future_release_guard& operator=(history_call_future_release_guard&&) = delete;

private:
    native_history_script& script_;
};

TEST_CASE("gameinput.presentation.composition_uses_reserved_history_for_keys_and_guide_waits",
    "[integration][gameinput][presentation][events]") {
    event_composition_fixture fixture;
    fixture.start();

    REQUIRE(fixture.open_public_readers_to_capacity());
    CHECK(fixture.public_reader_count == public_reader_capacity);
    const auto overflow =
        fixture.backing.runtime->open_history(fixture.target, fixture.backing.now);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == history_error::capacity_exceeded);

    const auto public_baseline =
        fixture.backing.runtime->read_history(fixture.public_readers[0U], fixture.backing.now);
    REQUIRE(public_baseline.count == 1U);
    CHECK(public_baseline.records[0U].baseline);

    fixture.pump();
    CHECK(fixture.script->cursor_factories.load(std::memory_order_relaxed) == 1U);

    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(south_button),
                .source_timestamp_us = 110U,
                .observation_qpc = 1'100U,
            },
    });
    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = 1'100U,
    });
    fixture.pump();

    XINPUT_KEYSTROKE key{};
    const DWORD key_result =
        fixture.presentation->presentation().get_keystroke(xinput::flavor::v1_4, 0U, 0U, &key);
    CHECK(key_result == ERROR_SUCCESS);
    CHECK(key.VirtualKey == 0x5800U);
    CHECK((key.Flags & XINPUT_KEYSTROKE_KEYDOWN) != 0U);
    const auto public_tail =
        fixture.backing.runtime->read_history(fixture.public_readers[0U], fixture.backing.now);
    CHECK_FALSE(public_tail.failure.has_value());
    REQUIRE(public_tail.count > 0U);
    CHECK((public_tail.records[0U].sample.state.buttons & south_button) != 0U);

    const auto begun = fixture.presentation->presentation().begin_guide_wait(0U);
    REQUIRE(begun.has_value());
    std::optional<xinput::guide_wait_result<void>> waited;
    std::exception_ptr wait_exception;
    std::thread waiter([&] {
        try {
            waited.emplace(fixture.presentation->presentation().wait_for_guide_button(*begun));
        } catch (...) {
            wait_exception = std::current_exception();
        }
    });
    const guide_wait_join_guard join_waiter{fixture.presentation->presentation(),
        *fixture.presentation,
        *begun,
        waiter};

    fixture.backing.native->publish(fixture.device,
        reading_parts{.guide = true, .source_timestamp_us = 120U, .observation_qpc = 1'200U});
    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(south_button),
                .source_timestamp_us = 130U,
                .observation_qpc = 1'300U,
            },
    });
    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = 1'300U,
    });
    fixture.pump();
    waiter.join();

    CHECK_FALSE(wait_exception);
    REQUIRE(waited.has_value());
    const auto wait_result =
        waited.value_or(std::unexpected(xinput::guide_wait_error::invalid_ticket));
    CHECK(wait_result.has_value());
    const auto latest = fixture.backing.connection().reader.read();
    REQUIRE(latest.has_value());
    CHECK((latest->sample.state.buttons & guide_button) != 0U);
    const auto guide_history =
        fixture.backing.runtime->read_history(fixture.public_readers[0U], fixture.backing.now);
    CHECK(std::ranges::any_of(std::span{guide_history.records}.first(guide_history.count),
        [](const history_record& record) {
            return record.origin == history_record_origin::callback_guide &&
                   (record.sample.state.buttons & guide_button) != 0U;
        }));
    CHECK(fixture.presentation->presentation().get_keystroke(xinput::flavor::v1_4, 0U, 0U, &key) ==
          ERROR_EMPTY);

    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = 501'101U,
    });
    fixture.pump();
    CHECK(fixture.presentation->presentation().get_keystroke(xinput::flavor::v1_4, 0U, 0U, &key) ==
          ERROR_SUCCESS);
    CHECK(key.VirtualKey == 0x5800U);
    CHECK((key.Flags & XINPUT_KEYSTROKE_REPEAT) != 0U);
}

TEST_CASE("gameinput.presentation.composition_retrieval_leaves_native_history_for_maintenance",
    "[integration][gameinput][presentation][events][probe]") {
    event_composition_fixture fixture;
    fixture.start();
    fixture.pump();

    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(south_button),
                .source_timestamp_us = 110U,
                .observation_qpc = 1'100U,
            },
    });
    fixture.script->enqueue(native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = 1'100U,
    });
    fixture.pump();

    const auto cursor_factories = fixture.script->cursor_factories.load(std::memory_order_relaxed);
    const auto cursor_destructions =
        fixture.script->cursor_destructions.load(std::memory_order_relaxed);
    XINPUT_KEYSTROKE key{};
    fixture.script->block_next();
    auto retrieval = std::async(std::launch::async, [&] {
        return fixture.presentation->presentation().get_keystroke(xinput::flavor::v1_4,
            0U,
            0U,
            &key);
    });
    const history_call_future_release_guard retrieval_cleanup{*fixture.script};

    REQUIRE(retrieval.wait_for(2s) == std::future_status::ready);
    CHECK(retrieval.get() == ERROR_SUCCESS);
    CHECK(key.VirtualKey == 0x5800U);
    CHECK((key.Flags & XINPUT_KEYSTROKE_KEYDOWN) != 0U);
    CHECK(fixture.script->cursor_factories.load(std::memory_order_relaxed) == cursor_factories);
    CHECK(
        fixture.script->cursor_destructions.load(std::memory_order_relaxed) == cursor_destructions);

    auto maintenance = std::async(std::launch::async,
        [&] { fixture.presentation->maintain_events(fixture.backing.now); });
    const history_call_future_release_guard maintenance_cleanup{*fixture.script};
    REQUIRE(fixture.script->wait_until_blocked(2s));
    CHECK(fixture.script->cursor_factories.load(std::memory_order_relaxed) == cursor_factories);
    CHECK(
        fixture.script->cursor_destructions.load(std::memory_order_relaxed) == cursor_destructions);

    fixture.script->release_blocked_call();
    REQUIRE(maintenance.wait_for(2s) == std::future_status::ready);
    maintenance.get();
    CHECK(fixture.script->cursor_factories.load(std::memory_order_relaxed) == cursor_factories);
    CHECK(
        fixture.script->cursor_destructions.load(std::memory_order_relaxed) == cursor_destructions);
}

TEST_CASE("gameinput.presentation.composition_cancels_a_real_guide_wait_ticket",
    "[integration][gameinput][presentation][events][concurrency]") {
    event_composition_fixture fixture;
    fixture.start();
    fixture.pump();

    const auto begun = fixture.presentation->presentation().begin_guide_wait(0U);
    REQUIRE(begun.has_value());
    std::optional<xinput::guide_wait_result<void>> waited;
    std::exception_ptr wait_exception;
    std::thread waiter([&] {
        try {
            waited.emplace(fixture.presentation->presentation().wait_for_guide_button(*begun));
        } catch (...) {
            wait_exception = std::current_exception();
        }
    });
    const guide_wait_join_guard join_waiter{fixture.presentation->presentation(),
        *fixture.presentation,
        *begun,
        waiter};

    const auto cancelled = fixture.presentation->presentation().cancel_guide_wait(*begun);
    REQUIRE(cancelled.has_value());
    CHECK(*cancelled);
    waiter.join();

    CHECK_FALSE(wait_exception);
    REQUIRE(waited.has_value());
    const auto wait_result =
        waited.value_or(std::unexpected(xinput::guide_wait_error::invalid_ticket));
    REQUIRE_FALSE(wait_result.has_value());
    CHECK(wait_result.error() == xinput::guide_wait_error::cancelled);
}

TEST_CASE("gameinput.presentation.event_diagnostic_closes_and_joins_its_guide_waiter",
    "[integration][gameinput][presentation][events][probe]") {
    event_composition_fixture fixture;
    fixture.start();
    fixture.pump();
    xinput_event_observer observer{*fixture.presentation};
    std::ostringstream output;

    observer.observe(fixture.backing.now, true, output);
    CHECK(output.str().contains("xinput_guide_begin user=0 result=admitted"));
    observer.stop();
    observer.report_shutdown(output);

    CHECK(output.str().contains("xinput_guide_cancel user=0 ticket="));
    CHECK(output.str().contains("result=true"));
    CHECK(output.str().contains("xinput_guide_wait user=0 ticket="));
    CHECK(output.str().contains("result=error error=cancelled"));
    CHECK(fixture.backing.runtime->stop(fixture.backing.now).drained);
}

TEST_CASE("gameinput.presentation.composition_retires_reserved_history_after_assignment_loss",
    "[integration][gameinput][presentation][events]") {
    event_composition_fixture fixture;
    fixture.start();
    fixture.pump();
    REQUIRE(fixture.script->cursor_factories.load(std::memory_order_relaxed) == 1U);

    fixture.backing.native->emit(fixture.device, native_device_event::disconnected);
    fixture.pump();

    const auto positions = fixture.presentation->presentation().positions();
    CHECK_FALSE(positions[0U].has_value());
    CHECK(fixture.backing.runtime->connections().size == 0U);
    CHECK(fixture.script->cursor_destructions.load(std::memory_order_relaxed) == 1U);
}

TEST_CASE("gameinput.presentation.composition_close_retains_a_blocked_native_reader_until_join",
    "[integration][gameinput][presentation][events][concurrency]") {
    event_composition_fixture fixture;
    fixture.start();
    fixture.pump();
    fixture.script->block_next();

    std::atomic<bool> pump_returned{};
    std::thread caller([&] {
        fixture.presentation->maintain_events(fixture.backing.now);
        pump_returned.store(true, std::memory_order_release);
    });
    const history_call_release_guard release_call{*fixture.script, caller};
    REQUIRE(fixture.script->wait_until_blocked(2s));

    fixture.presentation->close();
    const auto pending = fixture.backing.runtime->stop(fixture.backing.now);
    CHECK_FALSE(pending.drained);
    CHECK(pending.active_history_calls > 0U);
    CHECK(fixture.script->cursor_destructions.load(std::memory_order_relaxed) == 0U);

    fixture.script->release_blocked_call();
    caller.join();
    CHECK(pump_returned.load(std::memory_order_acquire));
    const auto drained = fixture.backing.runtime->stop(fixture.backing.now + 1s);
    CHECK(drained.drained);
    CHECK(fixture.script->cursor_destructions.load(std::memory_order_relaxed) == 1U);
}

} // namespace
} // namespace inputnext::gameinput_detail
