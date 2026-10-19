#include "gameinput/core/publication_source.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/reading_parts.hpp"
#include "gameinput/runtime/factory.hpp"
#include "runtime_history_test_support.hpp"
#include "runtime_test_support.hpp"
#include "support/history/history_store_test_access.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::gameinput_test_support;

class test_pause final {
public:
    static void wait(void* context) noexcept {
        auto& pause = *static_cast<test_pause*>(context);
        pause.entered_.release();
        pause.release_.acquire();
    }

    [[nodiscard]] bool wait_until_entered() { return entered_.try_acquire_for(query_wait); }

    void release() noexcept {
        if (!released_) {
            released_ = true;
            release_.release();
        }
    }

private:
    std::binary_semaphore entered_{0};
    std::binary_semaphore release_{0};
    bool released_{};
};

class pause_release_guard final {
public:
    explicit pause_release_guard(test_pause& pause) noexcept : pause_(pause) {}

    ~pause_release_guard() { pause_.release(); }

    pause_release_guard(const pause_release_guard&) = delete;
    pause_release_guard& operator=(const pause_release_guard&) = delete;
    pause_release_guard(pause_release_guard&&) = delete;
    pause_release_guard& operator=(pause_release_guard&&) = delete;

private:
    test_pause& pause_;
};

class history_call_release_guard final {
public:
    explicit history_call_release_guard(native_history_script& script) noexcept : script_(script) {}

    ~history_call_release_guard() { script_.release_blocked_call(); }

    history_call_release_guard(const history_call_release_guard&) = delete;
    history_call_release_guard& operator=(const history_call_release_guard&) = delete;
    history_call_release_guard(history_call_release_guard&&) = delete;
    history_call_release_guard& operator=(history_call_release_guard&&) = delete;

private:
    native_history_script& script_;
};

class thread_join_guard final {
public:
    explicit thread_join_guard(std::thread& thread) noexcept : thread_(thread) {}

    ~thread_join_guard() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    thread_join_guard(const thread_join_guard&) = delete;
    thread_join_guard& operator=(const thread_join_guard&) = delete;
    thread_join_guard(thread_join_guard&&) = delete;
    thread_join_guard& operator=(thread_join_guard&&) = delete;

private:
    std::thread& thread_;
};

[[nodiscard]] connection_target start_history_device(runtime_fixture& fixture,
    const std::shared_ptr<native_history_script>& script) {
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->configure_history(script);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(2U), 100U, 1'000U));
    fixture.native->set_current(device,
        native_reading_lane::system_button,
        reading_parts{.guide = false, .source_timestamp_us = 100U, .observation_qpc = 1'000U});

    REQUIRE(fixture.start().has_value());
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    return fixture.connection().target;
}

void set_read_commit_hook(gameinput_runtime& runtime, test_pause* pause) {
    history_detail::history_store_test_access::set_before_read_commit_test_hook(
        runtime_factory::history_for_test(runtime),
        pause == nullptr ? nullptr : &test_pause::wait,
        pause);
}

} // namespace

TEST_CASE("gameinput.runtime.history_returns_native_baseline_and_closes_reader",
    "[integration][gameinput][history]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    script->seed.baseline = input_update{
        .state = test_pad(4U),
        .source_timestamp_us = 120U,
        .observation_qpc = 1'200U,
    };
    const auto target = start_history_device(fixture, script);

    auto reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(reader.has_value());

    const auto batch = fixture.runtime->read_history(*reader, fixture.now);
    REQUIRE(batch.count == 1U);
    CHECK(batch.domain.target == target);
    CHECK(batch.source == history_source_kind::native_history);
    CHECK(batch.reset == history_reset::initial_open);
    CHECK(batch.records[0U].baseline);
    CHECK(batch.records[0U].sample.state.buttons == 4U);

    CHECK(fixture.runtime->close_history(*reader).has_value());
    CHECK(fixture.runtime->stop(fixture.now).drained);
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == 1U);
    CHECK(script->cursor_destructions.load(std::memory_order_relaxed) == 1U);
}

