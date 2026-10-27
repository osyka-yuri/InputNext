#include "effects/effects.hpp"
#include "effects/progress_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/output_execution.hpp>

namespace inputnext::effects_test {
namespace {

[[nodiscard]] output::backend_progress blocked_progress(const output::native_status result) {
    constexpr auto publication = output::publication_id{.value = 11U};
    constexpr auto attempt = output::attempt_id{.value = 7U};
    return output::backend_progress{.observed_publication = publication,
        .delivery = output::delivery_state::blocked,
        .last_attempt = attempt,
        .last_completed_attempt = output::completed_attempt{.id = attempt,
            .publication = publication,
            .result = output::native_result{.status = result}}};
}

} // namespace

TEST_CASE("effects.progress_failure_preserves_current_blocked_native_cause",
    "[unit][effects][progress]") {
    const auto unsupported = blocked_progress(output::native_status::unsupported);
    CHECK(effects::delivery_failure(unsupported) == effects::status::unsupported);

    const auto failed = blocked_progress(output::native_status::failed);
    CHECK(effects::delivery_failure(failed) == effects::status::io_failed);
}

TEST_CASE("effects.progress_failure_leaves_accepted_and_pending_delivery_unclassified",
    "[unit][effects][progress]") {
    auto accepted = blocked_progress(output::native_status::accepted);
    accepted.delivery = output::delivery_state::setter_accepted;
    CHECK(effects::delivery_failure(accepted) == effects::status::pending);

    const output::backend_progress pending{};
    CHECK(effects::delivery_failure(pending) == effects::status::pending);
}

TEST_CASE("effects.progress_failure_does_not_use_stale_or_in_flight_completion",
    "[unit][effects][progress]") {
    auto stale = blocked_progress(output::native_status::unsupported);
    stale.last_completed_attempt = output::completed_attempt{.id = output::attempt_id{.value = 7U},
        .publication = output::publication_id{.value = 10U},
        .result = output::native_result{.status = output::native_status::unsupported}};
    CHECK(effects::delivery_failure(stale) == effects::status::io_failed);

    auto previous_attempt = blocked_progress(output::native_status::unsupported);
    previous_attempt.last_attempt = output::attempt_id{.value = 8U};
    CHECK(effects::delivery_failure(previous_attempt) == effects::status::io_failed);

    auto in_flight = blocked_progress(output::native_status::unsupported);
    in_flight.write_in_flight = true;
    CHECK(effects::delivery_failure(in_flight) == effects::status::pending);
}

TEST_CASE("effects.progress_failure_reports_identity_exhaustion_as_capacity",
    "[unit][effects][progress]") {
    auto exhausted = blocked_progress(output::native_status::unsupported);
    exhausted.delivery = output::delivery_state::identity_exhausted;
    CHECK(effects::delivery_failure(exhausted) == effects::status::capacity_exceeded);
}

} // namespace inputnext::effects_test
