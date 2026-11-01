#include "output/authority_fixture.hpp"

#include "executor_test_support.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::start_executor;
using inputnext::test::output::wait_until;

class mutable_focus_source final : public focus::source {
public:
    explicit mutable_focus_source(focus::process_instance process) noexcept : process_(process) {}

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        return {.process = process_, .state = state_.load(std::memory_order_acquire)};
    }

    void set(focus::evidence state) noexcept { state_.store(state, std::memory_order_release); }

private:
    focus::process_instance process_;
    std::atomic<focus::evidence> state_{focus::evidence::foreground};
};

class test_source_lifetime final : public source_lifetime {};

class retirement_callback_state final {
public:
    explicit retirement_callback_state(executor& executor_value) noexcept
        : executor_(executor_value) {}

    void set_backend(backend_handle backend) noexcept {
        {
            std::scoped_lock lock{mutex_};
            backend_ = backend;
            ready_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_callback(
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return callback_finished_; });
    }

    [[nodiscard]] std::optional<execution_result<retirement_receipt>> receipt() const {
        std::scoped_lock lock{mutex_};
        return receipt_;
    }

    [[nodiscard]] std::size_t destructions() const noexcept {
        std::scoped_lock lock{mutex_};
        return destructions_;
    }

private:
    friend class retirement_callback_sink;

    executor& executor_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    backend_handle backend_{};
    std::optional<execution_result<retirement_receipt>> receipt_;
    std::size_t destructions_{};
    bool ready_{};
    bool callback_finished_{};
};

class retirement_callback_sink final : public native_sink {
public:
    explicit retirement_callback_sink(std::shared_ptr<retirement_callback_state> state) noexcept
        : state_(std::move(state)) {}

    retirement_callback_sink(const retirement_callback_sink&) = delete;
    retirement_callback_sink& operator=(const retirement_callback_sink&) = delete;
    retirement_callback_sink(retirement_callback_sink&&) = delete;
    retirement_callback_sink& operator=(retirement_callback_sink&&) = delete;

    ~retirement_callback_sink() noexcept override {
        {
            std::scoped_lock lock{state_->mutex_};
            ++state_->destructions_;
        }
        state_->changed_.notify_all();
    }

    [[nodiscard]] native_result write(
        [[maybe_unused]] const native_command& command) noexcept override {
        backend_handle backend{};
        {
            std::unique_lock lock{state_->mutex_};
            state_->changed_.wait(lock, [this] { return state_->ready_; });
            backend = state_->backend_;
        }
        auto receipt = state_->executor_.retire(backend);
        {
            std::scoped_lock lock{state_->mutex_};
            state_->receipt_.emplace(std::move(receipt));
            state_->callback_finished_ = true;
        }
        state_->changed_.notify_all();
        return native_result{.status = native_status::accepted};
    }

private:
    std::shared_ptr<retirement_callback_state> state_;
};

[[nodiscard]] bool
has_rumble_call(const controlled_native_state& state, std::size_t first, rumble_value expected) {
    const auto count = state.call_count();
    for (std::size_t index = first; index < count; ++index) {
        if (state.call(index).output.values.rumble == expected) {
            return true;
        }
    }
    return false;
}

TEST_CASE("output.executor_request_drive_and_autonomous_ttl_expiry",
    "[component][output][executor][delivery]") {
    authority_fixture fixture{};
    auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    constexpr rumble_value active_rumble{.low_frequency = 0.65F, .high_frequency = 0.35F};
    const auto running = start_executor(fixture.owner);
    auto state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(state);
    const auto backend =
        running->register_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(state->wait_for_write_count(1U));

    const auto submitted = fixture.owner.submit(*context,
        inputnext::test::output::rumble_request({.channels = rumble_mask,
            .sequence = 1U,
            .ttl = std::chrono::milliseconds{350},
            .low = active_rumble.low_frequency,
            .high = active_rumble.high_frequency}));
    REQUIRE(submitted.has_value());
    running->request_drive();

    const bool active_delivered =
        wait_until([&] { return has_rumble_call(*state, 0U, active_rumble); });
    REQUIRE(active_delivered);
    const auto active_count = state->call_count();
    const bool expiry_delivered =
        wait_until([&] { return has_rumble_call(*state, active_count, rumble_value{}); },
            std::chrono::seconds{2});

    CHECK(expiry_delivered);
    running->shutdown();
}

