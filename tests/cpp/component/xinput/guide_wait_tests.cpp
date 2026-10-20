#include "support/xinput/controlled_event_source.hpp"

#include "support/xinput/event_fixture.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

constexpr std::uint64_t qpc_frequency = 1000U;
constexpr button_mask guide_mask = static_cast<button_mask>(gamepad_button::guide);

[[nodiscard]] xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto result = service.assign(connection.connection.target, connection.connection.reader);
    if (!result.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *result;
}

[[nodiscard]] bool wait_until(const auto& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

class cancel_ticket_on_exit final {
public:
    cancel_ticket_on_exit(xinput::service& service, xinput::guide_wait_ticket ticket) noexcept
        : service_(service),
          ticket_(ticket) {}

    ~cancel_ticket_on_exit() {
        const auto cancellation = service_.cancel_guide_wait(ticket_);
        static_cast<void>(cancellation.has_value());
    }

    cancel_ticket_on_exit(const cancel_ticket_on_exit&) = delete;
    cancel_ticket_on_exit& operator=(const cancel_ticket_on_exit&) = delete;
    cancel_ticket_on_exit(cancel_ticket_on_exit&&) = delete;
    cancel_ticket_on_exit& operator=(cancel_ticket_on_exit&&) = delete;

private:
    xinput::service& service_;
    xinput::guide_wait_ticket ticket_;
};

[[nodiscard]] gamepad_state with_guide(bool pressed) noexcept {
    return gamepad_state{.buttons = pressed ? guide_mask : 0U};
}

} // namespace

TEST_CASE("typed Guide wait arms on baseline and requires a known release then press",
    "[component][xinput][guide]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto started = service.begin_guide_wait(occupant.user_index);
    REQUIRE(started.has_value());
    const auto ticket = *started;

    std::optional<xinput::guide_wait_result<void>> result;
    std::atomic<bool> done{};
    std::jthread waiter([&] {
        result = service.wait_for_guide_button(ticket);
        done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));

    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 201U, with_guide(true), 100U, true));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));
    CHECK_FALSE(done.load(std::memory_order_acquire));

    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{201U},
            2U,
            {{with_guide(false), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));

    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{201U},
            3U,
            {{with_guide(true), 300U}},
            {},
            300U));
    service.pump_events(occupant.user_index, focus_state.now());
    REQUIRE(wait_until([&] { return done.load(std::memory_order_acquire); }));
    waiter.join();
    if (!result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(result->has_value());
    CHECK(service.wait_for_guide_button(ticket) ==
          std::unexpected{xinput::guide_wait_error::invalid_ticket});

    auto another = service.begin_guide_wait(occupant.user_index);
    REQUIRE(another.has_value());
    CHECK(service.release_guide_wait(*another).has_value());
}

TEST_CASE("unknown Guide baseline and unknown-to-known held state do not signal",
    "[component][xinput][guide]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto started = service.begin_guide_wait(occupant.user_index);
    REQUIRE(started.has_value());
    const auto ticket = *started;

    std::optional<xinput::guide_wait_result<void>> result;
    std::atomic<bool> done{};
    std::jthread waiter([&] {
        result = service.wait_for_guide_button(ticket);
        done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));

    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 211U, with_guide(true), 100U, false));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{211U},
            2U,
            {{with_guide(true), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));
    CHECK_FALSE(done.load(std::memory_order_acquire));

    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{211U},
            3U,
            {{with_guide(false), 300U}},
            {},
            300U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{211U},
            4U,
            {{with_guide(true), 400U}},
            {},
            400U));
    service.pump_events(occupant.user_index, focus_state.now());
    REQUIRE(wait_until([&] { return done.load(std::memory_order_acquire); }));
    waiter.join();
    if (!result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(result->has_value());
}

TEST_CASE("Guide registration leaves stale idle reader cleanup to a later pump",
    "[component][xinput][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 216U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    const auto original_reader =
        xinput::detail::service_test_access::event_reader(service, occupant.user_index);
    REQUIRE(original_reader.has_value());

    focus_state.set(focus::evidence::background);
    focus_state.set(focus::evidence::foreground);
    events.block_next_close();

    std::optional<xinput::guide_wait_result<xinput::guide_wait_ticket>> result;
    std::atomic<bool> done{};
    std::jthread registrar([&] {
        result = service.begin_guide_wait(occupant.user_index);
        done.store(true, std::memory_order_release);
    });
    const release_event_source_on_exit release{events};
    const bool returned_without_waiting =
        wait_until([&] { return done.load(std::memory_order_acquire); });
    events.release_close();
    registrar.join();
    REQUIRE(returned_without_waiting);
    if (!result.has_value()) {
        FAIL("Guide registration did not produce a result");
        return;
    }
    if (!result->has_value()) {
        FAIL("eligible Guide registration did not produce a ticket");
        return;
    }
    const auto ticket = **result;
    CHECK(events.close_calls() == 0U);
    CHECK(xinput::detail::service_test_access::event_reader(service, occupant.user_index) ==
          original_reader);
    CHECK(xinput::detail::service_test_access::event_cleanup_pending(service, occupant.user_index));

    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(events.close_calls() == 1U);
    CHECK_FALSE(xinput::detail::service_test_access::event_reader(service, occupant.user_index)
            .has_value());
    CHECK(service.release_guide_wait(ticket).has_value());
}

