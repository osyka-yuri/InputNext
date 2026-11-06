#include "audio/audio_service.hpp"
#include "audio/controlled_audio_worker.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "audio_service_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::audio_detail;
using namespace inputnext::audio_integration_test_support;
using namespace inputnext::output;

[[nodiscard]] native_guid unique_container(const std::size_t index) noexcept {
    auto container = gameinput_test_support::available_container().value;
    container.bytes[1U] = static_cast<std::byte>(index);
    return container;
}

[[nodiscard]] connection_target connect_target_with_facts(audio_service_fixture& fixture,
    const std::size_t device_index,
    const std::u16string_view endpoint,
    const native_guid container) {
    const auto previous = fixture.graph.backing.runtime->connections();
    auto facts = gameinput_test_support::runtime_fixture::native_facts(std::u16string{endpoint});
    facts.container_id.value = container;
    const auto device =
        gameinput_test_support::handle(fixture.graph.backing.device_memory[device_index]);
    fixture.graph.backing.native->update_facts(device, std::move(facts));
    fixture.graph.backing.connect(device);
    fixture.graph.backing.maintain();
    const auto inventory = fixture.graph.backing.runtime->connections();
    for (const auto& candidate : std::span{inventory.entries}.first(inventory.size)) {
        const auto existed = std::ranges::find_if(std::span{previous.entries}.first(previous.size),
            [&](const auto& old) { return old.target == candidate.target; });
        if (existed == std::span{previous.entries}.first(previous.size).end()) {
            return candidate.target;
        }
    }
    if (inventory.size == 0U) {
        throw std::runtime_error{"audio residency fixture did not connect its GameInput target"};
    }
    throw std::runtime_error{"audio residency fixture did not identify its new GameInput target"};
}

[[nodiscard]] bool no_candidate_opened(const audio_service_fixture& fixture,
    const std::size_t first_candidate,
    const std::size_t created_count) {
    return std::ranges::all_of(
        std::span{fixture.native_states}.subspan(first_candidate, created_count - first_candidate),
        [](const auto& state) { return state->count_calls(native_operation::get_device) == 0U; });
}

[[nodiscard]] bool wait_for_worker_join_attempt(
    const std::shared_ptr<test::audio::controlled_audio_worker_state>& state,
    const std::size_t index) {
    return wait_until([&state, index] { return state->join_attempts(index) != 0U; }, 10s);
}

} // namespace

TEST_CASE("audio.resource.retains_container_reservation_until_native_owner_is_joined",
    "[integration][audio]") {
    const auto worker_state = std::make_shared<test::audio::controlled_audio_worker_state>();
    worker_state->block_join_for(0U);
    audio_service_fixture fixture{false, std::nullopt, false, false, worker_state};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(live);

    const auto original_state = fixture.native_states.front();
    REQUIRE(original_state != nullptr);
    original_state->fail_on(native_operation::wait, 23U);
    original_state->signal_buffer_event();
    REQUIRE(wait_until([&original_state] {
        return original_state->count_calls(native_operation::finish_worker_sta) == 1U;
    }));
    REQUIRE(wait_for_worker_join_attempt(worker_state, 0U));
    REQUIRE(wait_until([&fixture] {
        const auto progress = fixture.service->progress();
        return progress.resident_slots == 1U && progress.retained_slots == 1U;
    }));

    const auto alias_state = fixture.configure_native_port(1U, u"new-endpoint");
    alias_state->block_on(native_operation::finish_worker_sta);
    release_controlled_native_block release_alias_cleanup{alias_state};
    for (std::size_t index = 2U; index < test::audio::controlled_native_port_capacity; ++index) {
        static_cast<void>(fixture.configure_native_port(index, u"new-endpoint"));
    }
    const auto old_device = gameinput_test_support::handle(fixture.graph.backing.device_memory[0U]);
    fixture.graph.backing.native->emit(old_device,
        gameinput_detail::native_device_event::disconnected);
    fixture.graph.backing.maintain();
    const auto second_target = connect_target_with_facts(fixture,
        1U,
        u"new-endpoint",
        gameinput_test_support::available_container().value);
    const auto inventory = fixture.graph.backing.runtime->connections();
    REQUIRE(inventory.size == 1U);
    fixture.graph.backing.native->emit(
        gameinput_test_support::handle(fixture.graph.backing.device_memory[1U]),
        gameinput_detail::native_device_event::haptic_info_updated);
    REQUIRE(fixture.synchronize_inventory(inventory));
    REQUIRE(fixture.wait_for_backend(second_target));

    const bool alias_cleanup_blocked = wait_until(
        [&fixture, &alias_state] {
            fixture.graph.backing.maintain();
            return alias_state->wait_until_blocked(native_operation::finish_worker_sta, 0ms);
        },
        5s);
    INFO(fixture.readiness_diagnostic(second_target));
    REQUIRE(alias_cleanup_blocked);
    CHECK(alias_state->count_calls(native_operation::get_device) == 0U);
    release_alias_cleanup.release();
    REQUIRE(wait_for_worker_join_attempt(worker_state, 1U));
    REQUIRE(fixture.wait_for_residents(1U));
    CHECK(alias_state->count_calls(native_operation::get_device) == 0U);
    CHECK(fixture.service->progress().retained_slots == 1U);
    const auto created_before_owner_join = fixture.native_factory->created_count();
    REQUIRE(created_before_owner_join >= 2U);
    CHECK(no_candidate_opened(fixture, 1U, created_before_owner_join));
    const auto blocked_candidate = fixture.service->progress(second_target);
    CHECK_FALSE((blocked_candidate.has_value() && blocked_candidate->ready));

    worker_state->allow_join_for(0U);
    REQUIRE(fixture.wait_for_live(second_target));
    bool recovered_after_join = false;
    for (std::size_t index = 2U; index < fixture.native_states.size(); ++index) {
        recovered_after_join = recovered_after_join || fixture.native_states[index]->count_calls(
                                                           native_operation::get_device) == 1U;
    }
    CHECK(recovered_after_join);
}