TEST_CASE("gameinput.runtime.history_keeps_checkpoint_unqualified_after_activation_cut",
    "[integration][gameinput][history][coverage]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->configure_history(script);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(1U), 100U, 1'000U));
    fixture.native->set_current(device,
        native_reading_lane::system_button,
        reading_parts{.guide = false, .source_timestamp_us = 100U, .observation_qpc = 1'000U});
    REQUIRE(fixture.start().has_value());
    fixture.native->callback_on_next_reading_registration(
        reading_parts{.guide = true, .source_timestamp_us = 110U, .observation_qpc = 1'100U});
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    const auto latest = fixture.connection().reader.read();
    REQUIRE(latest.has_value());
    CHECK((latest->sample.state.buttons & static_cast<button_mask>(gamepad_button::guide)) != 0U);

    const auto target = fixture.connection().target;
    auto reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(reader.has_value());
    const auto batch = fixture.runtime->read_history(*reader, fixture.now);
    REQUIRE(batch.count == 1U);
    CHECK(batch.records[0U].baseline);
    CHECK_FALSE(batch.records[0U].sample.guide_known);
    CHECK((batch.records[0U].sample.state.buttons &
              static_cast<button_mask>(gamepad_button::guide)) == 0U);
    CHECK_FALSE(batch.caught_up);
    CHECK(batch.checkpoint_qpc == 0U);
    CHECK_FALSE(batch.failure.has_value());

    CHECK(fixture.runtime->stop(fixture.now).drained);
}

TEST_CASE("gameinput.runtime.history_orders_polling_seed_cut_without_qualifying_pad_clock",
    "[integration][gameinput][history][coverage]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    script->seed.baseline = input_update{
        .state = test_pad(4U),
        .source_timestamp_us = 120U,
        .observation_qpc = 1'200U,
    };
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->configure_history(script);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(2U), 100U, 1'000U));
    fixture.native->fail_next_reading_registrations(2U);

    REQUIRE(fixture.start().has_value());
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.connection().health == connection_health::polling_fallback);
    CHECK(fixture.connection().pad_known);
    CHECK_FALSE(fixture.connection().guide_known);
    CHECK(fixture.native->current_calls == 2U);

    auto reader = fixture.runtime->open_history(fixture.connection().target, fixture.now);
    REQUIRE(reader.has_value());
    const auto baseline = fixture.runtime->read_history(*reader, fixture.now);
    REQUIRE(baseline.count == 1U);
    CHECK(baseline.records[0U].baseline);
    CHECK_FALSE(baseline.records[0U].sample.guide_known);

    const auto uncovered = fixture.runtime->read_history(*reader, fixture.now);
    CHECK(uncovered.count == 0U);
    CHECK_FALSE(uncovered.failure.has_value());
    CHECK_FALSE(uncovered.caught_up);
    CHECK(uncovered.checkpoint_qpc == 0U);

    const auto retry_time = fixture.now + std::chrono::seconds{1};
    fixture.runtime->maintain(retry_time);
    REQUIRE(fixture.native->reading_registration_attempts == 2U);
    CHECK(fixture.connection().health == connection_health::polling_fallback);
    CHECK_FALSE(fixture.connection().guide_known);

    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(8U), 130U, 1'300U));
    fixture.native->set_current(device,
        native_reading_lane::system_button,
        reading_parts{.guide = true, .source_timestamp_us = 130U, .observation_qpc = 1'300U});
    script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(16U),
                .source_timestamp_us = 125U,
                .observation_qpc = 1'250U,
            },
    });
    script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(32U),
                .source_timestamp_us = 140U,
                .observation_qpc = 1'400U,
            },
    });
    const auto poll_time = retry_time + std::chrono::milliseconds{4};
    fixture.runtime->maintain(poll_time);

    const auto latest = fixture.connection();
    CHECK(latest.health == connection_health::polling_fallback);
    CHECK(latest.pad_known);
    CHECK(latest.guide_known);
    const auto guide_mask = static_cast<button_mask>(gamepad_button::guide);
    const auto latest_state = latest.reader.read();
    REQUIRE(latest_state.has_value());
    CHECK((latest_state->sample.state.buttons & guide_mask) != 0U);

    const auto cut = fixture.runtime->read_history(*reader, poll_time);
    REQUIRE(cut.count == 1U);
    CHECK_FALSE(cut.records[0U].baseline);
    CHECK(cut.records[0U].sequence == 2U);
    CHECK(cut.records[0U].sample.state.buttons == 16U);
    CHECK_FALSE(cut.records[0U].sample.guide_known);
    REQUIRE(cut.failure.has_value());
    CHECK(cut.failure
              .value_or(history_failure{
                  .code = history_error::provider_failure,
              })
              .code == history_error::history_lost);
    CHECK_FALSE(cut.caught_up);
    CHECK(cut.checkpoint_qpc == 0U);

    const auto recovery = fixture.runtime->read_history(*reader, poll_time);
    REQUIRE(recovery.count == 1U);
    CHECK(recovery.reset == history_reset::history_gap);
    CHECK(recovery.loss == history_loss::unknown);
    CHECK(recovery.records[0U].baseline);
    CHECK(recovery.records[0U].sample.guide_known);
    CHECK((recovery.records[0U].sample.state.buttons & guide_mask) != 0U);
    CHECK(recovery.records[0U].sample.effective_qpc == 1'300U);
    CHECK_FALSE(recovery.failure.has_value());

    const auto still_uncovered = fixture.runtime->read_history(*reader, poll_time);
    CHECK(still_uncovered.count == 0U);
    CHECK_FALSE(still_uncovered.failure.has_value());
    CHECK_FALSE(still_uncovered.caught_up);
    CHECK(still_uncovered.checkpoint_qpc == 0U);

    CHECK(fixture.runtime->stop(poll_time).drained);
}