TEST_CASE("output.executor_retires_foreground_bound_output_without_a_caller_hint",
    "[component][output][executor][delivery]") {
    runtime_id runtime{};
    runtime.bytes[0U] = std::byte{0x71U};
    runtime_id focus_runtime{};
    focus_runtime.bytes[0U] = std::byte{0x72U};
    device_registry registry{runtime};
    const focus::process_instance process{.pid = 73U, .creation_time_100ns = 811U};
    auto owned_focus_source = std::make_unique<mutable_focus_source>(process);
    auto* const focus_source_view = owned_focus_source.get();
    focus::observer focus_observer{focus_runtime, std::move(owned_focus_source)};
    output::authority owner{runtime, registry, focus_observer};
    const bool foreground_published = wait_until([&] {
        const auto publication = focus_observer.published();
        return publication.state == focus::evidence::foreground &&
               publication.completed_observations > 0U;
    });
    REQUIRE(foreground_published);

    std::unique_ptr<source_lifetime> source = std::make_unique<test_source_lifetime>();
    auto connection = registry.admit(source);
    REQUIRE(connection.has_value());
    auto context = owner.open(connection->target);
    REQUIRE(context.has_value());
    constexpr rumble_value active_rumble{.low_frequency = 0.55F, .high_frequency = 0.25F};
    const auto submitted = owner.submit(*context,
        inputnext::test::output::rumble_request({.channels = rumble_mask,
            .sequence = 1U,
            .ttl = std::chrono::seconds{5},
            .low = active_rumble.low_frequency,
            .high = active_rumble.high_frequency}));
    REQUIRE(submitted.has_value());

    const auto running = start_executor(owner);
    auto state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(state);
    const auto backend = running->register_backend(connection->target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(state->wait_for_write_count(1U));
    const bool active_delivered =
        wait_until([&] { return has_rumble_call(*state, 0U, active_rumble); });
    REQUIRE(active_delivered);
    const auto active_count = state->call_count();

    focus_source_view->set(focus::evidence::background);
    const bool background_published =
        wait_until([&] { return focus_observer.published().state == focus::evidence::background; });
    const bool neutral_delivered =
        wait_until([&] { return has_rumble_call(*state, active_count, rumble_value{}); });

    CHECK(background_published);
    CHECK(neutral_delivered);
    running->shutdown();
}

TEST_CASE("output.executor_retry_uses_a_new_attempt_for_the_same_publication",
    "[component][output][executor][delivery]") {
    authority_fixture fixture{};
    auto running = start_executor(fixture.owner);
    const auto state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(state);
    auto backend = running->register_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(state->wait_for_write_count(1U));

    const bool initial_attempt_completed = wait_until([&] {
        const auto progress = running->progress(*backend);
        return progress.has_value() && progress->last_completed_attempt.has_value() &&
               progress->delivery == delivery_state::setter_accepted;
    });
    REQUIRE(initial_attempt_completed);
    const auto first_progress = running->progress(*backend);
    REQUIRE(first_progress.has_value());
    if (!first_progress->last_completed_attempt.has_value()) {
        FAIL("initial write completed without an attempt result");
        return;
    }
    const auto first_completed = *first_progress->last_completed_attempt;

    const auto retry = running->request_retry(*backend);
    REQUIRE(retry.has_value());
    CHECK(state->wait_for_write_count(2U));
    const bool retry_completed = wait_until([&] {
        const auto progress = running->progress(*backend);
        return progress.has_value() && progress->last_completed_attempt.has_value() &&
               progress->last_completed_attempt->id != first_progress->last_completed_attempt->id;
    });
    REQUIRE(retry_completed);
    const auto second_progress = running->progress(*backend);
    REQUIRE(second_progress.has_value());
    if (!second_progress->last_completed_attempt.has_value()) {
        FAIL("retry write completed without an attempt result");
        return;
    }
    const auto second_completed = *second_progress->last_completed_attempt;
    CHECK(second_completed.publication == first_completed.publication);
    CHECK(second_completed.id != first_completed.id);
    CHECK(state->call(0U).output.values == state->call(1U).output.values);
    running->shutdown();
}

TEST_CASE("output.executor_allows_nonblocking_backend_retirement_from_native_callback",
    "[component][output][executor][delivery]") {
    authority_fixture fixture{};
    auto running = start_executor(fixture.owner);
    const auto state = std::make_shared<retirement_callback_state>(*running);
    std::unique_ptr<native_sink> candidate = std::make_unique<retirement_callback_sink>(state);
    auto backend = running->register_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    state->set_backend(*backend);

    REQUIRE(state->wait_for_callback());
    const auto receipt = state->receipt();
    if (!receipt.has_value() || !receipt.value().has_value()) {
        FAIL("native callback did not return a retirement receipt");
        return;
    }
    const auto retirement = receipt.value().value();
    const bool drained = wait_until([&] {
        const auto progress = running->progress(*backend);
        return progress.has_value() && progress->phase == backend_phase::drained;
    });
    REQUIRE(drained);
    CHECK(state->destructions() == 1U);
    CHECK(running->acknowledge_retirement(retirement).has_value());
    running->shutdown();
}

} // namespace
