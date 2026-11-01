#include "runtime/abi/effects/projection.hpp"

#include "effects/effects.hpp"
#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "values/required_value.hpp"

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::runtime_detail::effects_projection;
using inputnext::test::required_value;

[[nodiscard]] input_next_effects_context_v1 context_value(std::uint64_t id = 7U) {
    input_next_effects_context_v1 value{};
    value.struct_size = sizeof(value);
    value.runtime_id[0U] = 0x51U;
    value.id = id;
    return value;
}

[[nodiscard]] input_next_effects_request_v1 valid_discrete_request() {
    input_next_effects_request_v1 value{};
    value.struct_size = sizeof(value);
    value.channel_mask = INPUT_NEXT_EFFECTS_LEFT_TRIGGER;
    value.ttl_ms = 250U;
    value.context = context_value();
    value.mutation_sequence = 1U;
    value.left_trigger = {.mode = INPUT_NEXT_EFFECTS_TRIGGER_FEEDBACK,
        .start = 0.2F,
        .end = 1.0F,
        .strength = 0.5F,
        .frequency = 0.0F};
    value.right_trigger = {.mode = INPUT_NEXT_EFFECTS_TRIGGER_OFF,
        .start = 0.0F,
        .end = 0.0F,
        .strength = 0.0F,
        .frequency = 0.0F};
    return value;
}

[[nodiscard]] input_next_effects_body_request_v1 valid_body_request() {
    input_next_effects_body_request_v1 value{};
    value.struct_size = sizeof(value);
    value.ttl_ms = 250U;
    value.route_preference = INPUT_NEXT_EFFECTS_BODY_ROUTE_CLASSIC;
    value.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_KEEP;
    value.context = context_value();
    value.sequence = 1U;
    value.pcm.struct_size = sizeof(value.pcm);
    return value;
}

TEST_CASE("effects.abi.status_projection_is_explicit", "[unit][effects][abi]") {
    constexpr std::array cases{
        std::pair{effects::status::ok, INPUT_NEXT_STATUS_OK},
        std::pair{effects::status::invalid_argument, INPUT_NEXT_STATUS_INVALID_ARGUMENT},
        std::pair{effects::status::unavailable, INPUT_NEXT_STATUS_UNAVAILABLE},
        std::pair{effects::status::stale_target, INPUT_NEXT_STATUS_STALE_TARGET},
        std::pair{effects::status::unsupported_target, INPUT_NEXT_STATUS_UNSUPPORTED_TARGET},
        std::pair{effects::status::unsupported_channel, INPUT_NEXT_STATUS_UNSUPPORTED_CHANNEL},
        std::pair{effects::status::scheduler_unavailable, INPUT_NEXT_STATUS_SCHEDULER_UNAVAILABLE},
        std::pair{effects::status::busy, INPUT_NEXT_STATUS_BUSY},
        std::pair{effects::status::stale_lease, INPUT_NEXT_STATUS_STALE_LEASE},
        std::pair{effects::status::capacity_exceeded, INPUT_NEXT_STATUS_CAPACITY_EXCEEDED},
        std::pair{effects::status::stale_context, INPUT_NEXT_STATUS_STALE_CONTEXT},
        std::pair{effects::status::stale_effect, INPUT_NEXT_STATUS_STALE_EFFECT},
        std::pair{effects::status::stale_sequence, INPUT_NEXT_STATUS_STALE_SEQUENCE},
        std::pair{effects::status::source_retired, INPUT_NEXT_STATUS_SOURCE_RETIRED},
        std::pair{effects::status::unsupported, INPUT_NEXT_STATUS_UNSUPPORTED},
        std::pair{effects::status::io_failed, INPUT_NEXT_STATUS_IO_FAILED},
        std::pair{effects::status::revoked, INPUT_NEXT_STATUS_REVOKED},
        std::pair{effects::status::pending, INPUT_NEXT_STATUS_PENDING},
        std::pair{effects::status::no_data, INPUT_NEXT_STATUS_NO_DATA},
        std::pair{effects::status::not_admitted, INPUT_NEXT_STATUS_NOT_ADMITTED},
    };

    for (const auto& [typed, c_status] : cases) {
        CHECK(to_c_status(typed) == c_status);
    }
}

