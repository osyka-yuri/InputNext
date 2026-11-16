#include "output/authority/execution.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/composition/window_acquisition.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_submission.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::output::detail::authority_execution_access;
using inputnext::output::detail::authority_test_access;
using inputnext::output::detail::composition;
using inputnext::output::detail::internal_producer;
using inputnext::output::detail::project_representation;
using inputnext::output::detail::window_acquisition;
using output_error = inputnext::output::error;

[[nodiscard]] runtime_id test_runtime(std::byte first) noexcept {
    runtime_id result{};
    result.bytes[0U] = first;
    return result;
}

class mutable_window_source final : public focus::source {
public:
    explicit mutable_window_source(focus::process_instance process) noexcept : process_(process) {}

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        std::scoped_lock lock{mutex_};
        return focus::reading{.process = process_,
            .state = state_,
            .foreground_window = window_,
            .window_transition = transition_,
            .window_available = true};
    }

    void set_window(focus::window_id window) noexcept {
        std::scoped_lock lock{mutex_};
        window_ = window;
        ++transition_;
    }

    void set_state(focus::evidence state) noexcept {
        std::scoped_lock lock{mutex_};
        state_ = state;
    }

private:
    const focus::process_instance process_;
    std::mutex mutex_;
    focus::window_id window_{101U};
    std::uint64_t transition_{};
    focus::evidence state_{focus::evidence::foreground};
};

class registry_source final : public source_lifetime {};

class window_fixture final {
public:
    window_fixture()
        : runtime(test_runtime(std::byte{0x72U})),
          registry(runtime),
          process{.pid = 241U, .creation_time_100ns = 123456U},
          owned_focus_source(std::make_unique<mutable_window_source>(process)),
          focus_source(owned_focus_source.get()),
          focus_observer(test_runtime(std::byte{0x73U}), std::move(owned_focus_source)),
          trusted(runtime, registry, focus_observer),
          owner(trusted.public_authority()) {
        auto source = std::unique_ptr<source_lifetime>{std::make_unique<registry_source>()};
        const auto admitted = registry.admit(source);
        if (!admitted.has_value()) {
            throw std::runtime_error{"window authority fixture could not admit a connection"};
        }
        connection = *admitted;
        if (!wait_for_window(101U)) {
            throw std::runtime_error{"window authority fixture did not publish its initial window"};
        }
    }

    window_fixture(const window_fixture&) = delete;
    window_fixture& operator=(const window_fixture&) = delete;
    window_fixture(window_fixture&&) = delete;
    window_fixture& operator=(window_fixture&&) = delete;
    ~window_fixture() = default;

    [[nodiscard]] admitted_connection admit_connection() {
        auto source = std::unique_ptr<source_lifetime>{std::make_unique<registry_source>()};
        const auto admitted = registry.admit(source);
        if (!admitted.has_value()) {
            throw std::runtime_error{"window authority fixture could not admit another connection"};
        }
        return *admitted;
    }

    [[nodiscard]] bool wait_for_window(focus::window_id expected) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto guard = focus_observer.lock_presentation();
            const auto current = guard.current_foreground_window_epoch(expected);
            if (current.has_value()) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] bool wait_for_state(focus::evidence expected) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (focus_observer.published().state == expected) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] focus::window_epoch current_window(focus::window_id expected) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto guard = focus_observer.lock_presentation();
            const auto current = guard.current_foreground_window_epoch(expected);
            if (current.has_value()) {
                return *current;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error{"window authority fixture did not observe the requested window"};
    }

    [[nodiscard]] std::shared_ptr<const window_acquisition> acquire_window(
        focus::window_id expected) const {
        return std::make_shared<const window_acquisition>(connection.target,
            current_window(expected));
    }

    runtime_id runtime;
    device_registry registry;
    focus::process_instance process;
    std::unique_ptr<mutable_window_source> owned_focus_source;
    mutable_window_source* focus_source{};
    focus::observer focus_observer;
    composition trusted;
    authority& owner;
    admitted_connection connection;
};

