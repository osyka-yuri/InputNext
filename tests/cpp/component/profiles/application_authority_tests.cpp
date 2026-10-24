#include "application_test_support.hpp"

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/internal_producer.hpp"
#include "profiles/application.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::profiles;
using namespace inputnext::profiles_test_support;

TEST_CASE("profiles.application_retries_after_internal_context_capacity_saturation",
    "[profiles][component][authority][capacity]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));

    constexpr std::size_t internal_capacity = 32U;
    std::vector<output::detail::internal_producer> blockers;
    std::vector<output::internal_context> contexts;
    blockers.reserve(internal_capacity);
    contexts.reserve(internal_capacity);
    for (std::size_t index = 0U; index < internal_capacity; ++index) {
        auto options = application_options;
        options.owner = output::internal_owner_key{static_cast<std::uint64_t>(2000U + index)};
        auto blocker =
            inputnext::test::output::make_internal_producer(fixture.composition(), options);
        const auto opened = blocker.open(connection.connection.target);
        REQUIRE(opened.has_value());
        blockers.emplace_back(std::move(blocker));
        contexts.push_back(opened.value());
    }

    const auto saturated = fixture.app().replace_profile(left_profile(0.55F));
    CHECK(saturated.pending_contexts == 0U);
    REQUIRE(saturated.failure.has_value());
    const auto saturation_failure = required_value(saturated.failure);
    CHECK(saturation_failure.operation == application_operation::open);
    CHECK(saturation_failure.code == output::error::capacity_exceeded);
    CHECK_FALSE(saturated.terminal);
    CHECK(fixture.snapshot(connection.connection.target).present_channels() == 0U);

    const auto released =
        blockers.front().close(contexts.front(), contexts.front().identity.id.value);
    REQUIRE(released.has_value());
    REQUIRE(blockers.front().acknowledge(released.value()).has_value());

    const auto retried = fixture.app().maintain();
    CHECK(retried.pending_contexts == 1U);
    CHECK_FALSE(retried.failure.has_value());
    const auto applied = fixture.snapshot(connection.connection.target);
    REQUIRE(applied.left_trigger.has_value());
    const auto applied_left = required_value(applied.left_trigger);
    CHECK(applied_left.value == feedback_trigger(0.55F));

    for (std::size_t index = 1U; index < blockers.size(); ++index) {
        const auto closed =
            blockers[index].close(contexts[index], contexts[index].identity.id.value);
        REQUIRE(closed.has_value());
        REQUIRE(blockers[index].acknowledge(closed.value()).has_value());
    }
    const auto application_closed = fixture.app().close();
    CHECK(application_closed.pending_contexts == 0U);
    CHECK(application_closed.terminal);
}

TEST_CASE("profiles.application_loses_to_public_sdk_until_its_ttl_expires",
    "[profiles][component][authority][priority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto installed = fixture.app().replace_profile(left_profile(0.3F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto profile_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(profile_snapshot.left_trigger.has_value());
    const auto profile_left = required_value(profile_snapshot.left_trigger);
    const auto profile_identity = profile_left.provenance.identity;

    const auto sdk_context = fixture.authority().open(connection.connection.target);
    REQUIRE(sdk_context.has_value());
    const auto sdk = sdk_context.value();
    output::submit_request sdk_request;
    sdk_request.selected_channels = output::left_trigger_mask;
    sdk_request.sequence = 1U;
    sdk_request.ttl = 1ms;
    sdk_request.values.left_trigger = output::trigger_value{
        .mode = output::trigger_mode::weapon,
        .layout = output::trigger_layout::uniform,
        .start = 0.1F,
        .end = 0.6F,
        .strength = 0.9F,
    };
    REQUIRE(fixture.authority().submit(sdk, sdk_request).has_value());

    const auto overridden = fixture.snapshot(connection.connection.target);
    REQUIRE(overridden.left_trigger.has_value());
    const auto overridden_left = required_value(overridden.left_trigger);
    CHECK(overridden_left.provenance.identity == sdk.identity);
    CHECK(overridden_left.value.mode == output::trigger_mode::weapon);

    fixture.advance_clock(std::chrono::duration_cast<std::chrono::steady_clock::duration>(2ms));
    const auto expired = fixture.snapshot(connection.connection.target);
    REQUIRE(expired.left_trigger.has_value());
    const auto expired_left = required_value(expired.left_trigger);
    CHECK(expired_left.provenance.identity == profile_identity);
    CHECK(expired_left.value == feedback_trigger(0.3F));
    const auto stable = fixture.app().maintain();
    CHECK(stable.pending_contexts == 1U);
    CHECK_FALSE(stable.failure.has_value());

    const auto sdk_closed = fixture.authority().close(sdk, sdk.identity.id.value);
    REQUIRE(sdk_closed.has_value());
    REQUIRE(fixture.authority().acknowledge(sdk_closed.value()).has_value());

    const auto closed = fixture.app().close();
    CHECK(closed.pending_contexts == 0U);
    CHECK(closed.terminal);
}