TEST_CASE("audio.residency.keeps_sixteen_workers_until_a_blocked_join_completes",
    "[integration][audio][capacity]") {
    const auto worker_state = std::make_shared<test::audio::controlled_audio_worker_state>();
    worker_state->block_join_for(0U);
    audio_service_fixture fixture{false, std::nullopt, false, true, worker_state};

    std::array<connection_target, gameinput_live_connection_capacity> targets{};
    targets[0U] = fixture.graph.target;
    for (std::size_t index = 1U; index < gameinput_live_connection_capacity; ++index) {
        const auto endpoint = std::u16string{u"endpoint-"} + std::u16string(index, u'x');
        const auto state = fixture.configure_native_port(index, endpoint, unique_container(index));
        state->block_on(native_operation::begin_worker_sta);
        targets[index] =
            connect_target_with_facts(fixture, index, endpoint, unique_container(index));
    }

    const auto full_inventory = fixture.graph.backing.runtime->connections();
    REQUIRE(full_inventory.size == gameinput_live_connection_capacity);
    REQUIRE(fixture.synchronize_inventory(full_inventory));
    REQUIRE(wait_until(
        [&fixture, &targets] {
            return std::ranges::all_of(targets, [&fixture](const connection_target target) {
                const auto current = fixture.graph.connections->query_backend_progress(target);
                return current.has_value() && current->phase == backend_phase::current;
            });
        },
        10s));
    REQUIRE(wait_until(
        [&fixture] {
            return fixture.native_factory->created_count() == gameinput_live_connection_capacity &&
                   fixture.service->progress().resident_slots == gameinput_live_connection_capacity;
        },
        10s));
    CHECK(worker_state->created_workers() == gameinput_live_connection_capacity);

    const auto replacement_endpoint = std::u16string{u"replacement-endpoint"};
    const auto replacement_container = unique_container(gameinput_live_connection_capacity + 1U);
    for (std::size_t index = gameinput_live_connection_capacity;
        index < test::audio::controlled_native_port_capacity;
        ++index) {
        static_cast<void>(
            fixture.configure_native_port(index, replacement_endpoint, replacement_container));
    }
    const auto old_device = gameinput_test_support::handle(fixture.graph.backing.device_memory[0U]);
    fixture.graph.backing.native->emit(old_device,
        gameinput_detail::native_device_event::disconnected);
    fixture.graph.backing.maintain();
    const auto replacement_target = connect_target_with_facts(fixture,
        gameinput_live_connection_capacity,
        replacement_endpoint,
        replacement_container);
    const auto replacement_inventory = fixture.graph.backing.runtime->connections();
    REQUIRE(replacement_inventory.size == gameinput_live_connection_capacity);
    // A rescan can request fresh facts after the last explicit maintenance turn.
    fixture.graph.backing.native->emit(
        gameinput_test_support::handle(
            fixture.graph.backing.device_memory[gameinput_live_connection_capacity]),
        gameinput_detail::native_device_event::haptic_info_updated);
    REQUIRE(fixture.synchronize_inventory(replacement_inventory));
    const bool replacement_ready = fixture.wait_for_backend(replacement_target);
    INFO(fixture.readiness_diagnostic(replacement_target));
    REQUIRE(replacement_ready);

    CHECK(fixture.native_factory->created_count() == gameinput_live_connection_capacity);
    const auto retiring_state = fixture.native_states.front();
    REQUIRE(retiring_state != nullptr);
    retiring_state->release_block();
    REQUIRE(wait_until(
        [&retiring_state] {
            return retiring_state->count_calls(native_operation::finish_worker_sta) == 1U;
        },
        10s));
    REQUIRE(wait_for_worker_join_attempt(worker_state, 0U));
    CHECK(fixture.service->progress().resident_slots == gameinput_live_connection_capacity);
    CHECK(fixture.native_factory->created_count() == gameinput_live_connection_capacity);

    worker_state->allow_join_for(0U);
    const bool replacement_live = fixture.wait_for_live(replacement_target);
    INFO(fixture.readiness_diagnostic(replacement_target));
    REQUIRE(replacement_live);
    REQUIRE(wait_until(
        [&fixture] {
            return fixture.native_factory->created_count() >=
                       gameinput_live_connection_capacity + 1U &&
                   fixture.service->progress().resident_slots == gameinput_live_connection_capacity;
        },
        10s));
    const auto replacements =
        std::span{fixture.native_states}.subspan(gameinput_live_connection_capacity);
    CHECK(std::ranges::any_of(replacements,
        [](const auto& state) { return state->count_calls(native_operation::get_device) == 1U; }));
    CHECK(worker_state->created_workers() >= gameinput_live_connection_capacity + 1U);
}