[[nodiscard]] internal_producer make_process_producer(composition& trusted,
    std::uint64_t owner_key,
    internal_priority priority = internal_priority::game_profile) {
    const internal_context_options options{.owner = internal_owner_key{owner_key},
        .priority = priority,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::foreground_bound};
    auto producer = trusted.make_producer(options);
    if (!producer.has_value()) {
        throw std::runtime_error{"window authority fixture could not create a process grant"};
    }
    return std::move(*producer);
}

[[nodiscard]] backend_handle
make_backend(connection_target target, execution_owner owner, std::uint64_t id) noexcept {
    return backend_handle{.target = target, .incarnation = {.owner = owner, .id = backend_id{id}}};
}

[[nodiscard]] output::detail::represented_projection require_rumble_projection(authority& owner,
    connection_target target,
    context_identity expected_owner) {
    const auto snapshot = owner.read_snapshot(target);
    REQUIRE(snapshot.has_value());
    const auto value = snapshot.value_or(logical_snapshot{});
    REQUIRE(value.rumble.has_value());
    CHECK(value.rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          expected_owner);
    const auto projection = project_representation(value, rumble_mask);
    REQUIRE(projection.has_value());
    return projection.value_or(output::detail::represented_projection{});
}

void require_record_open(authority& owner, context_identity identity, bool expected) {
    const auto record = authority_test_access::observe_record(owner, identity);
    REQUIRE(record.has_value());
    CHECK(record.value_or(authority_test_access::record_observation{}).open == expected);
}

[[nodiscard]] submit_request finite_rumble_request(std::uint64_t sequence, float low, float high) {
    return inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = sequence, .ttl = 1s, .low = low, .high = high});
}

[[nodiscard]] finite_submission_window make_window(std::chrono::steady_clock::time_point entered) {
    const auto value = make_finite_submission_window(entered, 1s);
    if (!value.has_value()) {
        throw std::runtime_error{"window authority fixture could not make a finite window"};
    }
    return *value;
}

[[nodiscard]] finite_submission_window make_expired_window() {
    const auto value = make_finite_submission_window(std::chrono::steady_clock::now() - 2s, 1s);
    if (!value.has_value()) {
        throw std::runtime_error{"window authority fixture could not make an expired window"};
    }
    return *value;
}

TEST_CASE("output.window_rumble_is_bound_to_exact_target_and_rumble_channel",
    "[component][output][dinput]") {
    window_fixture fixture;
    const auto token = fixture.acquire_window(101U);
    auto created = fixture.trusted.make_window_rumble(token);
    REQUIRE(created.has_value());
    auto producer = std::move(*created);

    const auto other_connection = fixture.admit_connection();
    const auto wrong_target = producer.open(other_connection.target);
    REQUIRE_FALSE(wrong_target.has_value());
    CHECK(wrong_target.error() == output_error::not_connected);

    const auto context = producer.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto expired =
        producer.submit(*context, finite_rumble_request(1U, 0.5F, 0.25F), make_expired_window());
    REQUIRE_FALSE(expired.has_value());
    CHECK(expired.error() == output_error::expired);

    const auto missing_window = producer.submit(*context, finite_rumble_request(1U, 0.5F, 0.25F));
    REQUIRE_FALSE(missing_window.has_value());
    CHECK(missing_window.error() == output_error::invalid_argument);

    auto invalid_channels = finite_rumble_request(1U, 0.5F, 0.25F);
    invalid_channels.selected_channels = leds_mask;
    const auto rejected_channel =
        producer.submit(*context, invalid_channels, make_window(std::chrono::steady_clock::now()));
    REQUIRE_FALSE(rejected_channel.has_value());
    CHECK(rejected_channel.error() == output_error::invalid_argument);

    const auto submitted = producer.submit(*context,
        finite_rumble_request(1U, 0.5F, 0.25F),
        make_window(std::chrono::steady_clock::now()));
    REQUIRE(submitted.has_value());

    const internal_context_options colliding_options{.owner = context->identity.internal_owner,
        .priority = internal_priority::game_semantic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::foreground_bound};
    auto ordinary = fixture.trusted.make_producer(colliding_options);
    REQUIRE(ordinary.has_value());
    const auto ordinary_context = ordinary->open(fixture.connection.target);
    REQUIRE(ordinary_context.has_value());
    const auto cross_grant = ordinary->submit(*context, finite_rumble_request(2U, 0.9F, 0.1F));
    REQUIRE_FALSE(cross_grant.has_value());
    CHECK(cross_grant.error() == output_error::stale_context);
    const clear_request cross_clear{.selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = submitted->resulting_revisions};
    const auto rejected_clear = ordinary->clear(*context, cross_clear);
    REQUIRE_FALSE(rejected_clear.has_value());
    CHECK(rejected_clear.error() == output_error::stale_context);
    const auto rejected_close = ordinary->close(*context, context->identity.id.value);
    REQUIRE_FALSE(rejected_close.has_value());
    CHECK(rejected_close.error() == output_error::stale_context);

    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    const auto expected = project_representation(*snapshot, rumble_mask);
    REQUIRE(expected.has_value());
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    token->revoke();
    const auto stale = authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE(stale.has_value());
    CHECK_FALSE(*stale);
    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
    const auto close = producer.close(*context, context->identity.id.value);
    REQUIRE(close.has_value());
    const auto rejected_acknowledge = ordinary->acknowledge(*close);
    REQUIRE_FALSE(rejected_acknowledge.has_value());
    CHECK(rejected_acknowledge.error() == output_error::stale_context);
    REQUIRE(producer.acknowledge(*close).has_value());
    const auto ordinary_close =
        ordinary->close(*ordinary_context, ordinary_context->identity.id.value);
    REQUIRE(ordinary_close.has_value());
    REQUIRE(ordinary->acknowledge(*ordinary_close).has_value());

    const auto revoked_before_open_token = fixture.acquire_window(101U);
    auto revoked_before_open = fixture.trusted.make_window_rumble(revoked_before_open_token);
    REQUIRE(revoked_before_open.has_value());
    revoked_before_open_token->revoke();
    const auto rejected_open = revoked_before_open->open(fixture.connection.target);
    REQUIRE_FALSE(rejected_open.has_value());
    CHECK(rejected_open.error() == output_error::revoked);
}

