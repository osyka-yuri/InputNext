#include "audio/audio_service.hpp"
#include "audio/controlled_audio_worker.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "audio_service_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

#include <inputnext/device_metadata.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::audio_detail;
using namespace inputnext::audio_integration_test_support;
using namespace inputnext::output;

[[nodiscard]] body_playback_receipt submit_clip(audio_service_fixture& fixture,
    const output::audio_instance audio,
    const std::span<const float> samples) {
    const auto context = required_value(fixture.authority().open(fixture.graph.target),
        "audio qualification fixture could not open authority context");
    auto prepared = fixture.authority().pcm_preparer().prepare(body_pcm_input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    if (!prepared.has_value()) {
        throw std::runtime_error{"audio qualification fixture could not prepare PCM"};
    }
    auto submitted = fixture.authority().submit_body(context,
        body_request{.audio = audio,
            .sequence = 1U,
            .ttl = 5s,
            .preference = body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.25F, .high_frequency = 0.5F}},
        body_playback_update{.command = body_pcm_command::replace,
            .replacement = std::move(*prepared)});
    if (!submitted.has_value()) {
        throw std::runtime_error{"audio qualification fixture could not submit PCM"};
    }
    return *submitted;
}

template <typename Predicate>
[[nodiscard]] bool wait_for_target_progress(const audio_service_fixture& fixture,
    Predicate predicate,
    const std::chrono::milliseconds timeout = 3s) {
    return wait_until(
        [&fixture, &predicate] {
            const auto current = fixture.service->progress(fixture.graph.target);
            return current.has_value() && predicate(*current);
        },
        timeout);
}

[[nodiscard]] bool wait_for_closed_audio(const audio_service_fixture& fixture,
    const output::audio_instance audio,
    const std::chrono::milliseconds timeout = 320ms) {
    return wait_for_target_progress(
        fixture,
        [audio](const audio_target_progress& current) {
            return !current.ready && current.audio.has_value() && *current.audio == audio;
        },
        timeout);
}

[[nodiscard]] bool wait_for_logical_fallback(const audio_service_fixture& fixture,
    const output::audio_instance audio,
    const std::chrono::milliseconds timeout = 2s) {
    return wait_until(
        [&fixture, audio] {
            const auto current = fixture.authority().read_snapshot(fixture.graph.target);
            return current.has_value() && current->body.has_value() &&
                   current->body->intent.identity.audio == audio &&
                   current->body->intent.route == body_actuator_route::classic &&
                   current->body->intent.fallback != body_fallback_reason::none;
        },
        timeout);
}

[[nodiscard]] bool wait_for_retained_owner(const audio_service_fixture& fixture,
    const std::shared_ptr<test::audio::controlled_audio_worker_state>& workers,
    const std::size_t index) {
    return wait_until(
        [&fixture, &workers, index] {
            const auto progress = fixture.service->progress();
            return workers->join_attempts(index) != 0U && progress.resident_slots == 1U &&
                   progress.retained_slots == 1U;
        },
        3s);
}

[[nodiscard]] bool wait_for_replacement_audio(const audio_service_fixture& fixture,
    const output::audio_instance old_audio,
    output::audio_instance& replacement) {
    return wait_until(
        [&fixture, old_audio, &replacement] {
            const auto row = fixture.service->progress(fixture.graph.target);
            if (!row.has_value() || !row->ready || !row->audio.has_value() ||
                *row->audio == old_audio) {
                return false;
            }
            replacement = *row->audio;
            return true;
        },
        4s);
}

[[nodiscard]] bool verify_fresh_audio_has_no_replay(audio_service_fixture& fixture,
    const output::audio_instance expected_audio,
    const std::shared_ptr<test::audio::controlled_native_port_state>& native_state) {
    native_state->signal_buffer_event();
    if (!wait_for_target_progress(fixture, [](const audio_target_progress& current) {
            return current.accepted_silent_buffers > 1U;
        })) {
        return false;
    }
    const auto current = fixture.service->progress(fixture.graph.target);
    return current.has_value() && current->audio == expected_audio &&
           current->admitted_blocks == 0U && current->source_frames_consumed == 0U &&
           current->accepted_pcm_buffers == 0U;
}

class scoped_buffer_event_pump final {
public:
    explicit scoped_buffer_event_pump(
        std::shared_ptr<test::audio::controlled_native_port_state> state)
        : state_(std::move(state)),
          worker_([this] { run(); }) {}

