#include "values/required_value.hpp"
#include <expected>
#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/projection/represented.hpp"
#include "output/xinput/rumble.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::native_values;
using inputnext::output::detail::project_representation;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;
namespace focus = inputnext::focus;
namespace xinput = inputnext::xinput;

[[nodiscard]] body_request
prefer_hd(std::uint64_t sequence, audio_instance audio, body_lease_id lease = {}) {
    return body_request{.lease = lease,
        .audio = audio,
        .sequence = sequence,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd,
        .fallback = {.low_frequency = 0.5F, .high_frequency = 0.7F}};
}

TEST_CASE("output.body_audio_readiness_and_failure_are_latched_until_renewal",
    "[component][output]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    const auto audio =
        audio_routes.attach(fixture.connection.target, body_route_support::hd, false);
    REQUIRE(audio.has_value());
    const auto duplicate =
        audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    CHECK((!duplicate.has_value() && duplicate.error() == output_error::busy));

    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto acquired = fixture.owner.submit_body(*owner, prefer_hd(1U, *audio));
    REQUIRE(acquired.has_value());
    CHECK(acquired->intent.route == body_actuator_route::classic);
    CHECK(acquired->intent.fallback == body_fallback_reason::not_ready);

    REQUIRE(audio_routes.set_ready(*audio, true).has_value());
    auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent == acquired->intent);

    const auto renewed =
        fixture.owner.submit_body(*owner, prefer_hd(2U, *audio, acquired->identity.lease));
    REQUIRE(renewed.has_value());
    CHECK(renewed->intent.route == body_actuator_route::hd);
    snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());

    REQUIRE(audio_routes.set_ready(*audio, false).has_value());
    snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(snapshot->body).intent.fallback == body_fallback_reason::not_ready);
    const auto latched = required_value(snapshot->body).intent;

    REQUIRE(audio_routes.set_ready(*audio, true).has_value());
    snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    CHECK(required_value(snapshot->body).intent == latched);

    const auto retry =
        fixture.owner.submit_body(*owner, prefer_hd(3U, *audio, acquired->identity.lease));
    REQUIRE(retry.has_value());
    CHECK(retry->intent.route == body_actuator_route::hd);

    const auto current_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(current_snapshot.has_value());
    const auto projection =
        project_representation(*current_snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(projection.has_value());
    const auto output = native_values(*projection);
    CHECK(output.values.rumble == rumble_value{});
    REQUIRE(output.body.has_value());
    CHECK(required_value(output.body).route == body_actuator_route::hd);
}

TEST_CASE("output.body_audio_replacement_never_rebinds_an_existing_lease", "[component][output]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    const auto first_audio =
        audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(first_audio.has_value());
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto acquired = fixture.owner.submit_body(*owner, prefer_hd(1U, *first_audio));
    REQUIRE(acquired.has_value());
    CHECK(acquired->intent.route == body_actuator_route::hd);

    REQUIRE(audio_routes.detach(*first_audio).has_value());
    const auto stale_ready = audio_routes.set_ready(*first_audio, true);
    CHECK((!stale_ready.has_value() && stale_ready.error() == output_error::stale_audio));
    auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(snapshot->body).intent.fallback == body_fallback_reason::instance_lost);

    const auto replacement =
        audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(replacement.has_value());
    CHECK(*replacement != *first_audio);

    const auto old_binding_renewal =
        fixture.owner.submit_body(*owner, prefer_hd(2U, *first_audio, acquired->identity.lease));
    REQUIRE(old_binding_renewal.has_value());
    CHECK(old_binding_renewal->intent.route == body_actuator_route::classic);
    CHECK(old_binding_renewal->intent.fallback == body_fallback_reason::instance_lost);
    const auto attempted_rebind =
        fixture.owner.submit_body(*owner, prefer_hd(3U, *replacement, acquired->identity.lease));
    CHECK((!attempted_rebind.has_value() && attempted_rebind.error() == output_error::stale_audio));

    const body_clear_request clear{.lease = acquired->identity.lease,
        .audio = *first_audio,
        .sequence = 3U};
    REQUIRE(fixture.owner.clear_body(*owner, clear).has_value());
    const auto fresh = fixture.owner.submit_body(*owner, prefer_hd(1U, *replacement));
    REQUIRE(fresh.has_value());
    CHECK(fresh->identity.lease != acquired->identity.lease);
    CHECK(fresh->intent.route == body_actuator_route::hd);
}

