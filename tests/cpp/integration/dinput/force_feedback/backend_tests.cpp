#include "dinput/force_feedback/parameters.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/composition/window_acquisition.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/release_blocked_write.hpp"
#include "support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <thread>

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

namespace {

using namespace inputnext;
using namespace inputnext::dinput::force_feedback;
using namespace std::chrono_literals;
using inputnext::dinput::feedback_test_support::constant;
using inputnext::dinput::feedback_test_support::feedback_fixture;

TEST_CASE("dinput.feedback.start_and_download_require_live_exclusive_authority",
    "[integration][dinput][force_feedback][authority]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    CHECK(fixture.feedback->download(id, {}) == DIERR_NOTEXCLUSIVEACQUIRED);
    CHECK(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, {}) ==
          DIERR_NOTEXCLUSIVEACQUIRED);
    CHECK(fixture.feedback->command(&fixture, DISFFC_STOPALL, {}) == DIERR_NOTEXCLUSIVEACQUIRED);
    DWORD state = 0x1234U;
    CHECK(fixture.feedback->state(&fixture, &state, {}) == DIERR_NOTEXCLUSIVEACQUIRED);
    CHECK(state == 0x1234U);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    acquired.token->revoke();
    CHECK(FAILED(
        fixture.feedback->start(id, {.iterations = 1U, .flags = DIES_NODOWNLOAD}, acquired)));
    const auto after_loss = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(after_loss.has_value());
    CHECK_FALSE(after_loss->rumble.has_value());
    const auto replacement = fixture.acquisition();
    CHECK(FAILED(
        fixture.feedback->start(id, {.iterations = 1U, .flags = DIES_NODOWNLOAD}, replacement)));
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, replacement) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
}

TEST_CASE("dinput.feedback.backend_changes_discard_playback_until_an_explicit_start",
    "[integration][dinput][force_feedback][backend]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = INFINITE}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    SECTION("availability loss") {
        fixture.available.store(false, std::memory_order_release);
        REQUIRE(fixture.wait_for_stopped(id));
        fixture.available.store(true, std::memory_order_release);
    }
    SECTION("replacement without an observed availability gap") {
        fixture.backend_id.store(2U, std::memory_order_release);
        REQUIRE(fixture.wait_for_stopped(id));
    }
    SECTION("motor mask changes on the same backend") {
        fixture.high.store(false, std::memory_order_release);
        REQUIRE(fixture.wait_for_stopped(id));
    }
    CHECK(acquired.token->live());
    std::this_thread::sleep_for(10ms);
    const auto stopped = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(stopped.has_value());
    CHECK_FALSE(stopped->rumble.has_value());
    CHECK(fixture.feedback->start(id, {.iterations = 1U, .flags = DIES_NODOWNLOAD}, acquired) ==
          DIERR_NOTDOWNLOADED);
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    const auto restarted = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(restarted.has_value());
    REQUIRE(restarted->rumble.has_value());
    CHECK(restarted->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .value.high_frequency ==
          Catch::Approx(fixture.high.load(std::memory_order_acquire) ? 0.8F : 0.0F));
}

TEST_CASE("dinput.feedback.a_stale_worker_query_cannot_revoke_a_new_start",
    "[integration][dinput][force_feedback][backend][concurrency]") {
    feedback_fixture fixture;
    const auto acquired = fixture.acquisition();
    const auto id = fixture.create(&fixture);
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(fixture.pause_next_query());
    fixture.backend_id.store(2U, std::memory_order_release);
    auto started = std::async(std::launch::async,
        [&] { return fixture.feedback->start(id, {.iterations = 1U}, acquired); });
    const bool ready = started.wait_for(2s) == std::future_status::ready;
    if (!ready) {
        fixture.release_query.store(true, std::memory_order_release);
    }
    CHECK(ready);
    REQUIRE(started.get() == DI_OK);
    fixture.release_query.store(true, std::memory_order_release);
    std::this_thread::sleep_for(10ms);
    DWORD state{};
    REQUIRE(fixture.feedback->status(id, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) != 0U);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
}

TEST_CASE("dinput.feedback.a_stale_control_query_cannot_resume_invalidated_playback",
    "[integration][dinput][force_feedback][backend][concurrency]") {
    feedback_fixture fixture;
    const auto acquired = fixture.acquisition();
    const auto first = fixture.create(&fixture);
    const auto second = fixture.create(&fixture, 2000);
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U}, acquired) == DI_OK);
    fixture.paused_query_thread.store(GetCurrentThreadId(), std::memory_order_release);
    fixture.pause_query.store(true, std::memory_order_release);
    auto started = std::async(std::launch::async, [&] {
        fixture.paused_query_thread.store(GetCurrentThreadId(), std::memory_order_release);
        return fixture.feedback->start(second, {.iterations = 1U}, acquired);
    });
    const bool paused = fixture.pause_next_query();
    fixture.available.store(false, std::memory_order_release);
    const bool invalidated = fixture.wait_for_stopped(first);
    fixture.available.store(true, std::memory_order_release);
    fixture.release_query.store(true, std::memory_order_release);
    CHECK(paused);
    CHECK(invalidated);
    CHECK(started.get() == DIERR_GENERIC);
    DWORD state{};
    REQUIRE(fixture.feedback->status(second, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) == 0U);
    const auto current = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    CHECK_FALSE(current->rumble.has_value());
}

