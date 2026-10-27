#include "effects/effects.hpp"
#include "effects/identity.hpp"
#include "effects_session_fixture.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace inputnext::effects_test {

using namespace std::chrono_literals;
using effects_integration_test_support::session_fixture;

TEST_CASE("effects.owner_uses_bounded_nonreused_handles_and_exact_close_receipts",
    "[integration][effects][owner][identity][close]") {
    effects::identity_source identities{effects::identity_seeds{.next_context = 1000U}};
    session_fixture fixture{&identities};
    fixture.start();

    std::array<effects::context_handle, effects::context_capacity> contexts{};
    std::set<std::uint64_t> ids;
    for (auto& context : contexts) {
        const auto opened = fixture.service().open(fixture.target);
        REQUIRE(opened.has_value());
        context = *opened;
        CHECK(context.runtime == fixture.target.runtime);
        CHECK(context.id >= 1000U);
        CHECK(ids.insert(context.id).second);
    }

    const auto full = fixture.service().open(fixture.target);
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error() == effects::status::capacity_exceeded);

    const auto wrong_key = fixture.service().close(contexts[0U], contexts[0U].id + 1U);
    REQUIRE_FALSE(wrong_key.has_value());
    CHECK(wrong_key.error() == effects::status::invalid_argument);

    const auto first_close = fixture.service().close(contexts[0U], contexts[0U].id);
    REQUIRE(first_close.has_value());
    CHECK(first_close->context == contexts[0U]);
    CHECK(first_close->target == fixture.target);
    CHECK(first_close->close_request_id == contexts[0U].id);
    CHECK(first_close->publication.value != 0U);
    CHECK(first_close->disposition == effects::close_disposition::normal);
    CHECK(fixture.service().close(contexts[0U], contexts[0U].id) == first_close);

    auto forged = *first_close;
    ++forged.target.connection.value;
    CHECK(fixture.service().acknowledge(forged) == effects::status::stale_context);

    CHECK(fixture.service().acknowledge(*first_close) == effects::status::ok);
    CHECK(fixture.service().acknowledge(*first_close) == effects::status::ok);
    const auto reused = fixture.service().open(fixture.target);
    REQUIRE(reused.has_value());
    CHECK(reused->id != contexts[0U].id);
    CHECK(fixture.service().acknowledge(*first_close) == effects::status::stale_context);
    const auto old_context = fixture.service().inspect(contexts[0U]);
    REQUIRE_FALSE(old_context.has_value());
    CHECK(old_context.error() == effects::status::stale_context);
}

TEST_CASE("effects.owner_reserves_close_identity_before_publication_exhaustion",
    "[integration][effects][owner][identity][close]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    effects::identity_source identities{
        effects::identity_seeds{.next_context = 21U, .next_publication = maximum}};
    session_fixture fixture{&identities};
    fixture.start();

    const auto context = fixture.open();
    const auto ttl = 2s;
    const output::submit_request request{.selected_channels = output::rumble_mask,
        .sequence = 1U,
        .ttl = ttl,
        .values = {.rumble = {.low_frequency = 0.25F, .high_frequency = 0.4F}}};
    const auto submitted = fixture.service().submit(context, request, session_fixture::window(ttl));
    REQUIRE_FALSE(submitted.has_value());
    CHECK(submitted.error() == effects::status::capacity_exceeded);

    const auto closed = fixture.service().close(context, context.id);
    REQUIRE(closed.has_value());
    CHECK(closed->publication.value == maximum);
    CHECK(fixture.service().acknowledge(*closed) == effects::status::ok);
}

TEST_CASE("effects.owner_publication_namespace_covers_discrete_body_and_close_receipts",
    "[integration][effects][owner][body][identity]") {
    session_fixture fixture;
    fixture.start();
    const auto discrete_context = fixture.open();
    const auto body_context = fixture.open();

    const auto discrete_ttl = 2s;
    const output::submit_request discrete_request{.selected_channels = output::rumble_mask,
        .sequence = 1U,
        .ttl = discrete_ttl,
        .values = {.rumble = {.low_frequency = 0.2F, .high_frequency = 0.6F}}};
    const auto discrete = fixture.service().submit(discrete_context,
        discrete_request,
        session_fixture::window(discrete_ttl));
    REQUIRE(discrete.has_value());

    const auto body_ttl = 45s;
    const effects::body_request body_request{.context = body_context,
        .sequence = 1U,
        .ttl = body_ttl,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.15F, .high_frequency = 0.3F},
        .pcm_command = output::body_pcm_command::keep};
    const auto body =
        fixture.service().submit_body(body_request, session_fixture::window(body_ttl));
    REQUIRE(body.has_value());
    CHECK(body->sequence == 1U);
    CHECK(body->lease_id != 0U);

    const auto close = fixture.service().close(body_context, body_context.id);
    REQUIRE(close.has_value());
    CHECK(discrete->publication != body->publication);
    CHECK(discrete->publication != close->publication);
    CHECK(body->publication != close->publication);
}

