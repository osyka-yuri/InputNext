#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/service.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/internal_producer.hpp"
#include "support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>

namespace {

using namespace inputnext;
using namespace inputnext::dinput::force_feedback;
using namespace std::chrono_literals;
using inputnext::dinput::feedback_test_support::constant;
using inputnext::dinput::feedback_test_support::feedback_fixture;

TEST_CASE("dinput.feedback.stop_unload_and_mute_withdraw_before_returning",
    "[integration][dinput][force_feedback][cleanup]") {
    feedback_fixture fixture;
    auto foreign = test::output::make_internal_producer(fixture.trusted,
        {.owner = output::internal_owner_key{77U},
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
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    for (const auto command : std::array<DWORD, 2U>{DISFFC_STOPALL, DISFFC_SETACTUATORSOFF}) {
        REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
        REQUIRE(fixture.wait_for_amplitude(0.8F));
        REQUIRE(fixture.feedback->command(&fixture, command, acquired) == DI_OK);
        const auto stopped = fixture.trusted.public_authority().read_snapshot(fixture.target);
        REQUIRE(stopped.has_value());
        REQUIRE(stopped->rumble.has_value());
        const auto contribution =
            stopped->rumble.value_or(output::channel_snapshot<output::rumble_value>{});
        CHECK(contribution.provenance.identity == foreign_context->identity);
        CHECK(contribution.value.low_frequency == Catch::Approx(0.2F));
        REQUIRE(fixture.feedback->command(&fixture, DISFFC_SETACTUATORSON, acquired) == DI_OK);
    }
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->stop(id) == DI_OK);
    const auto stopped = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(stopped.has_value());
    REQUIRE(stopped->rumble.has_value());
    CHECK(stopped->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .provenance.identity == foreign_context->identity);
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->unload(id) == DI_OK);
    const auto unloaded = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(unloaded.has_value());
    REQUIRE(unloaded->rumble.has_value());
    CHECK(unloaded->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .provenance.identity == foreign_context->identity);
}

TEST_CASE("dinput.feedback.bounds_effect_storage_and_reclaims_released_slots",
    "[integration][dinput][force_feedback][capacity]") {
    feedback_fixture fixture;
    std::array<int, 5U> devices{};
    std::array<std::uint64_t, max_effects> ids{};
    for (std::size_t index = 0U; index < ids.size(); ++index) {
        ids[index] = fixture.create(&devices[index / max_effects_per_device]);
    }
    std::uint64_t rejected = 123U;
    CHECK(FAILED(fixture.feedback
            ->create(&devices[4U], *find_effect(GUID_ConstantForce), constant(), rejected)));
    CHECK(rejected == 0U);
    fixture.feedback->release(ids[0U]);
    std::uint64_t replacement{};
    CHECK(fixture.feedback->create(&devices[4U],
              *find_effect(GUID_ConstantForce),
              constant(),
              replacement) == DI_OK);
    CHECK(replacement != ids[0U]);
    fixture.feedback->shutdown();
    CHECK(FAILED(fixture.feedback->start(replacement,
        {.iterations = 1U, .flags = 0U},
        fixture.acquisition())));
    fixture.feedback->release(replacement);
}

TEST_CASE("dinput.feedback.failed_start_reclaims_new_contexts_despite_cleanup_failures",
    "[integration][dinput][force_feedback][cleanup][capacity]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    using access = output::detail::authority_test_access;
    auto& authority = fixture.trusted.public_authority();
    access::fail_internal_cleanup(authority, output::error::resources, output::error::resources);
    access::fail_internal_mutation(authority, output::error::resources, output::error::resources);
    for (std::size_t index = 0U; index < max_acquisitions * 2U; ++index) {
        CHECK(FAILED(fixture.feedback->start(id,
            {.iterations = 1U, .flags = DIES_SOLO},
            fixture.acquisition())));
    }
    access::fail_internal_cleanup(authority, std::nullopt, std::nullopt);
    access::fail_internal_mutation(authority, std::nullopt, std::nullopt);
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, fixture.acquisition()) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
}