TEST_CASE("dinput.feedback.a_replacement_backend_never_receives_the_old_effect",
    "[integration][dinput][force_feedback][backend][concurrency]") {
    feedback_fixture fixture;
    auto foreign = test::output::make_internal_producer(fixture.trusted,
        {.owner = output::internal_owner_key{80U},
            .priority = output::internal_priority::game_profile,
            .ttl = output::ttl_permission::finite_only,
            .foreground = output::foreground_policy::foreground_bound});
    const auto foreign_context = foreign.open(fixture.target);
    REQUIRE(foreign_context.has_value());
    REQUIRE(foreign
            .submit(*foreign_context,
                test::output::rumble_request({.channels = output::rumble_mask,
                    .sequence = 1U,
                    .ttl = 10s,
                    .low = 0.2F,
                    .high = 0.3F}))
            .has_value());
    const auto old_native = std::make_shared<test::output::controlled_native_state>();
    std::unique_ptr<output::native_sink> old_sink =
        std::make_unique<test::output::controlled_native_sink>(old_native);
    const auto old_backend =
        fixture.executor->register_backend(fixture.target, old_sink, output::rumble_mask);
    REQUIRE(old_backend.has_value());
    fixture.backend_id.store(old_backend->incarnation.id.value, std::memory_order_release);
    REQUIRE(old_native->wait_for_write_count(1U));
    const auto acquired = fixture.acquisition();
    const auto id = fixture.create(&fixture);
    REQUIRE(fixture.feedback->start(id, {.iterations = INFINITE}, acquired) == DI_OK);
    REQUIRE(old_native->wait_for_write_count(2U));
    const auto replacement_native = std::make_shared<test::output::controlled_native_state>();
    std::unique_ptr<output::native_sink> replacement_sink =
        std::make_unique<test::output::controlled_native_sink>(replacement_native);
    const auto replacement = fixture.executor->replace_backend(*old_backend,
        fixture.target,
        replacement_sink,
        output::rumble_mask);
    REQUIRE(replacement.has_value());
    // Deliberately keep the query port reporting the old handle until after native admission.
    REQUIRE(replacement_native->wait_for_write_count(1U));
    for (std::size_t index = 0U; index < replacement_native->call_count(); ++index) {
        CHECK(replacement_native->call(index).output.values.rumble.low_frequency ==
              Catch::Approx(0.2F));
    }
    fixture.backend_id.store(replacement->incarnation.id.value, std::memory_order_release);
    REQUIRE(fixture.wait_for_stopped(id));
    CHECK(acquired.token->live());
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(replacement_native->wait_for_write_count(2U));
    CHECK(replacement_native->call(replacement_native->call_count() - 1U)
              .output.values.rumble.low_frequency == Catch::Approx(0.8F));
}

TEST_CASE("dinput.feedback.state_excludes_downloads_from_an_old_acquisition_without_a_worker",
    "[integration][dinput][force_feedback][authority]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto old_acquisition = fixture.acquisition();
    REQUIRE(fixture.feedback->download(id, old_acquisition) == DI_OK);
    DWORD state{};
    REQUIRE(fixture.feedback->state(&fixture, &state, old_acquisition) == DI_OK);
    CHECK((state & DIGFFS_EMPTY) == 0U);
    old_acquisition.token->revoke();
    const auto replacement = fixture.acquisition();
    REQUIRE(fixture.feedback->state(&fixture, &state, replacement) == DI_OK);
    CHECK((state & DIGFFS_EMPTY) != 0U);
    REQUIRE(fixture.feedback->download(id, replacement) == DI_OK);
    REQUIRE(fixture.feedback->state(&fixture, &state, replacement) == DI_OK);
    CHECK((state & DIGFFS_EMPTY) == 0U);
}