TEST_CASE("output.window_rumble_revocation_isolated_to_its_exact_sibling_token",
    "[component][output][dinput]") {
    window_fixture fixture;
    const auto first_token = fixture.acquire_window(101U);
    const auto sibling_token = fixture.acquire_window(101U);
    REQUIRE(first_token->epoch() == sibling_token->epoch());
    auto first = fixture.trusted.make_window_rumble(first_token);
    auto sibling = fixture.trusted.make_window_rumble(sibling_token);
    REQUIRE(first.has_value());
    REQUIRE(sibling.has_value());
    const auto first_context = first->open(fixture.connection.target);
    const auto sibling_context = sibling->open(fixture.connection.target);
    REQUIRE(first_context.has_value());
    REQUIRE(sibling_context.has_value());
    REQUIRE(first
            ->submit(*first_context,
                finite_rumble_request(1U, 0.4F, 0.2F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());
    REQUIRE(sibling
            ->submit(*sibling_context,
                finite_rumble_request(1U, 0.7F, 0.6F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());

    first_token->revoke();
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->rumble.has_value());
    const auto sibling_rumble = snapshot->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(sibling_rumble.provenance.identity == sibling_context->identity);
    const auto first_state =
        authority_test_access::observe_record(fixture.owner, first_context->identity);
    const auto sibling_state =
        authority_test_access::observe_record(fixture.owner, sibling_context->identity);
    REQUIRE(first_state.has_value());
    REQUIRE(sibling_state.has_value());
    const auto first_observation =
        first_state.value_or(authority_test_access::record_observation{});
    const auto sibling_observation =
        sibling_state.value_or(authority_test_access::record_observation{});
    CHECK_FALSE(first_observation.open);
    CHECK(sibling_observation.open);

    const auto first_close = first->close(*first_context, first_context->identity.id.value);
    const auto sibling_close = sibling->close(*sibling_context, sibling_context->identity.id.value);
    REQUIRE(first_close.has_value());
    REQUIRE(sibling_close.has_value());
    REQUIRE(first->acknowledge(*first_close).has_value());
    REQUIRE(sibling->acknowledge(*sibling_close).has_value());
}

TEST_CASE("output.obsolete_window_open_does_not_revoke_current_process_contexts",
    "[component][output][dinput]") {
    window_fixture fixture;
    const auto old_token = fixture.acquire_window(101U);
    auto obsolete = fixture.trusted.make_window_rumble(old_token);
    REQUIRE(obsolete.has_value());

    fixture.focus_source->set_state(focus::evidence::background);
    REQUIRE(fixture.wait_for_state(focus::evidence::background));
    fixture.focus_source->set_state(focus::evidence::foreground);
    REQUIRE(fixture.wait_for_state(focus::evidence::foreground));
    const auto current_epoch = [&fixture] {
        const auto guard = fixture.focus_observer.lock_presentation();
        return guard.current_foreground_epoch();
    }();
    REQUIRE(current_epoch.has_value());
    CHECK(current_epoch.value_or(focus::foreground_epoch{}) != old_token->epoch().process);

    auto process = make_process_producer(fixture.trusted, 902U);
    const auto process_context = process.open(fixture.connection.target);
    REQUIRE(process_context.has_value());
    REQUIRE(process.submit(*process_context, finite_rumble_request(1U, 0.25F, 0.5F)).has_value());

    const auto rejected = obsolete->open(fixture.connection.target);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == output_error::revoked);
    const auto still_open =
        authority_test_access::observe_record(fixture.owner, process_context->identity);
    REQUIRE(still_open.has_value());
    CHECK(still_open.value_or(authority_test_access::record_observation{}).open);
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->rumble.has_value());
    const auto process_rumble = snapshot->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(process_rumble.provenance.identity == process_context->identity);

    const auto close = process.close(*process_context, process_context->identity.id.value);
    REQUIRE(close.has_value());
    REQUIRE(process.acknowledge(*close).has_value());
}

TEST_CASE("output.window_retirement_preserves_exact_grants_and_reclaims_reused_slots",
    "[component][output][dinput]") {
    window_fixture fixture;
    auto producer = fixture.trusted.make_window_rumble(fixture.acquire_window(101U));
    auto sibling = fixture.trusted.make_window_rumble(fixture.acquire_window(101U));
    REQUIRE(producer.has_value());
    REQUIRE(sibling.has_value());
    const auto context = producer->open(fixture.connection.target);
    const auto sibling_context = sibling->open(fixture.connection.target);
    REQUIRE(context.has_value());
    REQUIRE(sibling_context.has_value());
    const internal_context_options options{.owner = context->identity.internal_owner,
        .priority = internal_priority::game_semantic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::foreground_bound};
    auto ordinary = fixture.trusted.make_producer(options);
    REQUIRE(ordinary.has_value());
    const auto ordinary_context = ordinary->open(fixture.connection.target);
    REQUIRE(ordinary_context.has_value());
    ordinary->retire_window_context(context->identity);
    sibling->retire_window_context(context->identity);
    producer->retire_window_context(ordinary_context->identity);
    auto forged = context->identity;
    ++forged.target.connection.value;
    producer->retire_window_context(forged);
    REQUIRE(authority_test_access::observe_record(fixture.owner, context->identity).has_value());
    REQUIRE(authority_test_access::observe_record(fixture.owner, ordinary_context->identity)
            .has_value());
    producer->retire_window_context(context->identity);
    CHECK_FALSE(
        authority_test_access::observe_record(fixture.owner, context->identity).has_value());
    CHECK(authority_test_access::observe_record(fixture.owner, sibling_context->identity)
            .has_value());
    for (std::size_t index = 0U; index < 40U; ++index) {
        const auto replacement = producer->open(fixture.connection.target);
        REQUIRE(replacement.has_value());
        CHECK(replacement->identity != context->identity);
        producer->retire_window_context(context->identity);
        REQUIRE(authority_test_access::observe_record(fixture.owner, replacement->identity)
                .has_value());
        producer->retire_window_context(replacement->identity);
    }
}

TEST_CASE("output.window_retirement_uses_reserved_cleanup_headroom",
    "[component][output][dinput]") {
    window_fixture fixture;
    auto producer = fixture.trusted.make_window_rumble(fixture.acquire_window(101U));
    REQUIRE(producer.has_value());
    const auto context = producer->open(fixture.connection.target);
    REQUIRE(context.has_value());
    SECTION("open record consumes its reserved final version") {
        authority_test_access::set_logical_version(fixture.owner,
            std::numeric_limits<std::uint64_t>::max() - 1U);
    }
    SECTION("retained record needs no additional version") {
        REQUIRE(producer->close(*context, context->identity.id.value).has_value());
        authority_test_access::set_logical_version(fixture.owner,
            std::numeric_limits<std::uint64_t>::max());
    }
    producer->retire_window_context(context->identity);
    CHECK_FALSE(
        authority_test_access::observe_record(fixture.owner, context->identity).has_value());
    CHECK(authority_test_access::logical_version(fixture.owner) ==
          std::numeric_limits<std::uint64_t>::max());
    producer->retire_window_context(context->identity);
}

TEST_CASE("output.window_rumble_does_not_survive_a_window_round_trip_and_preserves_process_grants",
    "[component][output][dinput]") {
    window_fixture fixture;
    const auto window_token = fixture.acquire_window(101U);
    auto window_producer_result = fixture.trusted.make_window_rumble(window_token);
    REQUIRE(window_producer_result.has_value());
    auto window_producer = std::move(*window_producer_result);
    const auto window_context = window_producer.open(fixture.connection.target);
    REQUIRE(window_context.has_value());
    REQUIRE(window_producer
            .submit(*window_context,
                finite_rumble_request(1U, 0.8F, 0.75F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());

    auto process_producer = make_process_producer(fixture.trusted, 901U);
    const auto process_context = process_producer.open(fixture.connection.target);
    REQUIRE(process_context.has_value());
    REQUIRE(process_producer.submit(*process_context, finite_rumble_request(1U, 0.2F, 0.3F))
            .has_value());

    const auto before = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(before.has_value());
    REQUIRE(before->rumble.has_value());
    const auto window_rumble = before->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(window_rumble.provenance.identity == window_context->identity);
    const auto expected = project_representation(*before, rumble_mask);
    REQUIRE(expected.has_value());
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    const auto current =
        authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE(current.has_value());
    CHECK(*current);

    fixture.focus_source->set_window(202U);
    REQUIRE(fixture.wait_for_window(202U));
    const auto process_epoch_after_switch = [&fixture] {
        const auto guard = fixture.focus_observer.lock_presentation();
        return guard.current_foreground_epoch();
    }();
    REQUIRE(process_epoch_after_switch.has_value());
    CHECK(process_epoch_after_switch.value_or(focus::foreground_epoch{}) ==
          window_token->epoch().process);

    const auto after_switch = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_switch.has_value());
    REQUIRE(after_switch->rumble.has_value());
    const auto process_rumble_after_switch =
        after_switch->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(process_rumble_after_switch.provenance.identity == process_context->identity);
    const auto retired =
        authority_test_access::observe_record(fixture.owner, window_context->identity);
    REQUIRE(retired.has_value());
    CHECK_FALSE(retired.value_or(authority_test_access::record_observation{}).open);

    fixture.focus_source->set_window(101U);
    REQUIRE(fixture.wait_for_window(101U));
    const auto returned_epoch = fixture.current_window(101U);
    CHECK(returned_epoch != window_token->epoch());
    const auto after_return = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_return.has_value());
    REQUIRE(after_return->rumble.has_value());
    const auto process_rumble_after_return =
        after_return->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(process_rumble_after_return.provenance.identity == process_context->identity);

    const auto stale = authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE(stale.has_value());
    CHECK_FALSE(*stale);
    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());

    const auto window_close =
        window_producer.close(*window_context, window_context->identity.id.value);
    REQUIRE(window_close.has_value());
    REQUIRE(window_producer.acknowledge(*window_close).has_value());
    const auto process_close =
        process_producer.close(*process_context, process_context->identity.id.value);
    REQUIRE(process_close.has_value());
    REQUIRE(process_producer.acknowledge(*process_close).has_value());
}