TEST_CASE("effects.abi.discrete_parser_checks_inactive_values_and_all_reserved_fields",
    "[unit][effects][abi]") {
    const auto input = valid_discrete_request();
    effects::context_handle context{};
    output::submit_request parsed{};
    REQUIRE(parse_submit_request(input, context, parsed));
    CHECK(context.id == input.context.id);
    CHECK(context.runtime.valid());
    CHECK(parsed.selected_channels == output::left_trigger_mask);
    CHECK(parsed.values.left_trigger.mode == output::trigger_mode::feedback);
    CHECK(parsed.values.right_trigger.mode == output::trigger_mode::off);
    CHECK(parsed.ttl == 250ms);

    auto invalid = input;
    invalid.right_trigger.frequency = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.leds.reserved = 1U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.leds.microphone_led = 2U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.channel_mask |= 0x80000000U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.ttl_ms = 0U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.mutation_sequence = 0U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));

    invalid = input;
    invalid.context.reserved = 1U;
    CHECK_FALSE(parse_submit_request(invalid, context, parsed));
}

TEST_CASE("effects.abi.pcm_commands_translate_explicitly_and_budget_precedes_sample_scan",
    "[unit][effects][abi]") {
    effects::body_request parsed{};
    auto input = valid_body_request();
    REQUIRE(parse_body_request(input, parsed));
    CHECK(parsed.pcm_command == output::body_pcm_command::keep);
    CHECK_FALSE(parsed.replacement.has_value());

    input.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_STOP;
    REQUIRE(parse_body_request(input, parsed));
    CHECK(parsed.pcm_command == output::body_pcm_command::stop);

    std::array<float, 4U> samples{0.25F, -0.5F, 0.75F, -1.0F};
    input.pcm_command = INPUT_NEXT_EFFECTS_BODY_PCM_REPLACE;
    input.pcm = {.struct_size = sizeof(input.pcm),
        .frame_count = 4U,
        .channels = 1U,
        .sample_rate = output::body_pcm_sample_rate,
        .gain = 0.5F,
        .reserved = 0U,
        .samples = samples.data()};
    REQUIRE(parse_body_request(input, parsed));
    CHECK(parsed.pcm_command == output::body_pcm_command::replace);
    const auto replacement = required_value(parsed.replacement);
    CHECK(replacement.samples.size() == samples.size());
    CHECK(replacement.samples[2U] == samples[2U]);

    const auto over_budget_frame_count =
        (output::body_pcm_storage_budget_bytes / sizeof(float) / 2U) + 1U;
    const auto over_budget_sample_count = static_cast<std::size_t>(over_budget_frame_count * 2U);
    const std::vector<float> readable_over_budget_samples(over_budget_sample_count, 0.25F);
    input.pcm.frame_count = static_cast<std::uint32_t>(over_budget_frame_count);
    input.pcm.channels = 2U;
    input.pcm.samples = readable_over_budget_samples.data();
    CHECK_FALSE(parse_body_request(input, parsed));

    input = valid_body_request();
    input.pcm.struct_size = 0U;
    CHECK_FALSE(parse_body_request(input, parsed));
}