TEST_CASE("Guide cancellation is serialized with an active waiter and retained until consumption",
    "[component][xinput][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto started = service.begin_guide_wait(occupant.user_index);
    REQUIRE(started.has_value());
    const auto ticket = *started;

    std::optional<xinput::guide_wait_result<void>> result;
    std::atomic<bool> done{};
    std::jthread waiter([&] {
        result = service.wait_for_guide_button(ticket);
        done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));

    CHECK(service.wait_for_guide_button(ticket) == std::unexpected{xinput::guide_wait_error::busy});
    const auto cancelled = service.cancel_guide_wait(ticket);
    REQUIRE(cancelled.has_value());
    CHECK(*cancelled);
    REQUIRE(wait_until([&] { return done.load(std::memory_order_acquire); }));
    waiter.join();
    if (!result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(result->error() == xinput::guide_wait_error::cancelled);
    CHECK(service.cancel_guide_wait(ticket) ==
          std::unexpected{xinput::guide_wait_error::invalid_ticket});

    const auto next = service.begin_guide_wait(occupant.user_index);
    REQUIRE(next.has_value());
    CHECK(service.release_guide_wait(*next).has_value());
}

TEST_CASE("history reset retains a terminal Guide ticket until release",
    "[component][xinput][guide][recovery]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto started = service.begin_guide_wait(occupant.user_index);
    REQUIRE(started.has_value());
    const auto ticket = *started;
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 221U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());

    auto reset = initial_batch(connection.connection.target, 222U, with_guide(true), 200U, true);
    reset.reset = history_reset::history_gap;
    reset.loss = history_loss::unknown;
    events.enqueue(occupant.user_index, reset);
    service.pump_events(occupant.user_index, focus_state.now());

    const auto capacity = service.begin_guide_wait(occupant.user_index);
    REQUIRE_FALSE(capacity.has_value());
    CHECK(capacity.error() == xinput::guide_wait_error::capacity_exceeded);
    CHECK(service.wait_for_guide_button(ticket) ==
          std::unexpected{xinput::guide_wait_error::history_reset});
    CHECK(service.release_guide_wait(ticket) ==
          std::unexpected{xinput::guide_wait_error::invalid_ticket});

    const auto replacement = service.begin_guide_wait(occupant.user_index);
    REQUIRE(replacement.has_value());
    CHECK(service.release_guide_wait(*replacement).has_value());
}

TEST_CASE("Guide success uses fresh epoch qualification beyond the snapshot lease",
    "[component][xinput][guide][focus]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto started = service.begin_guide_wait(occupant.user_index);
    REQUIRE(started.has_value());
    const auto ticket = *started;
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 226U, with_guide(false), 100U, true));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{226U},
            2U,
            {{with_guide(true), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(xinput::detail::service_test_access::guide_wait_signalled(service, occupant.user_index));

    const auto completed = focus_state.published().completed_observations;
    focus_state.advance(std::chrono::milliseconds{200});
    focus_state.wait_for_refresh_after(completed);
    focus_state.advance(std::chrono::milliseconds{60});
    CHECK(service.wait_for_guide_button(ticket).has_value());

    const auto next = service.begin_guide_wait(occupant.user_index);
    REQUIRE(next.has_value());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{226U},
            3U,
            {{with_guide(false), 300U}},
            {},
            300U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{226U},
            4U,
            {{with_guide(true), 400U}},
            {},
            400U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(xinput::detail::service_test_access::guide_wait_signalled(service, occupant.user_index));
    focus_state.set(focus::evidence::background);
    CHECK(service.wait_for_guide_button(*next) ==
          std::unexpected{xinput::guide_wait_error::ineligible});
}

TEST_CASE("Guide ticket IDs stop at the maximum without wrapping or replacing retained tickets",
    "[component][xinput][guide][identity]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    xinput::detail::service_test_access::seed_guide_ticket_id(service,
        std::numeric_limits<std::uint64_t>::max());

    const auto ticket = service.begin_guide_wait(first_assignment.user_index);
    REQUIRE(ticket.has_value());
    CHECK(ticket->id == std::numeric_limits<std::uint64_t>::max());
    const auto exhausted = service.begin_guide_wait(second_assignment.user_index);
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error() == xinput::guide_wait_error::identity_exhausted);
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service,
        first_assignment.user_index));
    CHECK(service.release_guide_wait(*ticket).has_value());
}

