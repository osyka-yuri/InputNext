#pragma once

#include "focus/observer_test_access.hpp"
#include "support/focus/manual_clock.hpp"
#include "test_ports.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::xinput_test_support {

class manual_focus_source final : public focus::source {
public:
    [[nodiscard]] focus::process_instance process() const noexcept override {
        return {.pid = 812U, .creation_time_100ns = 9012U};
    }

    [[nodiscard]] focus::reading observe() noexcept override {
        {
            std::unique_lock lock{observation_mutex_};
            if (block_next_observation_) {
                block_next_observation_ = false;
                observation_entered_ = true;
                observation_changed_.notify_all();
                observation_changed_.wait(lock, [this] { return observation_released_; });
            }
        }
        return {.process = process(),
            .state = state_.load(std::memory_order_relaxed),
            .native_code = 0U};
    }

    void set(focus::evidence state) noexcept { state_.store(state, std::memory_order_relaxed); }

    void block_next_observation() noexcept {
        std::scoped_lock lock{observation_mutex_};
        block_next_observation_ = true;
        observation_entered_ = false;
        observation_released_ = false;
    }

    [[nodiscard]] bool wait_for_blocked_observation(std::chrono::milliseconds timeout) {
        std::unique_lock lock{observation_mutex_};
        return observation_changed_.wait_for(lock, timeout, [this] {
            return observation_entered_;
        });
    }

    void release_blocked_observation() noexcept {
        std::scoped_lock lock{observation_mutex_};
        observation_released_ = true;
        observation_changed_.notify_all();
    }

private:
    std::atomic<focus::evidence> state_{focus::evidence::foreground};
    std::mutex observation_mutex_;
    std::condition_variable observation_changed_;
    bool block_next_observation_{};
    bool observation_entered_{};
    bool observation_released_{};
};

class manual_focus_fixture final {
public:
    manual_focus_fixture()
        : source_(std::make_unique<manual_focus_source>()),
          source_view_(source_.get()),
          observer_(focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x81U),
              std::move(source_),
              &test::focus::manual_clock::read,
              &clock_)) {
        wait_for([](focus::snapshot value) {
            return value.state == focus::evidence::foreground && value.revision != 0U;
        });
    }

    manual_focus_fixture(const manual_focus_fixture&) = delete;
    manual_focus_fixture& operator=(const manual_focus_fixture&) = delete;
    manual_focus_fixture(manual_focus_fixture&&) = delete;
    manual_focus_fixture& operator=(manual_focus_fixture&&) = delete;

    ~manual_focus_fixture() {
        source_view_->release_blocked_observation();
        observer_->stop();
    }

    [[nodiscard]] focus::observer& observer() noexcept { return *observer_; }

    [[nodiscard]] focus::snapshot published() const { return observer_->published(); }

    [[nodiscard]] test::focus::manual_clock::time_point now() const noexcept {
        return clock_.now();
    }

    void advance(std::chrono::steady_clock::duration amount) noexcept { clock_.advance(amount); }

    void set(focus::evidence state) {
        const auto before = observer_->published();
        if (before.state == state) {
            return;
        }
        source_view_->set(state);
        wait_for([state, revision = before.revision](focus::snapshot value) {
            return value.state == state && value.revision > revision;
        });
    }

    void block_next_observation() noexcept { source_view_->block_next_observation(); }

    [[nodiscard]] bool wait_for_blocked_observation(std::chrono::milliseconds timeout) {
        return source_view_->wait_for_blocked_observation(timeout);
    }

    void release_blocked_observation() noexcept { source_view_->release_blocked_observation(); }

    void wait_for_refresh_after(std::uint64_t completed_observations) const {
        const auto expected_time = clock_.now();
        wait_for([completed_observations, expected_time](focus::snapshot value) {
            return value.state == focus::evidence::foreground &&
                   value.completed_observations > completed_observations &&
                   value.observation_started == expected_time;
        });
    }

private:
    template <typename Predicate>
    void wait_for(Predicate predicate) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate(observer_->published())) {
                return;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error{"manual focus observer did not publish expected evidence"};
    }

    test::focus::manual_clock clock_;
    std::unique_ptr<manual_focus_source> source_;
    manual_focus_source* source_view_{};
    std::unique_ptr<focus::observer> observer_;
};

struct event_value final {
    gamepad_state state{};
    std::uint64_t qpc{};
    bool guide_known{true};

    event_value(gamepad_state state_value,
        std::uint64_t qpc_value,
        bool is_guide_known = true) noexcept
        : state(state_value),
          qpc(qpc_value),
          guide_known(is_guide_known) {}
};

struct event_stream_id final {
    std::uint64_t value{};

    explicit event_stream_id(std::uint64_t input) noexcept : value(input) {}
};

[[nodiscard]] inline history_batch initial_batch(connection_target target,
    std::uint64_t stream,
    gamepad_state state = {},
    std::uint64_t qpc = 100U,
    bool guide_known = true) noexcept {
    history_batch batch{};
    batch.domain = history_domain{.target = target, .stream = stream};
    batch.source = history_source_kind::callback_capture;
    batch.reset = history_reset::initial_open;
    batch.count = 1U;
    batch.first_sequence = 1U;
    batch.last_sequence = 1U;
    batch.records[0U] = history_record{.sample = history_sample{.state = state,
                                           .source_timestamp_us = qpc,
                                           .effective_qpc = qpc,
                                           .guide_known = guide_known},
        .sequence = 1U,
        .baseline = true,
        .origin = history_record_origin::baseline};
    return batch;
}

[[nodiscard]] inline history_batch history_events(connection_target target,
    event_stream_id stream,
    std::uint64_t first_sequence,
    std::initializer_list<event_value> values,
    std::optional<history_failure> failure = std::nullopt,
    std::optional<std::uint64_t> checkpoint_qpc = std::nullopt) noexcept {
    history_batch batch{};
    batch.domain = history_domain{.target = target, .stream = stream.value};
    batch.source = history_source_kind::callback_capture;
    batch.first_sequence = first_sequence;
    batch.failure = failure;
    std::uint64_t sequence = first_sequence;
    for (const auto& value : values) {
        if (batch.count >= batch.records.size()) {
            break;
        }
        batch.records[batch.count++] = history_record{.sample = history_sample{.state = value.state,
                                                          .source_timestamp_us = value.qpc,
                                                          .effective_qpc = value.qpc,
                                                          .guide_known = value.guide_known},
            .sequence = sequence,
            .baseline = false,
            .origin = history_record_origin::callback_pad};
        batch.last_sequence = sequence;
        if (sequence != std::numeric_limits<std::uint64_t>::max()) {
            ++sequence;
        }
    }
    if (checkpoint_qpc.has_value() && !failure.has_value()) {
        batch.caught_up = true;
        batch.checkpoint_sequence = batch.count == 0U ? first_sequence - 1U : batch.last_sequence;
        batch.checkpoint_qpc = *checkpoint_qpc;
    }
    if (batch.count == 0U) {
        batch.first_sequence = 0U;
        batch.last_sequence = 0U;
    }
    return batch;
}

[[nodiscard]] inline history_batch empty_checkpoint(connection_target target,
    std::uint64_t stream,
    std::uint64_t sequence,
    std::uint64_t qpc) noexcept {
    return history_events(target, event_stream_id{stream}, sequence + 1U, {}, std::nullopt, qpc);
}

} // namespace inputnext::xinput_test_support