TEST_CASE("effects.abi.output_projection_preserves_status_domains_and_pcm_fallback_values",
    "[unit][effects][abi]") {
    effects::capabilities capabilities{};
    capabilities.supported_channels = output::supported_channel_mask;
    capabilities.features = effects::feature_flags::left_trigger |
                            effects::feature_flags::low_rumble |
                            effects::feature_flags::body_lease | effects::feature_flags::hd_ready;
    capabilities.completion = effects::completion_capabilities::raw_output_accepted_current;
    capabilities.audio_instance_id = 44U;
    capabilities.observation_revision = 5U;
    input_next_effects_capabilities_v1 c_capabilities{};
    REQUIRE(write_capabilities(capabilities, c_capabilities));
    CHECK(c_capabilities.supported_channels == INPUT_NEXT_EFFECTS_ALL_CHANNELS);
    CHECK(c_capabilities.audio_instance_id == 44U);
    CHECK(c_capabilities.capabilities_revision == 5U);
    CHECK((c_capabilities.feature_flags & INPUT_NEXT_EFFECTS_CAP_HD_READY) != 0U);
    CHECK(c_capabilities.completion_capabilities ==
          INPUT_NEXT_EFFECTS_COMPLETION_CAPABILITY_RAW_OUTPUT_ACCEPTED_CURRENT);

    effects::body_receipt receipt{};
    receipt.context.runtime.bytes[0U] = std::byte{0x51U};
    receipt.context.id = 9U;
    receipt.target.runtime.bytes[0U] = std::byte{0x51U};
    receipt.target.device.value = 2U;
    receipt.target.connection.value = 3U;
    receipt.lease_id = 6U;
    receipt.sequence = 4U;
    receipt.publication.value = 11U;
    receipt.route = output::body_actuator_route::classic;
    receipt.fallback = output::body_fallback_reason::preempted;
    input_next_effects_body_receipt_v1 c_receipt{};
    REQUIRE(write_body_receipt(receipt, c_receipt));
    CHECK(c_receipt.fallback_reason == INPUT_NEXT_EFFECTS_BODY_FALLBACK_PREEMPTED);
    CHECK(c_receipt.fallback_reason != INPUT_NEXT_EFFECTS_BODY_FALLBACK_PLAY_FAILED);
}

TEST_CASE("effects.abi.close_receipts_reject_client_only_cancelled_disposition",
    "[unit][effects][abi]") {
    input_next_effects_close_receipt_v1 input{};
    input.struct_size = sizeof(input);
    input.disposition = INPUT_NEXT_EFFECTS_CLOSE_NORMAL;
    input.context = context_value(13U);
    input.target.struct_size = sizeof(input.target);
    input.target.runtime_id[0U] = 0x51U;
    input.target.device_key = 2U;
    input.target.connection_id = 3U;
    input.close_request_id = 13U;
    input.publication_revision = 17U;
    effects::close_receipt parsed{};
    REQUIRE(parse_close_receipt(input, parsed));
    CHECK(parsed.disposition == effects::close_disposition::normal);

    input.disposition = INPUT_NEXT_EFFECTS_CLOSE_CANCELLED_BEFORE_CREATION;
    CHECK_FALSE(parse_close_receipt(input, parsed));
    input.disposition = INPUT_NEXT_EFFECTS_CLOSE_NORMAL;
    input.publication_revision = 0U;
    CHECK_FALSE(parse_close_receipt(input, parsed));
}

TEST_CASE("effects.abi.body_clear_requires_a_valid_lease_and_positive_sequence",
    "[unit][effects][abi]") {
    input_next_effects_body_clear_v1 input{};
    input.struct_size = sizeof(input);
    input.context = context_value();
    input.lease_id = 3U;
    input.sequence = 1U;

    effects::body_clear_request parsed{};
    REQUIRE(parse_body_clear_request(input, parsed));
    CHECK(parsed.lease_id == input.lease_id);
    CHECK(parsed.sequence == input.sequence);

    input.lease_id = 0U;
    CHECK_FALSE(parse_body_clear_request(input, parsed));
    input.lease_id = 3U;
    input.sequence = 0U;
    CHECK_FALSE(parse_body_clear_request(input, parsed));
    input.sequence = 1U;
    input.reserved = 1U;
    CHECK_FALSE(parse_body_clear_request(input, parsed));
}

} // namespace