    ~scoped_buffer_event_pump() noexcept {
        stopping_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    scoped_buffer_event_pump(const scoped_buffer_event_pump&) = delete;
    scoped_buffer_event_pump& operator=(const scoped_buffer_event_pump&) = delete;
    scoped_buffer_event_pump(scoped_buffer_event_pump&&) = delete;
    scoped_buffer_event_pump& operator=(scoped_buffer_event_pump&&) = delete;

private:
    void run() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(20ms);
            if (!stopping_.load(std::memory_order_acquire)) {
                state_->signal_buffer_event();
            }
        }
    }

    std::shared_ptr<test::audio::controlled_native_port_state> state_;
    std::atomic<bool> stopping_;
    std::thread worker_;
};

[[nodiscard]] bool wait_for_expiry_close(const audio_service_fixture& fixture,
    const output::audio_instance audio,
    const std::chrono::steady_clock::time_point began) {
    constexpr auto evidence_window_with_control_turn = 320ms;
    const auto deadline = began + evidence_window_with_control_turn;
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return wait_for_closed_audio(fixture, audio, std::max(std::chrono::milliseconds{1}, remaining));
}

void verify_transient_metadata_retry(const metadata_error failure) {
    audio_service_fixture fixture{};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }

    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
    const auto old_audio = fixture.audio_instance();
    const auto pending = submit_clip(fixture, old_audio, samples);
    REQUIRE(fixture.wait_for_hd_ack(old_audio));
    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    const auto worker_thread = state->worker_thread_id();
    REQUIRE(worker_thread != std::thread::id{});
    const auto revalidations_before = state->count_calls(native_operation::get_endpoint_state);

    const auto recovery_window_started = std::chrono::steady_clock::now();
    fixture.graph.backing.metadata->fail_queries_on(worker_thread, failure, 1U);
    REQUIRE(wait_until(
        [&fixture] {
            return fixture.graph.backing.metadata->observed_thread_failure_count() == 1U;
        },
        160ms));
    const auto after_transient = required_value(fixture.service->progress(fixture.graph.target),
        "audio progress disappeared after transient metadata contention");
    CHECK(after_transient.ready);
    CHECK(after_transient.audio == old_audio);

    REQUIRE(wait_until(
        [&state, revalidations_before] {
            return state->count_calls(native_operation::get_endpoint_state) > revalidations_before;
        },
        160ms));
    CHECK(std::chrono::steady_clock::now() - recovery_window_started < 240ms);
    const auto recovered = required_value(fixture.service->progress(fixture.graph.target),
        "audio progress disappeared after metadata retry recovered");
    CHECK(recovered.ready);
    CHECK(recovered.audio == old_audio);

    state->signal_buffer_event();
    REQUIRE(wait_for_target_progress(fixture,
        [](const audio_target_progress& current) { return current.accepted_pcm_buffers == 1U; }));
    const auto played = required_value(fixture.service->progress(fixture.graph.target),
        "audio progress disappeared after pending clip submission");
    CHECK(played.audio == old_audio);
    CHECK(played.last_admitted_playback == pending.playback);
    CHECK(played.source_frames_consumed == samples.size());
}

void verify_blocked_native_expiry(const native_operation blocked_operation) {
    const auto workers = std::make_shared<test::audio::controlled_audio_worker_state>();
    workers->block_join_for(0U);
    audio_service_fixture fixture{false, std::nullopt, false, false, workers};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }

    const auto old_audio = fixture.audio_instance();
    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
    static_cast<void>(submit_clip(fixture, old_audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(old_audio));
    const auto native_state = fixture.native_states.front();
    REQUIRE(native_state != nullptr);
    const auto fresh_state = fixture.configure_native_port(1U);
    const auto expiry_began = std::chrono::steady_clock::now();
    native_state->block_on(blocked_operation);
    release_controlled_native_block release_block{native_state};
    native_state->signal_buffer_event();
    REQUIRE(native_state->wait_until_blocked(blocked_operation));

    if (blocked_operation == native_operation::release_buffer) {
        REQUIRE(wait_for_target_progress(fixture,
            [frame_count = samples.size()](const audio_target_progress& current) {
                return current.admitted_blocks == 1U &&
                       current.source_frames_consumed == frame_count;
            }));
    }

    REQUIRE(wait_for_expiry_close(fixture, old_audio, expiry_began));
    REQUIRE(wait_for_logical_fallback(fixture, old_audio));
    const auto closed = required_value(fixture.service->progress(fixture.graph.target),
        "closed audio owner progress disappeared before native return");
    CHECK(!closed.ready);
    CHECK(closed.audio == old_audio);
    CHECK(fixture.service->progress().resident_slots == 1U);
    CHECK(native_state->wait_until_blocked(blocked_operation, 1ms));
    if (blocked_operation == native_operation::release_buffer) {
        fixture.authority().collect_body_pcm();
        CHECK(fixture.authority().pcm_preparer().usage().buffers == 1U);
        CHECK(closed.admitted_blocks == 1U);
        CHECK(closed.source_frames_consumed == samples.size());
        CHECK(closed.accepted_pcm_buffers == 0U);
    }

    release_block.release();
    REQUIRE(wait_until([&native_state] {
        return native_state->count_calls(native_operation::finish_worker_sta) == 1U;
    }));
    REQUIRE(wait_for_retained_owner(fixture, workers, 0U));
    const auto retained = required_value(fixture.service->progress(fixture.graph.target),
        "old audio progress disappeared before its worker joined");
    CHECK(retained.audio == old_audio);
    CHECK(!retained.ready);
    CHECK(fixture.service->progress().retained_slots == 1U);

    workers->allow_join_for(0U);
    output::audio_instance new_audio{};
    REQUIRE(wait_for_replacement_audio(fixture, old_audio, new_audio));
    CHECK(new_audio != old_audio);
    CHECK(verify_fresh_audio_has_no_replay(fixture, new_audio, fresh_state));
}

} // namespace

