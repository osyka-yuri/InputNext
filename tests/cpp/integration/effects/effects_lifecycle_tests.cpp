#include "effects_session_fixture.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_rumble_test_support.hpp"
#include "effects/effects.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include "values/required_value.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>

namespace inputnext::effects_test {
namespace {

using namespace std::chrono_literals;
using effects_integration_test_support::session_fixture;

[[nodiscard]] output::submit_request rumble_request(const std::uint64_t sequence,
    const float low,
    const float high,
    const std::chrono::milliseconds ttl = 30s) noexcept {
    return output::submit_request{.selected_channels = output::rumble_mask,
        .sequence = sequence,
        .ttl = ttl,
        .values = {.rumble = {.low_frequency = low, .high_frequency = high}}};
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
    const std::chrono::steady_clock::duration timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

[[nodiscard]] std::optional<effects::progress> query_progress(session_fixture& fixture,
    const effects::publication_revision publication) {
    const auto result = fixture.service().query_progress(fixture.target, publication);
    if (!result.has_value()) {
        return std::nullopt;
    }
    return *result;
}

[[nodiscard]] bool wait_for_stage(session_fixture& fixture,
    const effects::publication_revision publication,
    const effects::progress_stage stage) {
    return wait_until([&fixture, publication, stage] {
        const auto current = query_progress(fixture, publication);
        return current.has_value() && inputnext::test::required_value(current).stage == stage;
    });
}

[[nodiscard]] std::optional<connection_target> wait_for_replacement_target(session_fixture& fixture,
    const connection_target retired) {
    std::optional<connection_target> replacement;
    const bool found = wait_until([&fixture, retired, &replacement] {
        const auto catalogue = fixture.runtime.session->core().enumerate();
        if (!catalogue.has_value()) {
            return false;
        }
        for (const auto& entry : std::span{catalogue->entries}.first(catalogue->count)) {
            if (entry.target != retired) {
                replacement = entry.target;
                return true;
            }
        }
        return false;
    });
    return found ? replacement : std::nullopt;
}

TEST_CASE("effects.concurrent_submit_close_ack_preserves_linearized_context_ownership",
    "[integration][effects][lifecycle][concurrency]") {
    session_fixture fixture;
    fixture.start();
    const auto context = fixture.open();
    std::barrier start{3};
    std::optional<effects::result<effects::discrete_receipt>> submitted;
    std::optional<effects::result<effects::close_receipt>> closed;
    std::optional<effects::status> acknowledged;

    std::jthread submitter{[&] {
        start.arrive_and_wait();
        const auto request = rumble_request(1U, 0.35F, 0.6F);
        submitted.emplace(
            fixture.service().submit(context, request, session_fixture::window(request.ttl)));
    }};
    std::jthread closer{[&] {
        start.arrive_and_wait();
        closed.emplace(fixture.service().close(context, context.id));
        if (closed.has_value()) {
            const auto close_result = inputnext::test::required_value(closed);
            if (close_result.has_value()) {
                acknowledged.emplace(fixture.service().acknowledge(close_result.value()));
            }
        }
    }};
    start.arrive_and_wait();
    submitter.join();
    closer.join();

    const auto submit_result = inputnext::test::required_value(submitted);
    const auto close_result = inputnext::test::required_value(closed);
    const auto acknowledge_result = inputnext::test::required_value(acknowledged);
    REQUIRE(close_result.has_value());
    CHECK(acknowledge_result == effects::status::ok);
    if (submit_result.has_value()) {
        CHECK(submit_result.value().context == context);
    } else {
        CHECK((submit_result.error() == effects::status::revoked ||
               submit_result.error() == effects::status::stale_context));
    }

    const auto replacement = fixture.service().open(fixture.target);
    REQUIRE(replacement.has_value());
    CHECK(*replacement != context);
    const auto stale_request = rumble_request(1U, 0.9F, 0.1F);
    const auto stale_submit = fixture.service().submit(context,
        stale_request,
        session_fixture::window(stale_request.ttl));
    REQUIRE(!stale_submit.has_value());
    CHECK(stale_submit.error() == effects::status::stale_context);

    const auto current_request = rumble_request(1U, 0.2F, 0.4F);
    const auto current_submit = fixture.service().submit(*replacement,
        current_request,
        session_fixture::window(current_request.ttl));
    REQUIRE(current_submit.has_value());
    CHECK(current_submit->context == *replacement);
}

TEST_CASE("effects.retired_target_keeps_external_close_capacity_until_acknowledged",
    "[integration][effects][lifecycle][retirement]") {
    session_fixture fixture;
    fixture.start();
    std::array<effects::context_handle, effects::context_capacity> contexts{};
    for (auto& context : contexts) {
        const auto opened = fixture.service().open(fixture.target);
        INFO("Open status: " << static_cast<unsigned int>(
                 opened.has_value() ? effects::status::ok : opened.error()));
        REQUIRE(opened.has_value());
        context = *opened;
    }
    const auto retired_target = fixture.target;

    fixture.runtime.native->fake->emit(fixture.runtime.device,
        gameinput_detail::native_device_event::disconnected);
    fixture.runtime.native->fake->emit(fixture.runtime.device,
        gameinput_detail::native_device_event::connected);
    const auto replacement_target = wait_for_replacement_target(fixture, retired_target);
    const auto new_target = inputnext::test::required_value(replacement_target);
    REQUIRE(new_target != retired_target);
    fixture.target = new_target;

    const core::selector selection{.kind = core::selector_kind::exact_target,
        .target = fixture.target};
    REQUIRE(wait_until([&fixture, &selection] {
        const auto resolved = fixture.service().resolve(selection);
        return resolved.has_value() && resolved->target == fixture.target;
    }));

    const auto maintenance = fixture.service().maintain();
    CHECK(maintenance.occupied_contexts == effects::context_capacity);
    const auto retired_close = fixture.service().close(contexts.front(), contexts.front().id);
    REQUIRE(retired_close.has_value());
    CHECK(retired_close->target == retired_target);
    CHECK(fixture.service().close(contexts.front(), contexts.front().id) == retired_close);
    const auto retired_progress =
        fixture.service().query_progress(retired_target, retired_close->publication);
    REQUIRE(retired_progress.has_value());
    CHECK(retired_progress->stage == effects::progress_stage::connection_retired);

    const auto full = fixture.service().open(fixture.target);
    REQUIRE(!full.has_value());
    CHECK(full.error() == effects::status::capacity_exceeded);
    const auto hidden_on_replacement =
        fixture.service().query_progress(fixture.target, retired_close->publication);
    REQUIRE(hidden_on_replacement.has_value());
    CHECK(hidden_on_replacement->stage == effects::progress_stage::unknown);

    CHECK(fixture.service().acknowledge(*retired_close) == effects::status::ok);
    const auto replacement_context = fixture.service().open(fixture.target);
    REQUIRE(replacement_context.has_value());
    CHECK(*replacement_context != contexts.front());
    const auto old_handle_request = rumble_request(1U, 0.8F, 0.2F);
    const auto old_handle_submit = fixture.service().submit(contexts.front(),
        old_handle_request,
        session_fixture::window(old_handle_request.ttl));
    REQUIRE(!old_handle_submit.has_value());
    CHECK(old_handle_submit.error() == effects::status::stale_context);
}

TEST_CASE("effects.blocked_setter_cannot_complete_a_newer_mutation_or_close",
    "[integration][effects][lifecycle][progress][concurrency]") {
    session_fixture fixture;
    fixture.start();
    const auto blocked_context = fixture.open();
    const auto newer_context = fixture.open();

    const auto baseline_request = rumble_request(1U, 0.2F, 0.3F);
    const auto baseline = fixture.service().submit(blocked_context,
        baseline_request,
        session_fixture::window(baseline_request.ttl));
    REQUIRE(baseline.has_value());
    REQUIRE(
        wait_for_stage(fixture, baseline->publication, effects::progress_stage::current_completed));

    const gameinput_test_support::scoped_rumble_release release{*fixture.runtime.rumble_controls};
    fixture.runtime.rumble_controls->block_next_write();
    const auto blocked_request = rumble_request(2U, 0.9F, 0.1F);
    const auto blocked = fixture.service().submit(blocked_context,
        blocked_request,
        session_fixture::window(blocked_request.ttl));
    REQUIRE(blocked.has_value());
    REQUIRE(fixture.runtime.rumble_controls->wait_for_write());

    const auto newer_request = rumble_request(1U, 0.6F, 0.4F);
    const auto newer = fixture.service().submit(newer_context,
        newer_request,
        session_fixture::window(newer_request.ttl));
    REQUIRE(newer.has_value());
    const auto superseded =
        inputnext::test::required_value(query_progress(fixture, blocked->publication));
    CHECK(superseded.stage == effects::progress_stage::superseded);
    const auto newer_while_blocked =
        inputnext::test::required_value(query_progress(fixture, newer->publication));
    CHECK(newer_while_blocked.stage != effects::progress_stage::current_completed);

    const auto closed = fixture.service().close(blocked_context, blocked_context.id);
    REQUIRE(closed.has_value());
    CHECK(closed->disposition == effects::close_disposition::normal);
    const auto close_while_blocked =
        inputnext::test::required_value(query_progress(fixture, closed->publication));
    CHECK(close_while_blocked.stage != effects::progress_stage::current_completed);

    fixture.runtime.rumble_controls->release_write();
    REQUIRE(
        wait_for_stage(fixture, newer->publication, effects::progress_stage::current_completed));
    REQUIRE(
        wait_for_stage(fixture, closed->publication, effects::progress_stage::current_completed));
    const auto blocked_after_release =
        inputnext::test::required_value(query_progress(fixture, blocked->publication));
    CHECK(blocked_after_release.stage == effects::progress_stage::superseded);
    CHECK(fixture.service().acknowledge(*closed) == effects::status::ok);
}

} // namespace
} // namespace inputnext::effects_test