TEST_CASE("dinput.feedback.reset_invalidates_existing_effects_and_mutes_actuators",
    "[integration][dinput][force_feedback][cleanup]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->command(&fixture, DISFFC_RESET, acquired) == DI_OK);
    CHECK(fixture.feedback->start(id, {.iterations = 1U, .flags = 0U}, acquired) ==
          DIERR_NOTINITIALIZED);
    acquired.actuators_on = false;
    const auto new_id = fixture.create(&fixture);
    REQUIRE(fixture.feedback->start(new_id, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    const auto muted = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(muted.has_value());
    CHECK_FALSE(muted->rumble.has_value());
    REQUIRE(fixture.feedback->command(&fixture, DISFFC_SETACTUATORSON, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
}

TEST_CASE("dinput.feedback.bounds_acquisitions_and_recovers_after_stop",
    "[integration][dinput][force_feedback][capacity]") {
    feedback_fixture fixture;
    std::array<int, max_acquisitions + 1U> devices{};
    std::array<std::uint64_t, max_acquisitions + 1U> effects{};
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        effects[index] = fixture.create(&devices[index]);
        const auto acquired = fixture.acquisition();
        const auto result = fixture.feedback->start(effects[index], {.iterations = 1U}, acquired);
        CHECK(result == (index < max_acquisitions ? DI_OK : DIERR_OUTOFMEMORY));
    }
    REQUIRE(fixture.feedback->stop(effects[0U]) == DI_OK);
    REQUIRE(fixture.feedback->start(effects.back(), {.iterations = 1U}, fixture.acquisition()) ==
            DI_OK);
}

TEST_CASE("dinput.feedback.concurrent_control_and_shutdown_leave_no_contribution",
    "[integration][dinput][force_feedback][concurrency][lifecycle]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    auto controls = std::async(std::launch::async, [&] {
        for (unsigned index = 0U; index < 32U; ++index) {
            static_cast<void>(fixture.feedback->stop(id));
            static_cast<void>(fixture.feedback->start(id, {.iterations = 1U}, acquired));
        }
    });
    auto other_shutdown = std::async(std::launch::async, [&] { fixture.feedback->shutdown(); });
    fixture.feedback->shutdown();
    controls.get();
    other_shutdown.get();
    const auto current = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    CHECK_FALSE(current->rumble.has_value());
    CHECK(FAILED(fixture.feedback->start(id, {.iterations = 1U}, acquired)));
    fixture.feedback->release(id);
}

TEST_CASE("dinput.feedback.shutdown_retires_contexts_despite_permanent_cleanup_errors",
    "[integration][dinput][force_feedback][cleanup]") {
    feedback_fixture fixture;
    auto foreign = test::output::make_internal_producer(fixture.trusted,
        {.owner = output::internal_owner_key{79U},
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
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    const auto before = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(before.has_value());
    REQUIRE(before->rumble.has_value());
    const auto identity = before->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
                              .provenance.identity;
    using access = output::detail::authority_test_access;
    auto& authority = fixture.trusted.public_authority();
    bool close_fails{};
    SECTION("close always fails") {
        access::fail_internal_cleanup(authority, output::error::resources, std::nullopt);
        close_fails = true;
    }
    SECTION("acknowledge always fails") {
        access::fail_internal_cleanup(authority, std::nullopt, output::error::resources);
    }
    REQUIRE(FAILED(fixture.feedback->stop(id)));
    const auto pending = access::observe_record(authority, identity);
    REQUIRE(pending.has_value());
    CHECK(pending.value_or(access::record_observation{}).open == close_fails);
    auto stopped = std::async(std::launch::async, [&] { fixture.feedback->shutdown(); });
    const bool completed = stopped.wait_for(2s) == std::future_status::ready;
    // Let an implementation that retries forever exit before reporting a failed assertion.
    if (!completed) {
        access::fail_internal_cleanup(authority, std::nullopt, std::nullopt);
    }
    CHECK(completed);
    stopped.get();
    CHECK_FALSE(access::observe_record(authority, identity).has_value());
    const auto current = authority.read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    REQUIRE(current->rumble.has_value());
    CHECK(current->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .provenance.identity == foreign_context->identity);
    fixture.feedback->shutdown();
    access::fail_internal_cleanup(authority, std::nullopt, std::nullopt);
}

} // namespace
