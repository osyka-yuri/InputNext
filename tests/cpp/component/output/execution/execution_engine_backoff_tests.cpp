#include "output/authority_fixture.hpp"

#include "execution_engine_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

class release_write_guard final {
public:
    explicit release_write_guard(controlled_native_state& state) noexcept : state_(state) {}

    ~release_write_guard() noexcept { state_.release_write(); }

    release_write_guard(const release_write_guard&) = delete;
    release_write_guard& operator=(const release_write_guard&) = delete;
    release_write_guard(release_write_guard&&) = delete;
    release_write_guard& operator=(release_write_guard&&) = delete;

private:
    controlled_native_state& state_;
};

[[nodiscard]] inputnext::output::result<mutation_receipt> submit_rumble(authority_fixture& fixture,
    const public_context context,
    const std::uint64_t sequence,
    const float low,
    const float high) {
    return fixture.owner.submit(context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = sequence, .ttl = 5s, .low = low, .high = high}));
}

[[nodiscard]] logical_snapshot require_snapshot(authority_fixture& fixture) {
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    if (!snapshot.has_value()) {
        throw std::runtime_error{"Expected a live output snapshot"};
    }
    return *snapshot;
}

TEST_CASE("output.execution_backoff_grows_per_qualified_failure_and_caps",
    "[component][output][execution]") {
    constexpr std::array<std::chrono::milliseconds, 6U> delays{
        4ms,
        8ms,
        16ms,
        32ms,
        64ms,
        64ms,
    };

    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    for (std::size_t failure = 0U; failure < delays.size(); ++failure) {
        REQUIRE(execution_engine_test_access::set_backend_due(scope.engine(),
            *backend,
            execution_engine::clock::now()));
        sink_state->set_next_result(native_result{.status = native_status::retryable_failure});
        REQUIRE(scope.engine().drive_one(*lane));

        const auto progress = scope.engine().progress(*backend);
        REQUIRE(progress.has_value());
        CHECK(progress->delivery == delivery_state::retry_wait);
        CHECK(progress->consecutive_retryable_failures == static_cast<std::uint32_t>(failure + 1U));
        CHECK(progress->scheduled_retry_delay == delays[failure]);
        CHECK(sink_state->call_count() == failure + 1U);

        REQUIRE(execution_engine_test_access::set_backend_due(scope.engine(),
            *backend,
            execution_engine::clock::time_point::max()));
        CHECK(!scope.engine().drive_one(*lane));
        CHECK(sink_state->call_count() == failure + 1U);
        const auto waiting = scope.engine().progress(*backend);
        REQUIRE(waiting.has_value());
        CHECK(waiting->scheduled_retry_delay == delays[failure]);
    }

    REQUIRE(scope.engine().request_retry(*backend).has_value());
    const auto requested = scope.engine().progress(*backend);
    REQUIRE(requested.has_value());
    CHECK(requested->delivery == delivery_state::pending);
    CHECK(requested->consecutive_retryable_failures == static_cast<std::uint32_t>(delays.size()));
    CHECK(requested->retry_requested);
    CHECK(requested->scheduled_retry_delay == 0ms);
    REQUIRE(scope.engine().drive_one(*lane));
    const auto accepted = scope.engine().progress(*backend);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
}