TEST_CASE(
    "Enable generation invalidates blocked work and event exhaustion leaves packet service live",
    "[component][xinput][guide][identity]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto initial_generation =
        xinput::detail::service_test_access::event_enable_generation(service);
    service.enable(TRUE);
    CHECK(xinput::detail::service_test_access::event_enable_generation(service) ==
          initial_generation);
    service.enable(FALSE);
    const auto disabled_generation =
        xinput::detail::service_test_access::event_enable_generation(service);
    service.enable(FALSE);
    CHECK(xinput::detail::service_test_access::event_enable_generation(service) ==
          disabled_generation);
    service.enable(TRUE);
    CHECK(xinput::detail::service_test_access::event_enable_generation(service) ==
          disabled_generation + 1U);

    xinput::detail::service_test_access::seed_event_enable_generation(service,
        std::numeric_limits<std::uint64_t>::max());
    service.enable(FALSE);
    service.enable(TRUE);
    const auto ticket = service.begin_guide_wait(occupant.user_index);
    REQUIRE_FALSE(ticket.has_value());
    CHECK(ticket.error() == xinput::guide_wait_error::identity_exhausted);

    XINPUT_STATE state{};
    CHECK(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
}

TEST_CASE("close wakes a Guide waiter before a blocked history read finishes",
    "[component][xinput][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto ticket_result = service.begin_guide_wait(occupant.user_index);
    REQUIRE(ticket_result.has_value());
    const auto ticket = *ticket_result;
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 231U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{231U},
            2U,
            {{with_guide(true), 200U}},
            {},
            200U));
    events.block_next_read();
    std::jthread pump([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));

    std::optional<xinput::guide_wait_result<void>> result;
    std::atomic<bool> done{};
    std::jthread waiter([&] {
        result = service.wait_for_guide_button(ticket);
        done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));

    service.close();
    CHECK(wait_until([&] { return done.load(std::memory_order_acquire); }));
    CHECK(xinput::detail::service_test_access::event_operation_reserved(service,
        occupant.user_index));
    if (!result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(result->error() == xinput::guide_wait_error::closed);
    events.release_read();
    waiter.join();
    pump.join();
}

TEST_CASE("unassign notifies its Guide waiter before a blocked reader close",
    "[component][xinput][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto replacement = devices.add();
    const auto occupant = assign(service, connection);
    const auto ticket_result = service.begin_guide_wait(occupant.user_index);
    REQUIRE(ticket_result.has_value());
    const auto ticket = *ticket_result;
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 236U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());

    std::optional<xinput::guide_wait_result<void>> wait_result;
    std::atomic<bool> waiter_done{};
    std::jthread waiter([&] {
        wait_result = service.wait_for_guide_button(ticket);
        waiter_done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));

    events.block_next_close();
    std::atomic<bool> unassign_done{};
    std::jthread unassigner([&] {
        const auto unassigned = service.unassign(occupant);
        if (!unassigned.has_value()) {
            FAIL("unassign must complete while reader close is blocked");
            return;
        }
        unassign_done.store(true, std::memory_order_release);
    });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_close(std::chrono::seconds{2}));
    CHECK(wait_until([&] { return waiter_done.load(std::memory_order_acquire); }));
    if (!wait_result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(wait_result->error() == xinput::guide_wait_error::not_connected);
    CHECK_FALSE(unassign_done.load(std::memory_order_acquire));

    const auto reassigned = assign(service, replacement);
    XINPUT_STATE state{};
    CHECK(service.get_state(xinput::flavor::v1_4, reassigned.user_index, &state) == ERROR_SUCCESS);

    events.release_close();
    waiter.join();
    unassigner.join();
    CHECK(unassign_done.load(std::memory_order_acquire));
}

TEST_CASE("cancel wakes its waiter while a history read remains blocked",
    "[component][xinput][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    const auto ticket_result = service.begin_guide_wait(occupant.user_index);
    REQUIRE(ticket_result.has_value());
    const auto ticket = *ticket_result;
    events.enqueue(occupant.user_index,
        initial_batch(connection.connection.target, 241U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{241U},
            2U,
            {{with_guide(true), 200U}},
            {},
            200U));
    events.block_next_read();
    std::jthread pump([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));

    std::optional<xinput::guide_wait_result<void>> result;
    std::atomic<bool> done{};
    std::jthread waiter([&] {
        result = service.wait_for_guide_button(ticket);
        done.store(true, std::memory_order_release);
    });
    const cancel_ticket_on_exit cancel{service, ticket};
    REQUIRE(wait_until([&] {
        return xinput::detail::service_test_access::guide_awaiter_active(service,
            occupant.user_index);
    }));
    const auto cancelled = service.cancel_guide_wait(ticket);
    REQUIRE(cancelled.has_value());
    CHECK(*cancelled);
    CHECK(wait_until([&] { return done.load(std::memory_order_acquire); }));
    if (!result.has_value()) {
        FAIL("Guide wait returned before producing a result");
        return;
    }
    CHECK(result->error() == xinput::guide_wait_error::cancelled);
    CHECK(xinput::detail::service_test_access::event_operation_reserved(service,
        occupant.user_index));
    events.release_read();
    waiter.join();
    pump.join();
}
