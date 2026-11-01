#include "connection_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput_probe/output/dualsense_usb/evidence.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace inputnext::gameinput_detail {
namespace {

using namespace test_support;

TEST_CASE("gameinput.output.live_progress_excludes_stale_neutral_during_blocked_send",
    "[gameinput][output][integration][dualsense][evidence][concurrency]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start());
    REQUIRE(wait_until([&] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && !progress->write_in_flight &&
               progress->last_completed_attempt.has_value() &&
               progress->delivery == output::delivery_state::setter_accepted;
    }));
    const auto baseline = checked_value(fixture.connections->backend_progress(fixture.target));
    const auto neutral_attempt = checked_value(baseline.last_completed_attempt);
    const dualsense_usb_match known_neutral{.backend = baseline.backend,
        .native_output = neutral_attempt.output,
        .publication = neutral_attempt.publication,
        .attempt = neutral_attempt.id,
        .result = neutral_attempt.result};

    fixture.backing.metadata->block_next_queries(1U);
    scoped_query_release release_query{*fixture.backing.metadata, 1U};
    fixture.backing.connect(handle(fixture.backing.device_memory[1U]));
    fixture.backing.maintain();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));

    auto& authority = fixture.composition->public_authority();
    const auto context = authority.open(fixture.target);
    REQUIRE(context.has_value());
    fixture.raw_controls->block_next_send();
    REQUIRE(authority
            .submit(*context,
                {.selected_channels = output::supported_channel_mask,
                    .sequence = 1U,
                    .ttl = std::chrono::seconds{5},
                    .values = {.rumble = {.low_frequency = 0.75F}}})
            .has_value());
    fixture.output_executor->request_drive();
    REQUIRE(fixture.raw_controls->wait_for_send());
    REQUIRE(authority
            .submit(*context,
                {.selected_channels = output::supported_channel_mask,
                    .sequence = 2U,
                    .ttl = std::chrono::seconds{5},
                    .values = {}})
            .has_value());
    fixture.output_executor->request_drive();

    const auto cached = checked_value(fixture.connections->backend_progress(fixture.target));
    CHECK((!cached.write_in_flight));
    CHECK(cached.last_attempt == neutral_attempt.id);
    const auto live = checked_value(fixture.connections->query_backend_progress(fixture.target));
    CHECK(live.backend == baseline.backend);
    REQUIRE(live.write_in_flight);
    CHECK(live.last_attempt != neutral_attempt.id);
    const auto logical = authority.read_snapshot(fixture.target);
    REQUIRE(logical.has_value());
    const dualsense_usb_expectation expected{.logical_channels = output::supported_channel_mask,
        .logical_values = {},
        .native_output = {.channels = output::supported_channel_mask, .values = {}}};
    dualsense_usb_match matched{};
    CHECK(assess_dualsense_usb_evidence(baseline.backend,
              neutral_attempt.id,
              expected,
              live,
              *logical,
              known_neutral,
              matched) == dualsense_usb_evidence::pending);

    fixture.raw_controls->release_send();
    REQUIRE(wait_until([&] {
        const auto current = fixture.connections->query_backend_progress(fixture.target);
        const auto current_logical = authority.read_snapshot(fixture.target);
        return current.has_value() && current_logical.has_value() &&
               assess_dualsense_usb_evidence(baseline.backend,
                   neutral_attempt.id,
                   expected,
                   *current,
                   *current_logical,
                   known_neutral,
                   matched) == dualsense_usb_evidence::accepted_newer_attempt;
    }));
    CHECK(matched.attempt != neutral_attempt.id);
    const auto closed = authority.close(*context, context->identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(authority.acknowledge(*closed).has_value());
    release_query.release();
    fixture.connections->shutdown();
    CHECK((!fixture.connections->query_backend_progress(fixture.target).has_value()));
}

} // namespace
} // namespace inputnext::gameinput_detail