TEST_CASE("output.execution_backoff_ignores_unrepresented_changes_and_resets_on_new_provenance",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    REQUIRE(submit_rumble(fixture, *context, 1U, 0.4F, 0.7F).has_value());

    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(scope.engine().publish(*backend, require_snapshot(fixture)).has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    sink_state->set_next_result(native_result{.status = native_status::retryable_failure});
    REQUIRE(scope.engine().drive_one(*lane));

    const auto failed = scope.engine().progress(*backend);
    REQUIRE(failed.has_value());
    CHECK(failed->observed_publication == publication_id{2U});
    CHECK(failed->consecutive_retryable_failures == 1U);
    CHECK(failed->scheduled_retry_delay == 4ms);
    REQUIRE(execution_engine_test_access::set_backend_due(scope.engine(),
        *backend,
        execution_engine::clock::time_point::max()));

    submit_request leds{};
    leds.selected_channels = leds_mask;
    leds.sequence = 2U;
    leds.ttl = 5s;
    leds.values.leds = led_value{.rgb = {1U, 2U, 3U}, .player_leds = 2U};
    REQUIRE(fixture.owner.submit(*context, leds).has_value());
    auto snapshot = require_snapshot(fixture);
    REQUIRE(scope.engine().publish(*backend, snapshot).has_value());
    const auto after_unrepresented_change = scope.engine().progress(*backend);
    REQUIRE(after_unrepresented_change.has_value());
    CHECK(after_unrepresented_change->observed_publication == publication_id{2U});
    CHECK(after_unrepresented_change->unsupported_channels == leds_mask);
    CHECK(after_unrepresented_change->consecutive_retryable_failures == 1U);
    CHECK(after_unrepresented_change->scheduled_retry_delay == 4ms);

    REQUIRE(submit_rumble(fixture, *context, 3U, 0.4F, 0.7F).has_value());
    snapshot = require_snapshot(fixture);
    REQUIRE(scope.engine().publish(*backend, snapshot).has_value());
    const auto after_new_provenance = scope.engine().progress(*backend);
    REQUIRE(after_new_provenance.has_value());
    CHECK(after_new_provenance->observed_publication == publication_id{3U});
    CHECK(after_new_provenance->consecutive_retryable_failures == 0U);
    CHECK(after_new_provenance->delivery == delivery_state::pending);
    CHECK(after_new_provenance->scheduled_retry_delay == 0ms);

    REQUIRE(scope.engine().drive_one(*lane));
    const auto accepted = scope.engine().progress(*backend);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
    CHECK(accepted->scheduled_retry_delay == 0ms);
}

TEST_CASE("output.execution_backoff_reports_only_an_active_automatic_retry",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    sink_state->set_next_result(native_result{.status = native_status::retryable_failure});
    REQUIRE(scope.engine().drive_one(*lane));

    REQUIRE(execution_engine_test_access::set_backend_due(scope.engine(),
        *backend,
        execution_engine::clock::time_point::max()));
    auto waiting = scope.engine().progress(*backend);
    REQUIRE(waiting.has_value());
    CHECK(waiting->scheduled_retry_delay == 4ms);

    REQUIRE(scope.engine().request_retry(*backend).has_value());
    auto requested = scope.engine().progress(*backend);
    REQUIRE(requested.has_value());
    CHECK(requested->delivery == delivery_state::pending);
    CHECK(requested->retry_requested);
    CHECK(requested->consecutive_retryable_failures == 1U);
    CHECK(requested->scheduled_retry_delay == 0ms);

    REQUIRE(scope.engine().drive_one(*lane));
    const auto accepted = scope.engine().progress(*backend);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
    CHECK(accepted->scheduled_retry_delay == 0ms);
}

TEST_CASE("output.execution_backoff_resets_after_permanent_results_and_blocks_that_publication",
    "[component][output][execution]") {
    for (const auto status : {native_status::unsupported, native_status::failed}) {
        authority_fixture fixture{};
        execution_engine_scope scope{fixture.owner};
        const auto sink_state = std::make_shared<controlled_native_state>();
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        const auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        REQUIRE(
            execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 4U));
        sink_state->set_next_result(native_result{.status = status});

        REQUIRE(scope.engine().drive_one(*lane));
        auto blocked = scope.engine().progress(*backend);
        REQUIRE(blocked.has_value());
        CHECK(blocked->delivery == delivery_state::blocked);
        CHECK(blocked->consecutive_retryable_failures == 0U);
        CHECK(blocked->scheduled_retry_delay == 0ms);
        CHECK(!scope.engine().drive_one(*lane));
        CHECK(sink_state->call_count() == 1U);

        const auto context = fixture.owner.open(fixture.connection.target);
        REQUIRE(context.has_value());
        REQUIRE(submit_rumble(fixture, *context, 1U, 0.2F, 0.5F).has_value());
        REQUIRE(scope.engine().publish(*backend, require_snapshot(fixture)).has_value());
        const auto pending = scope.engine().progress(*backend);
        REQUIRE(pending.has_value());
        CHECK(pending->observed_publication == publication_id{2U});
        CHECK(pending->delivery == delivery_state::pending);
        CHECK(pending->consecutive_retryable_failures == 0U);

        REQUIRE(scope.engine().drive_one(*lane));
        const auto accepted = scope.engine().progress(*backend);
        REQUIRE(accepted.has_value());
        CHECK(accepted->delivery == delivery_state::setter_accepted);
        CHECK(accepted->consecutive_retryable_failures == 0U);
        CHECK(sink_state->call_count() == 2U);
    }
}

