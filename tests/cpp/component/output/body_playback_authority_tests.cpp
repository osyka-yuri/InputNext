#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/playback_test_support.hpp"
#include "values/required_value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace inputnext::output;
using inputnext::output::detail::authority_test_access;
using inputnext::test::output::take_optional_value;

struct authority_playback_fixture final {
    authority_playback_fixture() {
        const auto attached = authority.trusted.audio_routes().attach(authority.connection.target,
            body_route_support::hd,
            true);
        if (!attached.has_value()) {
            throw std::runtime_error{"playback authority fixture could not attach audio"};
        }
        audio = *attached;
        const auto opened = authority.owner.open(authority.connection.target);
        if (!opened.has_value()) {
            throw std::runtime_error{"playback authority fixture could not open context"};
        }
        context = *opened;
    }

    [[nodiscard]] body_request request(const std::uint64_t sequence) const noexcept {
        return body_request{.lease = body.identity.lease,
            .audio = audio,
            .sequence = sequence,
            .ttl = std::chrono::seconds{5},
            .preference = body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.25F, .high_frequency = 0.5F}};
    }

    [[nodiscard]] result<prepared_body_pcm> prepare() const {
        constexpr std::array samples{0.25F, -0.5F};
        return authority.owner.pcm_preparer().prepare(body_pcm_input{.samples = samples,
            .frame_count = samples.size(),
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
    }

    inputnext::test::output::authority_fixture authority;
    audio_instance audio{};
    public_context context{};
    body_receipt body{};
};

TEST_CASE("output.body_playback_commands_keep_replace_and_stop_without_replay",
    "[component][output][body_playback]") {
    authority_playback_fixture fixture{};
    auto prepared = fixture.prepare();
    REQUIRE(prepared.has_value());
    const auto& same_handle = prepared.value();

    auto first = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(1U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = same_handle});
    REQUIRE(first.has_value());
    fixture.body = first->body;
    REQUIRE(first->playback.has_value());
    const auto original_clip = take_optional_value(first->playback);
    CHECK(original_clip.clip_revision == 1U);

    auto kept = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{});
    REQUIRE(kept.has_value());
    fixture.body = kept->body;
    CHECK(kept->playback == original_clip);

    auto replaced = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(3U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = same_handle});
    REQUIRE(replaced.has_value());
    fixture.body = replaced->body;
    REQUIRE(replaced->playback.has_value());
    const auto replacement_clip = take_optional_value(replaced->playback);
    CHECK(replacement_clip.lease == original_clip.lease);
    CHECK(replacement_clip.clip_revision == original_clip.clip_revision + 1U);

    auto stopped = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(4U),
        body_playback_update{.command = body_pcm_command::stop, .replacement = {}});
    REQUIRE(stopped.has_value());
    fixture.body = stopped->body;
    CHECK_FALSE(stopped->playback.has_value());

    auto kept_after_stop = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(5U),
        body_playback_update{});
    REQUIRE(kept_after_stop.has_value());
    CHECK_FALSE(kept_after_stop->playback.has_value());
}

TEST_CASE(
    "output.body_playback_replacement_validation_is_atomic_and_counter_exhaustion_keeps_cleanup",
    "[component][output][body_playback]") {
    authority_playback_fixture fixture{};
    auto owned = fixture.prepare();
    REQUIRE(owned.has_value());
    const auto& owned_handle = owned.value();

    const body_pcm_preparer foreign_preparer{};
    constexpr std::array foreign_samples{0.75F};
    auto foreign = foreign_preparer.prepare(body_pcm_input{.samples = foreign_samples,
        .frame_count = foreign_samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    REQUIRE(foreign.has_value());

    const auto version_before = authority_test_access::logical_version(fixture.authority.owner);
    const auto lease_before = authority_test_access::next_body_lease_id(fixture.authority.owner);
    auto rejected = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(1U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = foreign.value()});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::invalid_argument);
    CHECK(authority_test_access::logical_version(fixture.authority.owner) == version_before);
    CHECK(authority_test_access::next_body_lease_id(fixture.authority.owner) == lease_before);

    auto accepted = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(1U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = owned_handle});
    REQUIRE(accepted.has_value());
    fixture.body = accepted->body;
    REQUIRE(accepted->playback.has_value());
    const auto original_clip = take_optional_value(accepted->playback);

    authority_test_access::set_next_body_playback_revision(fixture.authority.owner,
        std::numeric_limits<std::uint64_t>::max());
    auto maximum = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = owned_handle});
    REQUIRE(maximum.has_value());
    REQUIRE(maximum->playback.has_value());
    const auto maximum_clip = take_optional_value(maximum->playback);
    CHECK(maximum_clip.lease == original_clip.lease);
    CHECK(maximum_clip.clip_revision == std::numeric_limits<std::uint64_t>::max());

    const auto version_before_exhausted =
        authority_test_access::logical_version(fixture.authority.owner);
    const auto lease_before_exhausted =
        authority_test_access::next_body_lease_id(fixture.authority.owner);
    auto exhausted = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(3U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = owned_handle});
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error() == error::identity_exhausted);
    CHECK(authority_test_access::logical_version(fixture.authority.owner) ==
          version_before_exhausted);
    CHECK(authority_test_access::next_body_lease_id(fixture.authority.owner) ==
          lease_before_exhausted);

    auto kept = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(3U),
        body_playback_update{});
    REQUIRE(kept.has_value());
    fixture.body = kept->body;
    CHECK(kept->playback == maximum_clip);

    auto stopped = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(4U),
        body_playback_update{.command = body_pcm_command::stop, .replacement = {}});
    REQUIRE(stopped.has_value());
    fixture.body = stopped->body;
    CHECK_FALSE(stopped->playback.has_value());
}

