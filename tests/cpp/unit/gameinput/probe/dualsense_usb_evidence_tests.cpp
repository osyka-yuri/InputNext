#include "dualsense_usb_evidence.hpp"

#include "dualsense_usb_sequence.hpp"
#include "rumble_evidence.hpp"
#include "rumble_sequence.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] connection_target test_target() {
    runtime_id runtime{};
    runtime.bytes[0U] = std::byte{0x13};
    return {.runtime = runtime, .device = device_key{7U}, .connection = connection_id{9U}};
}

[[nodiscard]] output::backend_handle test_backend(const connection_target target) {
    output::authority_incarnation authority{};
    authority.bytes[0U] = std::byte{0x31};
    return {.target = target,
        .incarnation = {.owner = {.authority = authority, .id = output::execution_id{2U}},
            .id = output::backend_id{3U}}};
}

[[nodiscard]] dualsense_usb_expectation test_expectation(const output::channel_values& values) {
    return {.logical_channels = output::supported_channel_mask,
        .logical_values = values,
        .native_output = {.channels = output::supported_channel_mask, .values = values}};
}

[[nodiscard]] output::logical_snapshot test_logical(const connection_target target,
    const dualsense_usb_expectation& expected) {
    output::logical_snapshot logical{};
    logical.target = target;
    logical.incarnation = test_backend(target).incarnation.owner.authority;
    logical.logical_version = 11U;
    if ((expected.logical_channels & output::left_trigger_mask) != 0U) {
        logical.left_trigger = output::channel_snapshot<output::trigger_value>{
            .value = expected.logical_values.left_trigger,
            .provenance = {.source_channel = output::channel::left_trigger}};
    }
    if ((expected.logical_channels & output::right_trigger_mask) != 0U) {
        logical.right_trigger = output::channel_snapshot<output::trigger_value>{
            .value = expected.logical_values.right_trigger,
            .provenance = {.source_channel = output::channel::right_trigger}};
    }
    if ((expected.logical_channels & output::rumble_mask) != 0U) {
        logical.rumble =
            output::channel_snapshot<output::rumble_value>{.value = expected.logical_values.rumble,
                .provenance = {.source_channel = output::channel::rumble}};
    }
    if ((expected.logical_channels & output::leds_mask) != 0U) {
        logical.leds =
            output::channel_snapshot<output::led_value>{.value = expected.logical_values.leds,
                .provenance = {.source_channel = output::channel::leds}};
    }
    return logical;
}

struct evidence_fixture final {
    connection_target target{test_target()};
    output::backend_handle backend{test_backend(target)};
    output::channel_values values{.left_trigger = {.mode = output::trigger_mode::feedback,
                                      .layout = output::trigger_layout::uniform,
                                      .start = 0.2F,
                                      .end = 1.0F,
                                      .strength = 0.75F},
        .rumble = {.low_frequency = 0.4F, .high_frequency = 0.2F},
        .leds = {.rgb = {1U, 2U, 3U}, .player_leds = 2U, .microphone_led = true}};
    dualsense_usb_expectation expected{test_expectation(values)};
    output::logical_snapshot logical{test_logical(target, expected)};
    output::backend_progress progress{};
    output::completed_attempt completed{};

    evidence_fixture() {
        completed = {.id = output::attempt_id{9U},
            .publication = output::publication_id{4U},
            .result = {.status = output::native_status::accepted, .native_code = 0},
            .output = expected.native_output};
        progress = {.backend = backend,
            .phase = output::backend_phase::current,
            .represented_channels = output::supported_channel_mask,
            .unsupported_channels = 0U,
            .observed_publication = output::publication_id{4U},
            .delivery = output::delivery_state::setter_accepted,
            .last_attempt = completed.id,
            .last_completed_attempt = completed,
            .write_in_flight = false};
    }
};

[[nodiscard]] const dualsense_usb_step& find_step(const std::string_view name) {
    const auto steps = dualsense_usb_steps();
    const auto found = std::ranges::find(steps, name, &dualsense_usb_step::name);
    if (found == steps.end()) {
        throw std::runtime_error("The required USB diagnostic step is missing.");
    }
    return *found;
}

TEST_CASE("gameinput.output.dualsense_usb.sequence_covers_leds_rumble_and_both_triggers",
    "[gameinput][output][unit][dualsense_usb]") {
    const auto steps = dualsense_usb_steps();
    REQUIRE(steps.size() == 20U);
    for (const auto& step : steps) {
        CAPTURE(step.name);
        CHECK((!step.instruction.empty()));
    }
    constexpr std::array trigger_fields{&output::channel_values::left_trigger,
        &output::channel_values::right_trigger};
    constexpr std::array effects{
        std::pair{output::trigger_mode::feedback, output::trigger_layout::uniform},
        std::pair{output::trigger_mode::feedback, output::trigger_layout::per_zone},
        std::pair{output::trigger_mode::weapon, output::trigger_layout::uniform},
        std::pair{output::trigger_mode::vibration, output::trigger_layout::uniform},
        std::pair{output::trigger_mode::vibration, output::trigger_layout::per_zone}};
    for (const auto field : trigger_fields) {
        for (const auto& [mode, layout] : effects) {
            CAPTURE(mode, layout);
            CHECK(std::ranges::any_of(steps, [&](const auto& step) {
                const auto& trigger = step.values.*field;
                return trigger.mode == mode && trigger.layout == layout;
            }));
        }
    }
    CHECK(find_step("left_trigger_off").values.left_trigger.mode == output::trigger_mode::off);
    CHECK(find_step("right_trigger_off").values.right_trigger.mode == output::trigger_mode::off);
    CHECK(find_step("rumble_low_motor").values.rumble.low_frequency > 0.0F);
    CHECK(find_step("rumble_low_motor").values.rumble.high_frequency == 0.0F);
    CHECK(find_step("rumble_high_motor").values.rumble.high_frequency > 0.0F);
    CHECK(find_step("rumble_high_motor").values.rumble.low_frequency == 0.0F);
    CHECK((find_step("lightbar_rgb").values.leds.rgb != std::array<std::uint8_t, 3U>{}));
    CHECK(find_step("player_leds").values.leds.player_leds != 0U);
    CHECK(find_step("microphone_led_on").values.leds.microphone_led);
    CHECK(steps.back().name == "final_neutral");
    CHECK(steps.back().values == dualsense_usb_neutral_values());
}

