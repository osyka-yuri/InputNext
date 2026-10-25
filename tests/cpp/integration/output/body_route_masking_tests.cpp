#include "body_route_manual_fixture.hpp"
#include "output/release_blocked_write.hpp"
#include "values/required_value.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::required_value;
using inputnext::test::output::manual_body_route_fixture;
using inputnext::test::output::release_blocked_write;

[[nodiscard]] submit_request rumble_request(const std::uint64_t sequence,
    const std::chrono::milliseconds ttl,
    const rumble_value value) noexcept {
    submit_request request;
    request.selected_channels = rumble_mask;
    request.sequence = sequence;
    request.ttl = ttl;
    request.values.rumble = value;
    return request;
}

TEST_CASE("output.body_route_hidden_rumble_value_provenance_and_expiry_do_not_publish",
    "[integration][output][body_route]") {
    using clock = std::chrono::steady_clock;
    std::atomic<std::int64_t> ticks{};
    manual_body_route_fixture fixture{&ticks};

    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto acknowledgement = fixture.accepted_ack();
    const auto initial_progress = fixture.progress();
    REQUIRE(initial_progress.has_value());
    const auto initial_publication = initial_progress->observed_publication;
    REQUIRE(fixture.native_state->call_count() == 1U);

    const auto first_rumble = fixture.submit(fixture.context,
        rumble_request(1U, 100ms, {.low_frequency = 0.2F, .high_frequency = 0.3F}));
    REQUIRE(first_rumble.has_value());
    const auto hidden_value = fixture.submit(fixture.context,
        rumble_request(2U, 100ms, {.low_frequency = 0.7F, .high_frequency = 0.8F}));
    REQUIRE(hidden_value.has_value());
    const auto alternate = fixture.authority.owner.open(fixture.authority.connection.target);
    REQUIRE(alternate.has_value());
    const auto same_value_new_provenance = fixture.submit(*alternate,
        rumble_request(1U, 100ms, {.low_frequency = 0.7F, .high_frequency = 0.8F}));
    REQUIRE(same_value_new_provenance.has_value());

    const auto unchanged = fixture.snapshot();
    REQUIRE(unchanged.has_value());
    REQUIRE(fixture.publish(*unchanged).has_value());
    auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->observed_publication == initial_publication);
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(fixture.native_state->call_count() == 1U);
    auto qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);
    CHECK(!(fixture.drive()));

    const auto removed_latest = fixture.clear_channel(*alternate,
        rumble_mask,
        2U,
        same_value_new_provenance->resulting_revisions);
    REQUIRE(removed_latest.has_value());
    const auto revealed_first = fixture.snapshot();
    REQUIRE(revealed_first.has_value());
    REQUIRE(fixture.publish(*revealed_first).has_value());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->observed_publication == initial_publication);
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(fixture.native_state->call_count() == 1U);
    qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);

    ticks.store(std::chrono::duration_cast<clock::duration>(100ms).count(),
        std::memory_order_release);
    const auto expired_lower = fixture.snapshot();
    REQUIRE(expired_lower.has_value());
    REQUIRE(expired_lower->body.has_value());
    CHECK(!(expired_lower->rumble.has_value()));
    REQUIRE(fixture.publish(*expired_lower).has_value());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->observed_publication == initial_publication);
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(fixture.native_state->call_count() == 1U);
    qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);
}

