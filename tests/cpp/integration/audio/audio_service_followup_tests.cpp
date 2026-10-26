#include "audio/audio_service.hpp"
#include "audio/controlled_audio_worker.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "audio_service_test_support.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace inputnext::audio_integration_test_support {
namespace {

using namespace std::chrono_literals;
using namespace audio_detail;

[[nodiscard]] std::optional<audio_start_failure>
find_start_failure(const audio_service_progress& progress, const connection_target target) {
    for (std::size_t index = 0U; index < progress.start_failure_count; ++index) {
        if (progress.start_failures[index].target == target) {
            return progress.start_failures[index];
        }
    }
    return std::nullopt;
}

TEST_CASE("audio.service.skips_classic_backend_then_starts_after_hd_replacement",
    "[integration][audio][backend-selection]") {
    auto options = audio_service_fixture_options{};
    options.initial_model = metadata_device_model::generic_gamepad;
    const auto workers = std::make_shared<test::audio::controlled_audio_worker_state>();
    options.worker_state = workers;
    audio_service_fixture fixture{std::move(options)};

    const auto classic =
        required_value(fixture.graph.connections->query_backend_progress(fixture.graph.target),
            "classic backend progress disappeared");
    REQUIRE(classic.phase == output::backend_phase::current);
    CHECK(classic.body_routes == output::body_route_support::classic_only);
    CHECK(fixture.native_factory->create_attempt_count() == 0U);
    CHECK(workers->created_workers() == 0U);
    REQUIRE(fixture.native_states.front() != nullptr);
    CHECK(fixture.native_states.front()->call_count() == 0U);

    const auto inventory = fixture.graph.backing.runtime->connections();
    for (std::size_t index = 0U; index < 5U; ++index) {
        REQUIRE(fixture.synchronize_inventory(inventory));
        std::this_thread::sleep_for(80ms);
        CHECK(fixture.native_factory->create_attempt_count() == 0U);
        CHECK(workers->created_workers() == 0U);
    }

    fixture.set_dualsense_usb();
    REQUIRE(wait_until([&fixture, previous = classic.backend] {
        const auto current =
            fixture.graph.connections->query_backend_progress(fixture.graph.target);
        return current.has_value() && current->phase == output::backend_phase::current &&
               current->backend != previous &&
               current->body_routes == output::body_route_support::hd;
    }));
    REQUIRE(fixture.wait_for_live());

    const auto hd_progress =
        required_value(fixture.graph.connections->query_backend_progress(fixture.graph.target),
            "HD backend progress disappeared");
    CHECK(hd_progress.body_routes == output::body_route_support::hd);
    CHECK(fixture.native_factory->create_attempt_count() == 1U);
    CHECK(workers->created_workers() == 1U);
    CHECK(fixture.native_states.front()->count_calls(native_operation::begin_worker_sta) == 1U);
}

void check_factory_failure(const audio_service_fixture& fixture,
    const test::audio::controlled_factory_creation_mode mode) {
    const auto snapshot = fixture.service->progress();
    const auto failure = required_value(find_start_failure(snapshot, fixture.graph.target),
        "factory failure disappeared during retry");
    const auto backend =
        required_value(fixture.graph.connections->query_backend_progress(fixture.graph.target),
            "audio fixture backend progress disappeared");
    CHECK(snapshot.start_failure_count == 1U);
    CHECK(failure.backend == backend.backend);
    CHECK(failure.error.operation == native_operation::create_native_port);
    CHECK(failure.error.native_code ==
          (mode == test::audio::controlled_factory_creation_mode::fail ? 73U : 0U));
    CHECK(snapshot.resident_slots == 0U);
    CHECK(fixture.native_factory->created_count() == 0U);

    const auto target_progress = required_value(fixture.service->progress(fixture.graph.target),
        "target factory failure progress disappeared");
    CHECK(target_progress.phase == audio_target_phase::waiting_for_retry);
    CHECK(target_progress.backend == failure.backend);
    CHECK(target_progress.last_native_operation == native_operation::create_native_port);
    CHECK(target_progress.last_native_error == failure.error);
}

void check_factory_recovery(const audio_service_fixture& fixture) {
    const auto recovered = fixture.service->progress();
    CHECK(recovered.start_failure_count == 0U);
    CHECK(recovered.resident_slots == 1U);
    const auto live_progress = required_value(fixture.service->progress(fixture.graph.target),
        "recovered target progress disappeared");
    CHECK(live_progress.phase == audio_target_phase::live);
    CHECK(!live_progress.last_native_error.has_value());
    CHECK(live_progress.last_native_operation == native_operation::none);
}

TEST_CASE("audio.service.retains_factory_failures_across_retries_and_recovers",
    "[integration][audio][retry]") {
    constexpr std::array modes{test::audio::controlled_factory_creation_mode::fail,
        test::audio::controlled_factory_creation_mode::succeed_null};
    for (const auto mode : modes) {
        auto options = audio_service_fixture_options{};
        options.factory_creation_mode = mode;
        options.factory_failure_code = 73U;
        audio_service_fixture fixture{std::move(options)};

        REQUIRE(wait_until(
            [&fixture] { return fixture.service->progress(fixture.graph.target).has_value(); }));
        REQUIRE(wait_until(
            [&fixture] { return fixture.native_factory->create_attempt_count() >= 2U; }));
        check_factory_failure(fixture, mode);

        fixture.native_factory->clear_creation_mode();
        REQUIRE(fixture.wait_for_live());
        check_factory_recovery(fixture);
    }
}

TEST_CASE("audio.service.removing_target_clears_retained_factory_failure",
    "[integration][audio][retry]") {
    auto options = audio_service_fixture_options{};
    options.factory_creation_mode = test::audio::controlled_factory_creation_mode::fail;
    audio_service_fixture fixture{std::move(options)};
    REQUIRE(wait_until([&fixture] {
        return find_start_failure(fixture.service->progress(), fixture.graph.target).has_value();
    }));
    CHECK(fixture.service->progress().resident_slots == 0U);

    REQUIRE(fixture.synchronize_inventory(connection_inventory{}));
    REQUIRE(wait_until([&fixture] {
        const auto progress = fixture.service->progress();
        return progress.start_failure_count == 0U &&
               !fixture.service->progress(fixture.graph.target).has_value();
    }));
    CHECK(fixture.service->progress().resident_slots == 0U);
}

} // namespace
} // namespace inputnext::audio_integration_test_support
