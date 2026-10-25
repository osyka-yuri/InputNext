#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/pcm_controlled_memory_port.hpp"
#include "output/pcm_test_access.hpp"
#include "support/xinput/controlled_event_source.hpp"
#include "support/xinput/test_ports.hpp"

#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <future>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::make_pcm_preparer;
using inputnext::test::output::pcm_controlled_memory_port;
using inputnext::test::output::pcm_release_guard;
namespace xinput_support = inputnext::xinput_test_support;

[[nodiscard]] bool input_and_control_progress(authority_fixture& fixture) {
    xinput_support::metadata_source metadata;
    xinput_support::controlled_event_source events;
    xinput::service service{fixture.focus_observer, metadata, events, 1000U};
    const auto assigned = service.assign(fixture.connection.target, fixture.connection.reader);
    if (!assigned.has_value()) {
        return false;
    }
    const input_update update{
        .state = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 123U,
        .observation_qpc = 456U};
    const auto published = fixture.connection.producer.publish(update);
    const auto latest = fixture.connection.reader.read();
    XINPUT_STATE state{};
    const auto status = service.get_state(xinput::flavor::v1_4, assigned->user_index, &state);
    const auto opened = fixture.owner.open(fixture.connection.target);
    return published.has_value() && latest.has_value() && latest->sample.state == update.state &&
           status == ERROR_SUCCESS && state.Gamepad.wButtons == XINPUT_GAMEPAD_B &&
           opened.has_value();
}

TEST_CASE("output.pcm_preparation_does_not_block_ready_input_or_authority",
    "[integration][output][body-pcm][concurrency]") {
    authority_fixture fixture;
    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    inputnext::output::detail::authority_test_access::use_pcm_preparer(fixture.owner,
        make_pcm_preparer(memory));
    const auto preparer = fixture.owner.pcm_preparer();
    constexpr std::array samples{0.25F, -0.25F};
    memory->block_next_allocation();
    auto preparing = std::async(std::launch::async, [&] {
        return preparer.prepare(body_pcm_input{.samples = samples,
            .frame_count = samples.size(),
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
    });
    std::future<bool> progress;
    const pcm_release_guard release{memory};
    REQUIRE(memory->wait_for_allocation());
    progress = std::async(std::launch::async, [&] { return input_and_control_progress(fixture); });
    const bool completed_while_blocked = progress.wait_for(500ms) == std::future_status::ready;
    memory->release_all();
    CHECK(completed_while_blocked);
    CHECK(progress.get());
    REQUIRE(preparing.wait_for(2s) == std::future_status::ready);
    CHECK(preparing.get().has_value());
}

TEST_CASE("output.pcm_reclamation_does_not_hold_input_or_authority_gates",
    "[integration][output][body-pcm][concurrency]") {
    authority_fixture fixture;
    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    inputnext::output::detail::authority_test_access::use_pcm_preparer(fixture.owner,
        make_pcm_preparer(memory));
    const auto preparer = fixture.owner.pcm_preparer();
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    constexpr std::array samples{0.5F};
    auto prepared = preparer.prepare(body_pcm_input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    REQUIRE(prepared.has_value());
    REQUIRE(fixture.owner
            .submit_body(*context,
                body_request{.sequence = 1U, .ttl = 5s},
                body_playback_update{.command = body_pcm_command::replace,
                    .replacement = *prepared})
            .has_value());
    prepared = std::unexpected{inputnext::output::error::resources};
    memory->block_next_deallocation();
    auto closing = std::async(std::launch::async,
        [&] { return fixture.owner.close(*context, context->identity.id.value); });
    std::future<bool> progress;
    const pcm_release_guard release{memory};
    REQUIRE(memory->wait_for_deallocation());
    progress = std::async(std::launch::async, [&] { return input_and_control_progress(fixture); });
    const bool completed_while_blocked = progress.wait_for(500ms) == std::future_status::ready;
    memory->release_all();
    CHECK(completed_while_blocked);
    CHECK(progress.get());
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    CHECK(closing.get().has_value());
    CHECK(preparer.usage() == body_pcm_usage{});
}

} // namespace
