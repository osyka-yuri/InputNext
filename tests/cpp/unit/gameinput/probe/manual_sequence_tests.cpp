#include "manual_sequence.hpp"
#include "values/required_value.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using inputnext::gameinput_detail::manual_sequence;
using inputnext::gameinput_detail::manual_step_confirmation;
using inputnext::gameinput_detail::manual_step_result;
using inputnext::test::required_value;

TEST_CASE("gameinput.probe.manual_sequence_requires_delivery_and_user_result",
    "[unit][gameinput][probe]") {
    manual_sequence sequence{3U};
    CHECK_FALSE(sequence.record_result({.result = manual_step_result::passed,
        .accepted_at = manual_sequence::clock::time_point{}}));
    CHECK(sequence.index() == 0U);

    const auto first_readiness = required_value(sequence.mark_ready());
    CHECK(sequence.mark_ready() == first_readiness);
    CHECK(sequence.awaiting_confirmation());
    CHECK(sequence.index() == 0U);
    REQUIRE(sequence.record_result({.result = manual_step_result::passed,
        .readiness_id = first_readiness,
        .accepted_at = manual_sequence::clock::now()}));
    CHECK(sequence.index() == 1U);
    CHECK_FALSE(sequence.complete());

    const auto second_readiness = required_value(sequence.mark_ready());
    CHECK(second_readiness != first_readiness);
    REQUIRE(sequence.record_result({.result = manual_step_result::failed,
        .readiness_id = second_readiness,
        .accepted_at = manual_sequence::clock::now()}));
    CHECK_FALSE(sequence.complete());
    CHECK(sequence.index() == 2U);

    const auto third_readiness = required_value(sequence.mark_ready());
    REQUIRE(sequence.record_result({.result = manual_step_result::passed,
        .readiness_id = third_readiness,
        .accepted_at = manual_sequence::clock::now()}));
    CHECK(sequence.complete());
    CHECK(sequence.passed() == 2U);
    CHECK(sequence.failed() == 1U);
    CHECK_FALSE(sequence.mark_ready().has_value());
    CHECK_FALSE(sequence.awaiting_confirmation());
    CHECK(sequence.index() == 3U);
}

TEST_CASE("gameinput.probe.manual_sequence_uses_acceptance_time_after_delayed_turn",
    "[unit][gameinput][probe]") {
    using clock = manual_sequence::clock;
    constexpr auto ready_at = clock::time_point{std::chrono::seconds{1}};
    constexpr auto deadline = clock::time_point{std::chrono::seconds{2}};
    constexpr auto resumed = deadline + std::chrono::seconds{5};
    manual_sequence sequence{1U};
    const auto readiness = required_value(sequence.mark_ready(deadline, ready_at));

    SECTION("a confirmation accepted before renewal survives delayed processing") {
        const manual_step_confirmation confirmation{.result = manual_step_result::passed,
            .readiness_id = readiness,
            .accepted_at = deadline - clock::duration{1}};
        CHECK_FALSE(sequence.awaiting_confirmation(resumed));
        CHECK(sequence.record_result(confirmation, resumed));
        CHECK(sequence.complete());
    }

    SECTION("a confirmation accepted at renewal is expired") {
        CHECK_FALSE(sequence.record_result({.result = manual_step_result::passed,
                                               .readiness_id = readiness,
                                               .accepted_at = deadline},
            resumed));
        CHECK(sequence.index() == 0U);
        CHECK_FALSE(sequence.awaiting_confirmation(resumed));
    }

    SECTION("a confirmation accepted after renewal is expired") {
        CHECK_FALSE(sequence.record_result({.result = manual_step_result::failed,
                                               .readiness_id = readiness,
                                               .accepted_at = resumed},
            resumed));
        CHECK(sequence.failed() == 0U);
        CHECK(sequence.index() == 0U);
    }
}

TEST_CASE("gameinput.probe.manual_sequence_rejects_wrong_stale_and_duplicate_confirmations",
    "[unit][gameinput][probe]") {
    using clock = manual_sequence::clock;
    constexpr auto first_ready = clock::time_point{std::chrono::seconds{1}};
    constexpr auto first_deadline = clock::time_point{std::chrono::seconds{2}};
    constexpr auto second_ready = clock::time_point{std::chrono::seconds{3}};
    constexpr auto second_deadline = clock::time_point{std::chrono::seconds{4}};
    manual_sequence sequence{2U};
    const auto first_id = required_value(sequence.mark_ready(first_deadline, first_ready));

    CHECK_FALSE(sequence.record_result({.result = manual_step_result::passed,
                                           .readiness_id = first_id + 1U,
                                           .accepted_at = first_ready},
        first_ready));
    CHECK(sequence.awaiting_confirmation(first_ready));

    const manual_step_confirmation first{.result = manual_step_result::passed,
        .readiness_id = first_id,
        .accepted_at = first_ready};
    REQUIRE(sequence.record_result(first, first_ready));
    CHECK_FALSE(sequence.record_result(first, first_ready));

    const auto second_id = required_value(sequence.mark_ready(second_deadline, second_ready));
    CHECK(second_id != first_id);
    CHECK_FALSE(sequence.record_result(first, second_ready));
    CHECK(sequence.awaiting_confirmation(second_ready));
    REQUIRE(sequence.record_result({.result = manual_step_result::passed,
                                       .readiness_id = second_id,
                                       .accepted_at = second_ready},
        second_ready));
    CHECK(sequence.complete());
}

TEST_CASE("gameinput.probe.manual_sequence_rejects_pending_and_future_confirmations",
    "[unit][gameinput][probe]") {
    using clock = manual_sequence::clock;
    constexpr auto ready_at = clock::time_point{std::chrono::seconds{1}};
    constexpr auto deadline = clock::time_point{std::chrono::seconds{2}};
    manual_sequence sequence{1U};
    const auto stale_id = required_value(sequence.mark_ready(deadline, ready_at));
    sequence.mark_pending();
    CHECK_FALSE(sequence.record_result(
        {.result = manual_step_result::passed, .readiness_id = stale_id, .accepted_at = ready_at},
        ready_at));

    const auto current_id = required_value(sequence.mark_ready(deadline, ready_at));
    CHECK(current_id != stale_id);
    CHECK_FALSE(sequence.record_result({.result = manual_step_result::passed,
                                           .readiness_id = current_id,
                                           .accepted_at = ready_at + clock::duration{1}},
        ready_at));
    CHECK(sequence.awaiting_confirmation(ready_at));
    REQUIRE(sequence.record_result(
        {.result = manual_step_result::passed, .readiness_id = current_id, .accepted_at = ready_at},
        ready_at));
    CHECK(sequence.complete());
}

TEST_CASE("gameinput.probe.manual_sequence_excludes_skipped_steps", "[unit][gameinput][probe]") {
    manual_sequence sequence{20U, 15U};
    CHECK(sequence.index() == 15U);
    CHECK(sequence.passed() == 0U);
    CHECK(sequence.failed() == 0U);
    for (unsigned int index = 0U; index < 5U; ++index) {
        const auto readiness = required_value(sequence.mark_ready());
        REQUIRE(sequence.record_result({.result = manual_step_result::passed,
            .readiness_id = readiness,
            .accepted_at = manual_sequence::clock::now()}));
    }
    CHECK(sequence.complete());
    CHECK(sequence.passed() == 5U);
    CHECK_THROWS_AS((manual_sequence{20U, 21U}), std::invalid_argument);
}