TEST_CASE("gameinput.output.dualsense_usb.evidence_requires_matching_current_full_output",
    "[gameinput][output][unit][dualsense_usb][evidence]") {
    evidence_fixture fixture;
    dualsense_usb_match matched{};

    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              fixture.progress,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::accepted_newer_attempt);
    CHECK(matched.native_output == fixture.expected.native_output);
    CHECK(matched.result.status == output::native_status::accepted);

    auto wrong_backend = fixture.progress;
    wrong_backend.represented_channels = output::rumble_mask;
    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              wrong_backend,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::rejected);

    auto wrong_payload = fixture.progress;
    auto altered = fixture.completed;
    altered.output.values.leds.rgb[0U] = 99U;
    wrong_payload.last_completed_attempt = altered;
    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              wrong_payload,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::pending);

    auto stale_logical = fixture.logical;
    stale_logical.rumble = output::channel_snapshot<output::rumble_value>{
        .value = {.low_frequency = 0.4F, .high_frequency = 0.8F},
        .provenance = {}};
    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              fixture.progress,
              stale_logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::pending);

    auto in_flight = fixture.progress;
    in_flight.last_attempt = output::attempt_id{10U};
    in_flight.write_in_flight = true;
    in_flight.delivery = output::delivery_state::in_flight;
    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              in_flight,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::pending);

    auto failed = fixture.progress;
    altered = fixture.completed;
    altered.result.status = output::native_status::unsupported;
    failed.last_completed_attempt = altered;
    failed.delivery = output::delivery_state::blocked;
    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              output::attempt_id{8U},
              fixture.expected,
              failed,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::rejected);
}

TEST_CASE("gameinput.output.dualsense_usb.evidence_marks_known_matching_attempt_as_reuse",
    "[gameinput][output][unit][dualsense_usb][evidence]") {
    evidence_fixture fixture;
    const dualsense_usb_match previous{.backend = fixture.backend,
        .native_output = fixture.completed.output,
        .publication = fixture.completed.publication,
        .attempt = fixture.completed.id,
        .result = fixture.completed.result};
    fixture.progress.observed_publication = output::publication_id{5U};
    dualsense_usb_match matched{};

    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              fixture.completed.id,
              fixture.expected,
              fixture.progress,
              fixture.logical,
              previous,
              matched) == dualsense_usb_evidence::historical_reuse);
    CHECK(matched == previous);

    CHECK(assess_dualsense_usb_evidence(fixture.backend,
              fixture.completed.id,
              fixture.expected,
              fixture.progress,
              fixture.logical,
              std::nullopt,
              matched) == dualsense_usb_evidence::pending);
}

TEST_CASE("gameinput.output.rumble.evidence_accepts_exact_full_usb_output",
    "[gameinput][output][unit][rumble][evidence]") {
    const auto target = test_target();
    const auto backend = test_backend(target);
    const rumble_step step{.name = "low_motor",
        .action = rumble_step_action::set_state,
        .left_motor = 19'660U,
        .right_motor = 0U,
        .expects_native_output = true,
        .logical_channel_present = true,
        .instruction = "Feel the low-frequency vibration."};
    const auto expected_rumble = normalized_value(step);
    output::native_output expected_output{.channels = output::supported_channel_mask};
    expected_output.values.rumble = expected_rumble;
    const output::completed_attempt completed{.id = output::attempt_id{9U},
        .publication = output::publication_id{4U},
        .result = {.status = output::native_status::accepted},
        .output = expected_output};
    const output::backend_progress progress{.backend = backend,
        .phase = output::backend_phase::current,
        .represented_channels = output::supported_channel_mask,
        .observed_publication = output::publication_id{4U},
        .delivery = output::delivery_state::setter_accepted,
        .last_attempt = completed.id,
        .last_completed_attempt = completed};
    const rumble_native_observation observation{.progress = progress,
        .logical_channel_present = true,
        .logical_value = expected_rumble};
    rumble_native_match matched{};

    CHECK(assess_native_evidence(step,
              backend,
              output::attempt_id{8U},
              observation,
              std::nullopt,
              matched) == rumble_native_evidence::accepted_newer_attempt);

    auto changed_other_channel = observation;
    changed_other_channel.progress.last_completed_attempt->output.values.leds.rgb[0U] = 1U;
    CHECK(assess_native_evidence(step,
              backend,
              output::attempt_id{8U},
              changed_other_channel,
              std::nullopt,
              matched) == rumble_native_evidence::pending);

    auto partial = observation;
    partial.progress.represented_channels = output::rumble_mask | output::leds_mask;
    CHECK(assess_native_evidence(step,
              backend,
              output::attempt_id{8U},
              partial,
              std::nullopt,
              matched) == rumble_native_evidence::rejected);

    auto unsupported = observation;
    unsupported.progress.unsupported_channels = output::leds_mask;
    CHECK(assess_native_evidence(step,
              backend,
              output::attempt_id{8U},
              unsupported,
              std::nullopt,
              matched) == rumble_native_evidence::rejected);
}

} // namespace
} // namespace inputnext::gameinput_detail