TEST_CASE("profiles.application_terminal_close_never_reopens_after_new_assignments",
    "[profiles][component][authority][lifecycle]") {
    application_fixture fixture;
    const auto first = fixture.add_connection();
    static_cast<void>(fixture.assign(first));
    const auto installed = fixture.app().replace_profile(left_profile(0.65F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    REQUIRE(fixture.snapshot(first.connection.target).left_trigger.has_value());

    const auto closed = fixture.app().close();
    CHECK(closed.terminal);
    CHECK(closed.pending_contexts == 0U);
    CHECK_FALSE(closed.failure.has_value());
    CHECK(fixture.snapshot(first.connection.target).present_channels() == 0U);

    const auto second = fixture.add_connection();
    static_cast<void>(fixture.assign(second));
    const auto version_before =
        output::detail::authority_test_access::logical_version(fixture.authority());
    const auto after_close = fixture.app().maintain();
    CHECK(after_close.terminal);
    CHECK(after_close.pending_contexts == 0U);
    CHECK_FALSE(after_close.failure.has_value());
    CHECK(output::detail::authority_test_access::logical_version(fixture.authority()) ==
          version_before);
    CHECK(fixture.snapshot(second.connection.target).present_channels() == 0U);
    const auto repeated_close = fixture.app().close();
    CHECK(repeated_close.terminal);
    CHECK(repeated_close.pending_contexts == 0U);
}

TEST_CASE("profiles.application_accepts_exact_close_proof_for_reclaimed_handle",
    "[profiles][component][authority][reclamation]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto installed = fixture.app().replace_profile(left_profile(0.75F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto original = fixture.snapshot(connection.connection.target);
    REQUIRE(original.left_trigger.has_value());
    const auto original_left = required_value(original.left_trigger);
    const auto original_identity = original_left.provenance.identity;
    const output::internal_context original_context{.identity = original_identity};

    const auto externally_closed =
        fixture.producer().close(original_context, original_identity.id.value);
    REQUIRE(externally_closed.has_value());
    REQUIRE(fixture.producer().acknowledge(externally_closed.value()).has_value());
    CHECK_FALSE(fixture.record(original_identity).has_value());

    const auto reconciled = fixture.app().maintain();
    CHECK(reconciled.pending_contexts == 1U);
    CHECK_FALSE(reconciled.failure.has_value());
    const auto replacement = fixture.snapshot(connection.connection.target);
    REQUIRE(replacement.left_trigger.has_value());
    const auto replacement_left = required_value(replacement.left_trigger);
    CHECK(replacement_left.value == feedback_trigger(0.75F));
    CHECK(replacement_left.provenance.identity != original_identity);
    CHECK_FALSE(fixture.record(original_identity).has_value());

    const auto closed = fixture.app().close();
    CHECK(closed.pending_contexts == 0U);
    CHECK(closed.terminal);
}

} // namespace