TEST_CASE("output.execution_backoff_stale_completion_cannot_change_new_publication_cycle",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    sink_state->set_next_result(native_result{.status = native_status::retryable_failure});
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    std::jthread writer(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const release_write_guard release{*sink_state};
    const bool entered = sink_state->wait_for_blocked_write();
    bool published = false;
    if (entered) {
        const auto context = fixture.owner.open(fixture.connection.target);
        if (context.has_value() && submit_rumble(fixture, *context, 1U, 0.3F, 0.6F).has_value()) {
            published = scope.engine().publish(*backend, require_snapshot(fixture)).has_value();
        }
    }
    sink_state->release_write();
    writer.join();

    CHECK(entered);
    CHECK(published);
    const auto progress = scope.engine().progress(*backend);
    REQUIRE(progress.has_value());
    CHECK(progress->observed_publication == publication_id{2U});
    CHECK(progress->consecutive_retryable_failures == 0U);
    CHECK(progress->delivery == delivery_state::pending);
    CHECK(progress->scheduled_retry_delay == 0ms);
    REQUIRE(progress->last_completed_attempt.has_value());
    const auto completed = progress->last_completed_attempt.value_or(completed_attempt{});
    CHECK(completed.publication == publication_id{1U});
    CHECK(completed.result.status == native_status::retryable_failure);
}

TEST_CASE("output.execution_backoff_cycle_is_isolated_by_backend_replacement",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto first_state = std::make_shared<controlled_native_state>();
    first_state->block_next_write();
    first_state->set_next_result(native_result{.status = native_status::retryable_failure});
    std::unique_ptr<native_sink> first_sink = std::make_unique<controlled_native_sink>(first_state);
    const auto first = scope.register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first.has_value());
    const auto lane = scope.engine().lane_for(*first);
    REQUIRE(lane.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(), *first, 2U));

    std::jthread writer(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const release_write_guard release{*first_state};
    const bool entered = first_state->wait_for_blocked_write();
    const auto replacement_state = std::make_shared<controlled_native_state>();
    std::optional<backend_handle> replacement;
    if (entered) {
        std::unique_ptr<native_sink> replacement_sink =
            std::make_unique<controlled_native_sink>(replacement_state);
        const auto committed = scope.register_backend(fixture.connection.target,
            replacement_sink,
            rumble_mask,
            *first);
        if (committed.has_value()) {
            replacement = *committed;
        }
    }
    first_state->release_write();
    writer.join();

    CHECK(entered);
    REQUIRE(replacement.has_value());
    const auto replacement_handle = replacement.value_or(backend_handle{});
    const auto old_progress = scope.engine().progress(*first);
    const auto current_progress = scope.engine().progress(replacement_handle);
    REQUIRE(old_progress.has_value());
    REQUIRE(current_progress.has_value());
    CHECK(old_progress->phase == backend_phase::retiring);
    CHECK(old_progress->consecutive_retryable_failures == 2U);
    CHECK(current_progress->phase == backend_phase::current);
    CHECK(current_progress->consecutive_retryable_failures == 0U);
    CHECK(current_progress->delivery == delivery_state::pending);

    const auto receipt = scope.engine().retire(*first);
    REQUIRE(receipt.has_value());
    REQUIRE(scope.engine().drive_one(*lane));
    CHECK(replacement_state->call_count() == 0U);
    REQUIRE(scope.engine().drive_one(*lane));
    const auto accepted = scope.engine().progress(replacement_handle);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
    REQUIRE(scope.engine().acknowledge_retirement(*receipt).has_value());
}

TEST_CASE("output.execution_backoff_counter_saturates_without_blocking_retirement",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(),
        *backend,
        std::numeric_limits<std::uint32_t>::max()));
    sink_state->set_next_result(native_result{.status = native_status::retryable_failure});

    REQUIRE(scope.engine().drive_one(*lane));
    const auto failed = scope.engine().progress(*backend);
    REQUIRE(failed.has_value());
    CHECK(failed->consecutive_retryable_failures == std::numeric_limits<std::uint32_t>::max());
    CHECK(failed->scheduled_retry_delay == 64ms);

    const auto receipt = scope.engine().retire(*backend);
    REQUIRE(receipt.has_value());
    REQUIRE(scope.engine().drive_one(*lane));
    const auto drained = scope.engine().progress(*backend);
    REQUIRE(drained.has_value());
    CHECK(drained->phase == backend_phase::drained);
    CHECK(drained->consecutive_retryable_failures == std::numeric_limits<std::uint32_t>::max());
    REQUIRE(scope.engine().acknowledge_retirement(*receipt).has_value());
    CHECK(sink_state->destruction_count() == 1U);
}

} // namespace