TEST_CASE("effects.owner_forwards_finite_expiry_and_prepares_pcm_through_native_owner",
    "[integration][effects][owner][body][pcm][expiry]") {
    session_fixture fixture;
    fixture.start();
    const auto context = fixture.open();

    constexpr auto ttl = 2s;
    const output::submit_request request{.selected_channels = output::rumble_mask,
        .sequence = 1U,
        .ttl = ttl,
        .values = {.rumble = {.low_frequency = 0.2F, .high_frequency = 0.4F}}};
    const auto expired_window =
        output::make_finite_submission_window(std::chrono::steady_clock::now() - 3s, ttl);
    REQUIRE(expired_window.has_value());
    const auto expired = fixture.service().submit(context, request, *expired_window);
    REQUIRE_FALSE(expired.has_value());
    CHECK(expired.error() == effects::status::not_admitted);

    const auto body_context = fixture.open();
    std::array<float, 4U> samples{0.1F, -0.2F, 0.3F, -0.4F};
    const auto body_ttl = 45s;
    effects::body_request body_request{.context = body_context,
        .sequence = 1U,
        .ttl = body_ttl,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.1F, .high_frequency = 0.2F},
        .pcm_command = output::body_pcm_command::replace,
        .replacement = output::body_pcm_input{.samples = samples,
            .frame_count = 2U,
            .channels = 2U,
            .sample_rate = output::body_pcm_sample_rate,
            .gain = 1.0F}};
    const auto prepared =
        fixture.service().submit_body(body_request, session_fixture::window(body_ttl));
    REQUIRE(prepared.has_value());
    CHECK(prepared->lease_id != 0U);
    samples.fill(0.9F);

    const auto keep_ttl = 45s;
    body_request.lease_id = prepared->lease_id;
    body_request.audio_instance_id = prepared->audio_instance_id;
    body_request.sequence = 2U;
    body_request.ttl = keep_ttl;
    body_request.pcm_command = output::body_pcm_command::keep;
    body_request.replacement.reset();
    const auto kept =
        fixture.service().submit_body(body_request, session_fixture::window(keep_ttl));
    REQUIRE(kept.has_value());
    CHECK(kept->lease_id == prepared->lease_id);
    CHECK(kept->sequence == 2U);
}

TEST_CASE("effects.owner_delegates_pcm_budget_to_native_preparer",
    "[integration][effects][owner][body][pcm][budget]") {
    session_fixture fixture;
    fixture.start();
    const auto context = fixture.open();

    constexpr auto channels = 2U;
    constexpr auto frames = (output::body_pcm_storage_budget_bytes / sizeof(float) / channels) + 1U;
    std::vector<float> samples(static_cast<std::size_t>(frames * channels), 0.0F);
    constexpr auto ttl = 45s;
    const effects::body_request request{.context = context,
        .sequence = 1U,
        .ttl = ttl,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.1F, .high_frequency = 0.2F},
        .pcm_command = output::body_pcm_command::replace,
        .replacement = output::body_pcm_input{.samples = samples,
            .frame_count = frames,
            .channels = channels,
            .sample_rate = output::body_pcm_sample_rate,
            .gain = 1.0F}};
    const auto rejected = fixture.service().submit_body(request, session_fixture::window(ttl));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == effects::status::capacity_exceeded);
}

TEST_CASE("effects.shutdown_releases_native_contexts_and_retains_external_receipts",
    "[integration][effects][owner][shutdown]") {
    session_fixture fixture;
    fixture.start();
    const auto context = fixture.open();

    const auto cleanup = fixture.service().revoke_for_shutdown();
    CHECK(cleanup.admission_closed);
    CHECK(cleanup.occupied_contexts == 1U);
    CHECK(cleanup.pending_native_cleanup == 0U);
    CHECK_FALSE(cleanup.failure.has_value());

    const auto info = fixture.service().inspect(context);
    REQUIRE(info.has_value());
    CHECK(info->state == effects::context_state::revoked);
    const auto receipt = fixture.service().close(context, context.id);
    REQUIRE(receipt.has_value());
    CHECK(fixture.service().acknowledge(*receipt) == effects::status::ok);
    const auto refused = fixture.service().open(fixture.target);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == effects::status::unavailable);
}

} // namespace inputnext::effects_test