TEST_CASE("output.body_route_clear_restores_latest_hidden_rumble",
    "[integration][output][body_route]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto acknowledgement = fixture.accepted_ack();

    const rumble_value latest{.low_frequency = 0.25F, .high_frequency = 0.75F};
    REQUIRE(fixture.submit(fixture.context, rumble_request(1U, 5s, latest)).has_value());
    const auto hidden = fixture.snapshot();
    REQUIRE(hidden.has_value());
    REQUIRE(fixture.publish(*hidden).has_value());
    auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(fixture.native_state->call_count() == 1U);

    REQUIRE(fixture.clear_body(2U).has_value());
    const auto restored_snapshot = fixture.snapshot();
    REQUIRE(restored_snapshot.has_value());
    CHECK(!(restored_snapshot->body.has_value()));
    REQUIRE(restored_snapshot->rumble.has_value());
    CHECK(required_value(restored_snapshot->rumble).value == latest);
    REQUIRE(fixture.publish(*restored_snapshot).has_value());
    REQUIRE(fixture.drive());
    CHECK(fixture.native_state->call_count() == 2U);
    const auto restored = fixture.native_state->call(1U);
    CHECK(!(restored.output.body.has_value()));
    CHECK(restored.output.values.rumble == latest);
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(!(progress->acknowledged_body_route.has_value()));
}

TEST_CASE("output.body_route_expiry_restores_latest_hidden_rumble",
    "[integration][output][body_route]") {
    using clock = std::chrono::steady_clock;
    std::atomic<std::int64_t> ticks{};
    manual_body_route_fixture fixture{&ticks};
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto acknowledgement = fixture.accepted_ack();
    REQUIRE(fixture
            .submit(fixture.context,
                rumble_request(1U, 20s, {.low_frequency = 0.1F, .high_frequency = 0.2F}))
            .has_value());
    const rumble_value latest{.low_frequency = 0.55F, .high_frequency = 0.65F};
    REQUIRE(fixture.submit(fixture.context, rumble_request(2U, 20s, latest)).has_value());
    const auto hidden = fixture.snapshot();
    REQUIRE(hidden.has_value());
    REQUIRE(fixture.publish(*hidden).has_value());
    CHECK(!fixture.drive());
    CHECK(fixture.accepted_ack() == acknowledgement);
    ticks.store(std::chrono::duration_cast<clock::duration>(5s + 1ms).count(),
        std::memory_order_release);
    const auto expired = fixture.snapshot();
    REQUIRE(expired.has_value());
    CHECK(!expired->body.has_value());
    REQUIRE(expired->rumble.has_value());
    CHECK(required_value(expired->rumble).value == latest);
    REQUIRE(fixture.publish(*expired).has_value());
    REQUIRE(fixture.drive());
    REQUIRE(fixture.native_state->call_count() == 2U);
    const auto restored = fixture.native_state->call(1U);
    CHECK(!restored.output.body.has_value());
    CHECK(restored.output.values.rumble == latest);
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(!progress->acknowledged_body_route.has_value());
}

TEST_CASE("output.body_route_initial_blocked_write_ignores_hidden_rumble",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    fixture.native_state->block_next_write();
    bool driven = false;
    std::jthread writer{[&] { driven = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());
    const auto command = fixture.native_state->call(0U);
    for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
        const float amplitude = static_cast<float>(sequence) * 0.2F;
        REQUIRE(fixture
                .submit(fixture.context,
                    rumble_request(sequence,
                        5s,
                        {.low_frequency = amplitude, .high_frequency = amplitude}))
                .has_value());
        const auto snapshot = fixture.snapshot();
        REQUIRE(snapshot.has_value());
        REQUIRE(fixture.publish(*snapshot).has_value());
        const auto pending = fixture.progress();
        REQUIRE(pending.has_value());
        CHECK(pending->observed_publication == command.publication);
        CHECK(!pending->acknowledged_body_route.has_value());
        CHECK(fixture.native_state->call_count() == 1U);
    }
    fixture.native_state->release_write();
    writer.join();
    CHECK(driven);
    const auto acknowledgement = fixture.accepted_ack();
    CHECK(acknowledgement.publication == command.publication);
    CHECK(acknowledgement.attempt == command.attempt);
    const auto qualified = fixture.qualifies(acknowledgement);
    REQUIRE(qualified.has_value());
    CHECK(*qualified);
    CHECK(!fixture.drive());
    CHECK(fixture.native_state->call_count() == 1U);
}

} // namespace
