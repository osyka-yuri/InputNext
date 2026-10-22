#include "output/authority_fixture.hpp"

#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"
#include "output/xinput/rumble.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

TEST_CASE("output.xinput_rumble_tags_isolate_colliding_internal_owner_keys",
    "[component][output][xinput]") {
    authority_fixture fixture{};
    auto created = fixture.trusted.make_xinput_rumble();
    REQUIRE(created.has_value());
    const auto bridge = std::move(created.value());
    const auto service_id = bridge->service_id();

    focus::foreground_epoch epoch{};
    {
        const auto guard = fixture.focus_observer.lock_presentation();
        const auto current = guard.current_foreground_epoch();
        REQUIRE(current.has_value());
        epoch = current.value_or(focus::foreground_epoch{});
    }
    const xinput::assignment occupant{.user_index = 0U,
        .id = 91U,
        .target = fixture.connection.target};
    const output::detail::xinput_rumble_key key{.service = service_id,
        .occupant = occupant,
        .enable_generation = 1U,
        .epoch = epoch};

    std::optional<internal_context> xinput_context;
    {
        auto turn = bridge->begin();
        REQUIRE(
            turn.submit(key,
                    xinput_context,
                    rumble_value{.low_frequency = 0.25F, .high_frequency = 0.75F},
                    1U)
                .has_value());
    }
    REQUIRE(xinput_context.has_value());
    CHECK(xinput_context.value_or(internal_context{}).identity.internal_owner ==
          internal_owner_key{service_id.id.value});

    const internal_context_options colliding_options{.owner =
                                                         internal_owner_key{service_id.id.value},
        .priority = internal_priority::game_semantic,
        .ttl = ttl_permission::finite_or_indefinite,
        .foreground = foreground_policy::foreground_bound};
    auto producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, colliding_options);
    const auto generic_context = producer.open(fixture.connection.target);
    REQUIRE(generic_context.has_value());
    CHECK(generic_context.value_or(internal_context{}).identity !=
          xinput_context.value_or(internal_context{}).identity);

    const auto generic_request = inputnext::test::output::rumble_request({.channels = rumble_mask,
        .sequence = 1U,
        .ttl = std::chrono::milliseconds{0},
        .low = 0.5F,
        .high = 0.125F});
    REQUIRE(
        producer.submit(generic_context.value_or(internal_context{}), generic_request).has_value());
    const auto cannot_mutate_xinput = producer.submit(xinput_context.value_or(internal_context{}),
        inputnext::test::output::rumble_request({.channels = rumble_mask,
            .sequence = 2U,
            .ttl = std::chrono::milliseconds{0},
            .low = 1.0F,
            .high = 1.0F}));
    CHECK((!cannot_mutate_xinput.has_value() &&
           cannot_mutate_xinput.error() == output_error::stale_context));

    const auto xinput_before = output::detail::authority_test_access::observe_record(fixture.owner,
        xinput_context.value_or(internal_context{}).identity);
    REQUIRE(xinput_before.has_value());
    const auto xinput_state =
        xinput_before.value_or(output::detail::authority_test_access::record_observation{});
    CHECK(xinput_state.open);
    CHECK(xinput_state.accepted_sequence == 1U);

    {
        auto turn = bridge->begin();
        turn.revoke_and_ack(xinput_context);
    }
    const auto generic_after = output::detail::authority_test_access::observe_record(fixture.owner,
        generic_context.value_or(internal_context{}).identity);
    REQUIRE(generic_after.has_value());
    const auto generic_state =
        generic_after.value_or(output::detail::authority_test_access::record_observation{});
    CHECK(generic_state.open);
    CHECK(generic_state.accepted_sequence == 1U);

    const auto generic_close = producer.close(generic_context.value_or(internal_context{}),
        generic_context.value_or(internal_context{}).identity.id.value);
    REQUIRE(generic_close.has_value());
    REQUIRE(producer.acknowledge(generic_close.value()).has_value());
}

TEST_CASE("output.xinput_rumble_identity_exhaustion_fails_before_record_installation",
    "[component][output][xinput]") {
    authority_fixture fixture{std::byte{0x61U},
        authority_seed{.next_context_id = std::numeric_limits<std::uint64_t>::max()}};
    auto created = fixture.trusted.make_xinput_rumble();
    REQUIRE(created.has_value());
    const auto bridge = std::move(created.value());
    focus::foreground_epoch epoch{};
    {
        const auto guard = fixture.focus_observer.lock_presentation();
        const auto current = guard.current_foreground_epoch();
        REQUIRE(current.has_value());
        epoch = current.value_or(focus::foreground_epoch{});
    }

    const xinput::assignment occupant{.user_index = 0U,
        .id = 1U,
        .target = fixture.connection.target};
    const output::detail::xinput_rumble_key key{.service = bridge->service_id(),
        .occupant = occupant,
        .enable_generation = 1U,
        .epoch = epoch};
    std::optional<internal_context> context;
    bool identity_was_exhausted{};
    {
        auto turn = bridge->begin();
        const auto rejected = turn.submit(key,
            context,
            rumble_value{.low_frequency = 0.5F, .high_frequency = 0.5F},
            1U);
        identity_was_exhausted =
            !rejected.has_value() && rejected.error() == output_error::identity_exhausted;
    }
    CHECK(identity_was_exhausted);
    CHECK((!context.has_value()));
    CHECK(output::detail::authority_test_access::logical_version(fixture.owner) == 0U);
}

} // namespace