TEST_CASE("output.window_rumble_is_revoked_with_its_backend_and_cannot_bind_to_a_replacement",
    "[component][output][dinput][backend][replacement]") {
    const auto owned_fixture = std::make_unique<window_fixture>();
    auto& fixture = *owned_fixture;
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    const auto old_backend = make_backend(fixture.connection.target, *execution, 1U);
    const auto new_backend = make_backend(fixture.connection.target, *execution, 2U);
    const auto token = fixture.acquire_window(101U);
    const auto stale_owner = execution_owner{.authority = execution->authority,
        .id = execution_id{execution->id.value + 1U}};
    const auto stale_backend = make_backend(fixture.connection.target, stale_owner, 3U);
    const auto rejected_factory = fixture.trusted.make_window_rumble(token, stale_backend);
    REQUIRE_FALSE(rejected_factory.has_value());
    CHECK(rejected_factory.error() == output_error::not_connected);

    auto old_result = fixture.trusted.make_window_rumble(token, old_backend);
    auto replacement_result = fixture.trusted.make_window_rumble(token, new_backend);
    REQUIRE(old_result.has_value());
    REQUIRE(replacement_result.has_value());
    auto old_producer = std::move(*old_result);
    auto replacement_producer = std::move(*replacement_result);
    auto process_producer = make_process_producer(fixture.trusted, 903U);

    const auto old_context = old_producer.open(fixture.connection.target);
    const auto replacement_context = replacement_producer.open(fixture.connection.target);
    const auto process_context = process_producer.open(fixture.connection.target);
    REQUIRE(old_context.has_value());
    REQUIRE(replacement_context.has_value());
    REQUIRE(process_context.has_value());
    REQUIRE(replacement_producer
            .submit(*replacement_context,
                finite_rumble_request(1U, 0.4F, 0.3F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());
    REQUIRE(process_producer.submit(*process_context, finite_rumble_request(1U, 0.2F, 0.1F))
            .has_value());
    REQUIRE(old_producer
            .submit(*old_context,
                finite_rumble_request(1U, 0.8F, 0.7F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());

    const auto old_projection =
        require_rumble_projection(fixture.owner, fixture.connection.target, old_context->identity);
    const auto old_qualifies = authority_execution_access::qualifies(fixture.owner,
        *execution,
        old_projection,
        old_backend);
    REQUIRE(old_qualifies.has_value());
    CHECK(*old_qualifies);

    authority_execution_access::retire_backend(fixture.owner, old_backend);
    require_record_open(fixture.owner, old_context->identity, false);
    require_record_open(fixture.owner, replacement_context->identity, true);
    require_record_open(fixture.owner, process_context->identity, true);
    static_cast<void>(require_rumble_projection(fixture.owner,
        fixture.connection.target,
        replacement_context->identity));
    const auto stale_completion = authority_execution_access::qualifies(fixture.owner,
        *execution,
        old_projection,
        old_backend);
    REQUIRE(stale_completion.has_value());
    CHECK_FALSE(*stale_completion);
    require_record_open(fixture.owner, replacement_context->identity, true);

    // An operation admitted just after backend retirement can open the old producer again, but
    // exact-handle qualification must revoke it before a replacement backend can write it.
    const auto late_context = old_producer.open(fixture.connection.target);
    REQUIRE(late_context.has_value());
    REQUIRE(old_producer
            .submit(*late_context,
                finite_rumble_request(1U, 0.9F, 0.9F),
                make_window(std::chrono::steady_clock::now()))
            .has_value());
    const auto late_projection =
        require_rumble_projection(fixture.owner, fixture.connection.target, late_context->identity);
    const auto replacement_qualification = authority_execution_access::qualifies(fixture.owner,
        *execution,
        late_projection,
        new_backend);
    REQUIRE(replacement_qualification.has_value());
    CHECK_FALSE(*replacement_qualification);

    static_cast<void>(require_rumble_projection(fixture.owner,
        fixture.connection.target,
        replacement_context->identity));
    require_record_open(fixture.owner, late_context->identity, false);

    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
    for (auto&& [producer, context] :
        std::array<std::pair<internal_producer*, internal_context>, 4U>{
            std::pair{&old_producer, *old_context},
            std::pair{&old_producer, *late_context},
            std::pair{&replacement_producer, *replacement_context},
            std::pair{&process_producer, *process_context}}) {
        const auto closed = producer->close(context, context.identity.id.value);
        REQUIRE(closed.has_value());
        REQUIRE(producer->acknowledge(*closed).has_value());
    }
}

} // namespace