TEST_CASE("gameinput.runtime.history_open_call_is_counted_before_source_lease",
    "[integration][gameinput][history][concurrency]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    const auto target = start_history_device(fixture, script);
    test_pause pause;
    runtime_factory::set_before_history_open_test_hook(*fixture.runtime, &test_pause::wait, &pause);

    std::optional<history_result<history_reader_handle>> opened;
    std::thread opener([&] { opened.emplace(fixture.runtime->open_history(target, fixture.now)); });
    const thread_join_guard join_on_exit{opener};
    const pause_release_guard release_on_exit{pause};
    REQUIRE(pause.wait_until_entered());

    const auto stopping = fixture.runtime->stop(fixture.now);
    CHECK_FALSE(stopping.drained);
    CHECK(stopping.active_history_calls == 1U);
    CHECK(stopping.pending_history_owners == 0U);
    CHECK(fixture.native->released_devices.load(std::memory_order_relaxed) == 1U);
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == 0U);

    pause.release();
    opener.join();
    runtime_factory::set_before_history_open_test_hook(*fixture.runtime, nullptr, nullptr);
    if (!opened.has_value()) {
        FAIL("History open did not return");
        return;
    }
    if (opened->has_value()) {
        FAIL("History open unexpectedly succeeded after stop");
        return;
    }
    CHECK(opened->error().code == history_error::closed);
    CHECK(fixture.runtime->stop(fixture.now).drained);
}