TEST_CASE("audio.qualification.retries_busy_metadata_before_expiry_without_replacing_clip",
    "[integration][audio]") {
    verify_transient_metadata_retry(metadata_error::busy);
}

TEST_CASE("audio.qualification.retries_resource_contention_before_expiry_without_replacing_clip",
    "[integration][audio]") {
    verify_transient_metadata_retry(metadata_error::resource_exhausted);
}

TEST_CASE("audio.qualification.persistent_busy_closes_by_existing_evidence_expiry",
    "[integration][audio]") {
    const auto workers = std::make_shared<test::audio::controlled_audio_worker_state>();
    workers->block_join_for(0U);
    audio_service_fixture fixture{false, std::nullopt, false, false, workers};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }

    const auto old_audio = fixture.audio_instance();
    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    const auto worker_thread = state->worker_thread_id();
    REQUIRE(worker_thread != std::thread::id{});
    const auto expiry_began = std::chrono::steady_clock::now();
    fixture.graph.backing.metadata->fail_queries_on(worker_thread, metadata_error::busy, 64U);
    const scoped_buffer_event_pump pump{state};
    const bool observed_busy_failure = wait_until(
        [&fixture] {
            return fixture.graph.backing.metadata->observed_thread_failure_count() >= 1U;
        },
        160ms);
    if (!observed_busy_failure) {
        const auto busy_failure_count =
            fixture.graph.backing.metadata->observed_thread_failure_count();
        const auto busy_failure_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - expiry_began)
                                              .count();
        const auto busy_failure_progress = fixture.readiness_diagnostic();
        CAPTURE(busy_failure_count, busy_failure_wait_ms, busy_failure_progress);
        FAIL("worker did not observe a busy metadata response before the deadline");
    }
    const auto after_busy_failure = required_value(fixture.service->progress(fixture.graph.target),
        "audio progress disappeared after busy metadata contention");
    if (!after_busy_failure.ready || after_busy_failure.audio != old_audio) {
        const auto busy_failure_count =
            fixture.graph.backing.metadata->observed_thread_failure_count();
        const auto busy_failure_progress = fixture.readiness_diagnostic();
        CAPTURE(busy_failure_count, busy_failure_progress);
        FAIL("existing audio did not remain ready after the busy metadata response");
    }
    const bool closed_by_expiry = wait_for_expiry_close(fixture, old_audio, expiry_began);
    if (!closed_by_expiry) {
        const auto failures_at_expiry =
            fixture.graph.backing.metadata->observed_thread_failure_count();
        const auto expiry_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - expiry_began)
                                        .count();
        const auto expiry_progress = fixture.readiness_diagnostic();
        const auto finish_worker_calls = state->count_calls(native_operation::finish_worker_sta);
        const auto worker_join_attempts = workers->join_attempts(0U);
        CAPTURE(failures_at_expiry,
            expiry_wait_ms,
            expiry_progress,
            finish_worker_calls,
            worker_join_attempts);
        FAIL("audio did not close after existing identity evidence expired");
    }
    const bool finish_worker_sta_called = wait_until(
        [&state] { return state->count_calls(native_operation::finish_worker_sta) == 1U; });
    if (!finish_worker_sta_called) {
        const auto finish_calls_after_wait =
            state->count_calls(native_operation::finish_worker_sta);
        const auto join_attempts_after_wait = workers->join_attempts(0U);
        CAPTURE(finish_calls_after_wait, join_attempts_after_wait);
        FAIL("finish_worker_sta was not called after expiry closure");
    }
    fixture.graph.backing.metadata->clear_thread_query_failures();
    const bool owner_retained = wait_for_retained_owner(fixture, workers, 0U);
    if (!owner_retained) {
        const auto retirement_progress = fixture.service->progress();
        const auto retirement_join_attempts = workers->join_attempts(0U);
        CAPTURE(retirement_progress.resident_slots,
            retirement_progress.retained_slots,
            retirement_join_attempts);
        FAIL("expired audio owner did not remain retained while its worker was blocked from "
             "joining");
    }

    const auto retained = required_value(fixture.service->progress(fixture.graph.target),
        "expired audio owner progress disappeared before its worker joined");
    CHECK(retained.audio == old_audio);
    CHECK(!retained.ready);
    CHECK(retained.last_metadata_error == metadata_error::busy);
    CHECK(fixture.service->progress().retained_slots == 1U);
}