TEST_CASE("dinput.feedback.window_revocation_cleans_its_context_during_a_blocked_native_write",
    "[integration][dinput][force_feedback][concurrency][authority]") {
    feedback_fixture fixture;
    auto foreign = test::output::make_internal_producer(fixture.trusted,
        {.owner = output::internal_owner_key{78U},
            .priority = output::internal_priority::game_profile,
            .ttl = output::ttl_permission::finite_only,
            .foreground = output::foreground_policy::foreground_bound});
    const auto foreign_context = foreign.open(fixture.target);
    REQUIRE(foreign_context.has_value());
    REQUIRE(foreign
            .submit(*foreign_context,
                test::output::rumble_request({.channels = output::rumble_mask,
                    .sequence = 1U,
                    .ttl = 10s,
                    .low = 0.2F,
                    .high = 0.3F}))
            .has_value());
    const auto native = std::make_shared<test::output::controlled_native_state>();
    const test::output::release_blocked_write release{native};
    std::unique_ptr<output::native_sink> sink =
        std::make_unique<test::output::controlled_native_sink>(native);
    const auto backend =
        fixture.executor->register_backend(fixture.target, sink, output::rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(native->wait_for_write_count(1U));
    const auto wait_for_settled = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto progress = fixture.executor->progress(*backend);
            if (progress.has_value() && !progress->write_in_flight &&
                progress->delivery == output::delivery_state::setter_accepted) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    };
    REQUIRE(wait_for_settled());
    native->block_next_write();
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(native->wait_for_blocked_write());
    const auto before = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(before.has_value());
    REQUIRE(before->rumble.has_value());
    const auto obsolete = before->rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    const auto blocked = native->call(native->call_count() - 1U);
    fixture.focus.set_window(202U, 3U);
    REQUIRE(fixture.focus.wait_for_window(202U, 3U));
    acquired.token->revoke();
    bool terminal_shutdown{};
    SECTION("ordinary stop") {
    }
    SECTION("terminal shutdown with a persistent close error") {
        output::detail::authority_test_access::fail_internal_cleanup(
            fixture.trusted.public_authority(),
            output::error::resources,
            std::nullopt);
        terminal_shutdown = true;
    }
    auto stopped = std::async(std::launch::async, [&] {
        if (terminal_shutdown) {
            fixture.feedback->shutdown();
            return DI_OK;
        }
        return fixture.feedback->stop(id);
    });
    const bool stopped_before_native_release = stopped.wait_for(2s) == std::future_status::ready;
    if (!stopped_before_native_release) {
        output::detail::authority_test_access::fail_internal_cleanup(
            fixture.trusted.public_authority(),
            std::nullopt,
            std::nullopt);
        native->release_write();
    }
    CHECK(stopped_before_native_release);
    REQUIRE(stopped.get() == DI_OK);
    CHECK_FALSE(
        output::detail::authority_test_access::observe_record(fixture.trusted.public_authority(),
            obsolete.provenance.identity)
            .has_value());
    const auto current = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    REQUIRE(current->rumble.has_value());
    CHECK(current->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .provenance.identity == foreign_context->identity);
    native->release_write();
    REQUIRE(native->wait_for_write_count(3U));
    REQUIRE(wait_for_settled());
    const auto observed = fixture.executor->observe(*backend);
    REQUIRE(observed.has_value());
    CHECK(observed->projection_current);
    CHECK(observed->progress.observed_publication != blocked.publication);
    const auto completion =
        observed->progress.last_completed_attempt.value_or(output::completed_attempt{});
    CHECK(completion.id != blocked.attempt);
    CHECK(completion.output.values.rumble.low_frequency == Catch::Approx(0.2F));
    output::detail::authority_test_access::fail_internal_cleanup(fixture.trusted.public_authority(),
        std::nullopt,
        std::nullopt);
}

TEST_CASE("dinput.feedback.a_small_sample_period_does_not_accelerate_the_shared_worker",
    "[integration][dinput][force_feedback][timing]") {
    feedback_fixture fixture;
    const auto native = std::make_shared<test::output::controlled_native_state>();
    std::unique_ptr<output::native_sink> sink =
        std::make_unique<test::output::controlled_native_sink>(native);
    REQUIRE(
        fixture.executor->register_backend(fixture.target, sink, output::rumble_mask).has_value());
    REQUIRE(native->wait_for_write_count(1U));
    auto parameters = constant();
    parameters.sample_period = 1U;
    parameters.force =
        DIPERIODIC{.dwMagnitude = 8000U, .lOffset = 0, .dwPhase = 9000U, .dwPeriod = 100000U};
    std::uint64_t id{};
    REQUIRE(fixture.feedback->create(&fixture, *find_effect(GUID_Sine), parameters, id) == DI_OK);
    const auto initial_writes = native->call_count();
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, fixture.acquisition()) == DI_OK);
    REQUIRE(native->wait_for_write_count(initial_writes + 2U));
    std::this_thread::sleep_for(30ms);
    const auto writes = native->call_count() - initial_writes;
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto scheduled_ticks = elapsed / 4ms;
    // Each tick can publish a sample and independently expire its previous native value.
    CHECK(writes <= (static_cast<std::size_t>(scheduled_ticks) + 3U) * 2U);
}

} // namespace