TEST_CASE("gameinput.runtime.history_stop_waits_for_blocked_native_call_and_reclaims_lease",
    "[integration][gameinput][history][concurrency]") {
    runtime_fixture fixture;
    const auto first_script = std::make_shared<native_history_script>();
    const auto target = start_history_device(fixture, first_script);
    auto first_reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(first_reader.has_value());
    const auto baseline = fixture.runtime->read_history(*first_reader, fixture.now);
    REQUIRE(baseline.count == 1U);

    const auto second_script = std::make_shared<native_history_script>();
    second_script->seed.baseline = input_update{
        .state = test_pad(4U),
        .source_timestamp_us = 120U,
        .observation_qpc = 1'200U,
    };
    second_script->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(16U),
                .source_timestamp_us = 130U,
                .observation_qpc = 1'300U,
            },
    });
    const auto second_device = handle(fixture.device_memory[1U]);
    fixture.native->configure_history(second_device, second_script);
    fixture.native->set_current(second_device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(4U), 120U, 1'200U));
    fixture.native->set_current(second_device,
        native_reading_lane::system_button,
        reading_parts{.guide = false, .source_timestamp_us = 120U, .observation_qpc = 1'200U});
    fixture.connect(second_device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 2U);
    const auto second_target = fixture.connection(1U).target;
    auto second_reader = fixture.runtime->open_history(second_target, fixture.now);
    REQUIRE(second_reader.has_value());
    const auto second_baseline = fixture.runtime->read_history(*second_reader, fixture.now);
    REQUIRE(second_baseline.count == 1U);
    CHECK(second_baseline.records[0U].baseline);

    first_script->block_next();
    std::optional<history_batch> read;
    std::thread reader_thread(
        [&] { read.emplace(fixture.runtime->read_history(*first_reader, fixture.now)); });
    const thread_join_guard join_on_exit{reader_thread};
    const history_call_release_guard release_on_exit{*first_script};
    REQUIRE(first_script->wait_until_blocked(query_wait));

    fixture.native->publish(second_device, pad_parts(test_pad(8U), 125U, 1'250U));
    const auto latest = fixture.registry.read(second_target);
    REQUIRE(latest.has_value());
    CHECK(latest->sample.state.buttons == 8U);

    const auto second_progress = fixture.runtime->read_history(*second_reader, fixture.now);
    CHECK(second_progress.count == 1U);
    if (second_progress.count >= 1U) {
        CHECK_FALSE(second_progress.records[0U].baseline);
        CHECK(second_progress.records[0U].sample.state.buttons == 16U);
        CHECK(second_progress.records[0U].sample.effective_qpc == 1'300U);
    }
    CHECK_FALSE(second_progress.failure.has_value());
    CHECK(second_progress.caught_up);
    CHECK(second_progress.checkpoint_qpc == 1'300U);

    const auto stopping = fixture.runtime->stop(fixture.now);
    CHECK_FALSE(stopping.drained);
    CHECK(stopping.active_history_calls == 1U);
    CHECK(stopping.pending_history_owners == 1U);
    CHECK(fixture.native->released_devices.load(std::memory_order_relaxed) == 1U);
    CHECK(first_script->cursor_destructions.load(std::memory_order_relaxed) == 0U);
    CHECK(second_script->cursor_destructions.load(std::memory_order_relaxed) == 1U);

    first_script->release_blocked_call();
    reader_thread.join();
    if (!read.has_value()) {
        FAIL("Blocked history read did not return");
        return;
    }
    CHECK(read->count == 0U);
    if (!read->failure.has_value()) {
        FAIL("Stopped history read did not report failure");
        return;
    }
    CHECK(read->failure->code == history_error::closed);

    const auto drained = fixture.runtime->stop(fixture.now);
    CHECK(drained.drained);
    CHECK(drained.active_history_calls == 0U);
    CHECK(drained.pending_history_owners == 0U);
    CHECK(first_script->cursor_destructions.load(std::memory_order_relaxed) == 1U);
    CHECK(second_script->cursor_destructions.load(std::memory_order_relaxed) == 1U);
    CHECK(fixture.native->released_devices.load(std::memory_order_relaxed) == 2U);
}

TEST_CASE("gameinput.runtime.history_commit_linearizes_at_exact_registry_validation",
    "[integration][gameinput][history][concurrency]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    const auto target = start_history_device(fixture, script);
    auto reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(reader.has_value());

    test_pause pause;
    set_read_commit_hook(*fixture.runtime, &pause);
    std::optional<history_batch> read;
    std::thread reader_thread(
        [&] { read.emplace(fixture.runtime->read_history(*reader, fixture.now)); });
    const thread_join_guard join_on_exit{reader_thread};
    const pause_release_guard release_on_exit{pause};
    REQUIRE(pause.wait_until_entered());

    std::unique_ptr<source_lifetime> replacement{std::make_unique<publication_source>()};
    const auto replaced = fixture.registry.replace(target, replacement);
    REQUIRE(replaced.has_value());

    pause.release();
    reader_thread.join();
    set_read_commit_hook(*fixture.runtime, nullptr);
    if (!read.has_value()) {
        FAIL("Paused history read did not return");
        return;
    }
    CHECK(read->count == 1U);
    CHECK(read->records[0U].baseline);

    const auto stale = fixture.runtime->read_history(*reader, fixture.now);
    if (!stale.failure.has_value()) {
        FAIL("Replacement was not observed by the next history read");
        return;
    }
    CHECK(stale.failure->code == history_error::disconnected);
    fixture.maintain();
    CHECK(fixture.runtime->stop(fixture.now).drained);
}

TEST_CASE("gameinput.runtime.history_stop_cancels_read_after_final_validation",
    "[integration][gameinput][history][concurrency]") {
    runtime_fixture fixture;
    const auto script = std::make_shared<native_history_script>();
    const auto target = start_history_device(fixture, script);
    auto reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(reader.has_value());

    test_pause pause;
    set_read_commit_hook(*fixture.runtime, &pause);
    std::optional<history_batch> read;
    std::thread reader_thread(
        [&] { read.emplace(fixture.runtime->read_history(*reader, fixture.now)); });
    const thread_join_guard join_on_exit{reader_thread};
    const pause_release_guard release_on_exit{pause};
    REQUIRE(pause.wait_until_entered());

    const auto stopping = fixture.runtime->stop(fixture.now);
    CHECK_FALSE(stopping.drained);
    CHECK(stopping.active_history_calls == 1U);
    CHECK(stopping.pending_history_owners == 1U);

    pause.release();
    reader_thread.join();
    set_read_commit_hook(*fixture.runtime, nullptr);
    if (!read.has_value()) {
        FAIL("Stopped history read did not return");
        return;
    }
    CHECK(read->count == 0U);
    if (!read->failure.has_value()) {
        FAIL("Stopped history read did not report failure");
        return;
    }
    CHECK(read->failure->code == history_error::closed);
    CHECK(fixture.runtime->stop(fixture.now).drained);
}