TEST_CASE("audio.qualification.stale_or_conflicting_refresh_evidence_closes_old_incarnation",
    "[integration][audio]") {
    SECTION("stale metadata result") {
        const auto workers = std::make_shared<test::audio::controlled_audio_worker_state>();
        workers->block_join_for(0U);
        audio_service_fixture fixture{false, std::nullopt, false, false, workers};
        REQUIRE(fixture.wait_for_live());
        const auto old_audio = fixture.audio_instance();
        constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
        static_cast<void>(submit_clip(fixture, old_audio, samples));
        REQUIRE(fixture.wait_for_hd_ack(old_audio));
        const auto state = fixture.native_states.front();
        REQUIRE(state != nullptr);
        const auto worker_thread = state->worker_thread_id();
        REQUIRE(worker_thread != std::thread::id{});
        fixture.graph.backing.metadata->fail_queries_on(worker_thread, metadata_error::stale, 1U);
        REQUIRE(wait_until(
            [&fixture] {
                return fixture.graph.backing.metadata->observed_thread_failure_count() == 1U;
            },
            160ms));
        REQUIRE(wait_for_closed_audio(fixture, old_audio));
        REQUIRE(wait_for_logical_fallback(fixture, old_audio));
        REQUIRE(wait_until(
            [&state] { return state->count_calls(native_operation::finish_worker_sta) == 1U; }));
        REQUIRE(wait_for_retained_owner(fixture, workers, 0U));
        const auto closed = required_value(fixture.service->progress(fixture.graph.target),
            "stale audio incarnation progress disappeared before its worker joined");
        CHECK(closed.audio == old_audio);
        CHECK(!closed.ready);
        CHECK(closed.last_metadata_error == metadata_error::stale);
        CHECK(fixture.service->progress().retained_slots == 1U);
    }

    SECTION("native endpoint identity conflict") {
        const auto workers = std::make_shared<test::audio::controlled_audio_worker_state>();
        workers->block_join_for(0U);
        audio_service_fixture fixture{false, std::nullopt, false, false, workers};
        REQUIRE(fixture.wait_for_live());
        const auto old_audio = fixture.audio_instance();
        constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
        static_cast<void>(submit_clip(fixture, old_audio, samples));
        REQUIRE(fixture.wait_for_hd_ack(old_audio));
        const auto state = fixture.native_states.front();
        REQUIRE(state != nullptr);
        {
            std::scoped_lock lock{state->mutex};
            state->endpoint.container_id.bytes[1U] = std::byte{0x55};
        }
        REQUIRE(wait_until(
            [&state] { return state->count_calls(native_operation::get_endpoint_state) > 1U; },
            160ms));
        REQUIRE(wait_for_closed_audio(fixture, old_audio));
        REQUIRE(wait_for_logical_fallback(fixture, old_audio));
        REQUIRE(wait_until(
            [&state] { return state->count_calls(native_operation::finish_worker_sta) == 1U; }));
        REQUIRE(wait_for_retained_owner(fixture, workers, 0U));
        const auto closed = required_value(fixture.service->progress(fixture.graph.target),
            "conflicted audio incarnation progress disappeared before its worker joined");
        CHECK(closed.audio == old_audio);
        CHECK(!closed.ready);
        CHECK(fixture.service->progress().retained_slots == 1U);
    }
}

TEST_CASE("audio.qualification.native_refresh_result_arriving_after_expiry_cannot_revive_audio",
    "[integration][audio]") {
    verify_blocked_native_expiry(native_operation::get_endpoint_state);
}

TEST_CASE("audio.qualification.get_buffer_remains_owned_until_expired_audio_is_released",
    "[integration][audio]") {
    verify_blocked_native_expiry(native_operation::get_buffer);
}

TEST_CASE("audio.qualification.release_buffer_keeps_submission_fence_until_native_return",
    "[integration][audio]") {
    verify_blocked_native_expiry(native_operation::release_buffer);
}