TEST_CASE("output.body_playback_headroom_failure_preserves_clip_sequence_and_revision",
    "[component][output][body_playback]") {
    authority_playback_fixture fixture{};
    auto prepared = fixture.prepare();
    REQUIRE(prepared.has_value());
    const auto& replacement = prepared.value();
    auto first = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(1U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = replacement});
    REQUIRE(first.has_value());
    fixture.body = first->body;
    REQUIRE(first->playback.has_value());
    const auto first_identity = take_optional_value(first->playback);

    const auto exhausted_version = std::numeric_limits<std::uint64_t>::max() - 5U;
    authority_test_access::set_logical_version(fixture.authority.owner, exhausted_version);
    auto rejected = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = replacement});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::logical_version_exhausted);
    CHECK(authority_test_access::logical_version(fixture.authority.owner) == exhausted_version);

    auto observed =
        authority_test_access::observe_record(fixture.authority.owner, fixture.context.identity);
    REQUIRE(observed.has_value());
    const auto observation = take_optional_value(observed);
    REQUIRE(observation.body.has_value());
    CHECK(inputnext::test::required_value(observation.body).accepted_sequence == 1U);
    CHECK(observation.playback == first_identity);

    authority_test_access::set_logical_version(fixture.authority.owner, 0U);
    auto retried = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = body_pcm_command::replace, .replacement = replacement});
    REQUIRE(retried.has_value());
    REQUIRE(retried->playback.has_value());
    const auto retried_identity = take_optional_value(retried->playback);
    CHECK(retried_identity.lease == first_identity.lease);
    CHECK(retried_identity.clip_revision == first_identity.clip_revision + 1U);
}

TEST_CASE("output.body_playback_rejects_replacement_payload_for_keep_and_stop",
    "[component][output][body_playback]") {
    authority_playback_fixture fixture{};
    auto prepared = fixture.prepare();
    REQUIRE(prepared.has_value());
    auto accepted = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(1U),
        body_playback_update{.command = body_pcm_command::replace,
            .replacement = std::move(*prepared)});
    REQUIRE(accepted.has_value());
    fixture.body = accepted->body;

    auto extra = fixture.prepare();
    REQUIRE(extra.has_value());

    auto bad_keep = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = body_pcm_command::keep, .replacement = std::move(*extra)});
    REQUIRE_FALSE(bad_keep.has_value());
    CHECK(bad_keep.error() == error::invalid_argument);

    auto stop_extra = fixture.prepare();
    REQUIRE(stop_extra.has_value());
    auto bad_stop = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = body_pcm_command::stop,
            .replacement = std::move(*stop_extra)});
    REQUIRE_FALSE(bad_stop.has_value());
    CHECK(bad_stop.error() == error::invalid_argument);

    auto bad_command = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{.command = std::bit_cast<body_pcm_command>(std::uint8_t{255U}),
            .replacement = {}});
    REQUIRE_FALSE(bad_command.has_value());
    CHECK(bad_command.error() == error::invalid_argument);

    auto kept = fixture.authority.owner.submit_body(fixture.context,
        fixture.request(2U),
        body_playback_update{});
    REQUIRE(kept.has_value());
    CHECK(kept->playback == accepted->playback);
}

} // namespace