TEST_CASE("output.body_diagnostic_preemption_is_latched_even_when_it_is_cleared",
    "[component][output]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    const auto audio = audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(audio.has_value());
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto body = fixture.owner.submit_body(*owner, prefer_hd(1U, *audio));
    REQUIRE(body.has_value());
    CHECK(body->intent.route == body_actuator_route::hd);

    const internal_context_options diagnostic_options{
        .owner = internal_owner_key{812U},
        .priority = internal_priority::diagnostic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto diagnostic_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, diagnostic_options);
    const auto diagnostic = diagnostic_producer.open(fixture.connection.target);
    REQUIRE(diagnostic.has_value());
    const auto diagnostic_values = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.9F, .high = 0.8F});
    const auto diagnostic_receipt = diagnostic_producer.submit(*diagnostic, diagnostic_values);
    REQUIRE(diagnostic_receipt.has_value());

    auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(snapshot->body).intent.fallback == body_fallback_reason::preempted);
    const auto output_projection =
        project_representation(*snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(output_projection.has_value());
    CHECK(native_values(*output_projection).values.rumble == diagnostic_values.values.rumble);

    const clear_request clear_diagnostic{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = diagnostic_receipt->resulting_revisions,
    };
    REQUIRE(diagnostic_producer.clear(*diagnostic, clear_diagnostic).has_value());
    snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent.fallback == body_fallback_reason::preempted);
    CHECK(required_value(snapshot->body).intent.route == body_actuator_route::classic);

    const auto renewal =
        fixture.owner.submit_body(*owner, prefer_hd(2U, *audio, body->identity.lease));
    REQUIRE(renewal.has_value());
    CHECK(renewal->intent.route == body_actuator_route::hd);
}

TEST_CASE("output.body_audio_identity_exhaustion_does_not_reuse_an_instance",
    "[component][output]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    inputnext::output::detail::authority_test_access::set_next_audio_instance_id(fixture.owner,
        std::numeric_limits<std::uint64_t>::max());
    const auto final_instance =
        audio_routes.attach(fixture.connection.target, body_route_support::classic_only);
    REQUIRE(final_instance.has_value());
    CHECK(final_instance->id.value == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(audio_routes.detach(*final_instance).has_value());
    const auto exhausted =
        audio_routes.attach(fixture.connection.target, body_route_support::classic_only);
    CHECK((!exhausted.has_value() && exhausted.error() == output_error::identity_exhausted));
}

TEST_CASE("output.body_and_audio_reservations_constrain_new_xinput_rumble_admission",
    "[component][output][xinput]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    const auto audio = audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(audio.has_value());
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto body = fixture.owner.submit_body(*owner, prefer_hd(1U, *audio));
    REQUIRE(body.has_value());

    auto created = fixture.trusted.make_xinput_rumble();
    REQUIRE(created.has_value());
    const auto bridge = std::move(*created);
    focus::foreground_epoch epoch{};
    {
        const auto guard = fixture.focus_observer.lock_presentation();
        const auto current = guard.current_foreground_epoch();
        REQUIRE(current.has_value());
        epoch = required_value(current);
    }
    const xinput::assignment occupant{.user_index = 0U,
        .id = 29U,
        .target = fixture.connection.target};
    const inputnext::output::detail::xinput_rumble_key key{
        .service = bridge->service_id(),
        .occupant = occupant,
        .enable_generation = 1U,
        .epoch = epoch,
    };

    inputnext::output::detail::authority_test_access::set_logical_version(fixture.owner,
        std::numeric_limits<std::uint64_t>::max() - 7U);
    std::optional<internal_context> current;
    inputnext::output::result<std::uint64_t> submitted =
        std::unexpected{output_error::invalid_argument};
    {
        auto transaction = bridge->begin();
        submitted = transaction.submit(key,
            current,
            rumble_value{.low_frequency = 0.2F, .high_frequency = 0.3F},
            1U);
    }
    CHECK((!submitted.has_value() && submitted.error() == output_error::logical_version_exhausted));
    CHECK(!(current.has_value()));
}

TEST_CASE("output.body_and_audio_reservations_constrain_xinput_rumble_updates",
    "[component][output][xinput]") {
    authority_fixture fixture{};
    auto created = fixture.trusted.make_xinput_rumble();
    REQUIRE(created.has_value());
    const auto bridge = std::move(*created);
    focus::foreground_epoch epoch{};
    {
        const auto guard = fixture.focus_observer.lock_presentation();
        const auto current = guard.current_foreground_epoch();
        REQUIRE(current.has_value());
        epoch = required_value(current);
    }
    const xinput::assignment occupant{.user_index = 0U,
        .id = 30U,
        .target = fixture.connection.target};
    const inputnext::output::detail::xinput_rumble_key key{
        .service = bridge->service_id(),
        .occupant = occupant,
        .enable_generation = 1U,
        .epoch = epoch,
    };
    std::optional<internal_context> current;
    {
        auto transaction = bridge->begin();
        REQUIRE(transaction
                .submit(key,
                    current,
                    rumble_value{.low_frequency = 0.2F, .high_frequency = 0.3F},
                    1U)
                .has_value());
    }
    REQUIRE(current.has_value());

    auto audio_routes = fixture.trusted.audio_routes();
    const auto audio = audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(audio.has_value());
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    REQUIRE(fixture.owner.submit_body(*owner, prefer_hd(1U, *audio)).has_value());
    inputnext::output::detail::authority_test_access::set_logical_version(fixture.owner,
        std::numeric_limits<std::uint64_t>::max() - 6U);

    inputnext::output::result<std::uint64_t> updated =
        std::unexpected{output_error::invalid_argument};
    {
        auto transaction = bridge->begin();
        updated = transaction.submit(key,
            current,
            rumble_value{.low_frequency = 0.8F, .high_frequency = 0.9F},
            2U);
    }
    CHECK((!updated.has_value() && updated.error() == output_error::logical_version_exhausted));
    const auto observation =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            required_value(current).identity);
    REQUIRE(observation.has_value());
    CHECK(required_value(observation).accepted_sequence == 1U);
}

} // namespace
