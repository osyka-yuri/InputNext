#include "support/focus/controlled_source.hpp"

#include "focus/observer_test_access.hpp"
#include "support/focus/manual_clock.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/source_lifetime.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

using namespace std::chrono_literals;

class source_release final {
public:
    explicit source_release(test::focus::controlled_source& source) noexcept : source_(source) {}

    ~source_release() { source_.release(); }

    source_release(const source_release&) = delete;
    source_release& operator=(const source_release&) = delete;
    source_release(source_release&&) = delete;
    source_release& operator=(source_release&&) = delete;

private:
    test::focus::controlled_source& source_;
};

class counting_clock final {
public:
    using clock = std::chrono::steady_clock;
    using duration = clock::duration;
    using time_point = clock::time_point;
    using rep = duration::rep;

    [[nodiscard]] time_point now() const noexcept {
        return time_point{duration{ticks_.load(std::memory_order_relaxed)}};
    }

    void advance(duration amount) noexcept {
        ticks_.fetch_add(amount.count(), std::memory_order_relaxed);
    }

    void reset_calls() const noexcept { calls_.store(0U, std::memory_order_relaxed); }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static time_point read(const void* context) noexcept {
        const auto& clock = *static_cast<const counting_clock*>(context);
        clock.calls_.fetch_add(1U, std::memory_order_relaxed);
        return clock.now();
    }

private:
    std::atomic<rep> ticks_;
    mutable std::atomic<std::size_t> calls_;
};

template <typename Predicate>
[[nodiscard]] bool wait_for_publication(const focus::observer& observer, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        if (predicate(observer.published())) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

[[nodiscard]] xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto result = service.assign(connection.connection.target, connection.connection.reader);
    if (!result.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *result;
}

[[nodiscard]] test_connection admit_unpublished(registry_fixture& devices) {
    std::unique_ptr<source_lifetime> candidate = std::make_unique<test_source_lifetime>();
    auto admitted = devices.registry().admit(candidate);
    if (!admitted.has_value()) {
        throw std::runtime_error{"test device registry admission failed"};
    }
    return test_connection{.connection = *admitted};
}

} // namespace

TEST_CASE("GetState samples initial focus once when disabled and retains inactive qualification",
    "[component][xinput][focus]") {
    counting_clock clock;
    const focus::process_instance process{.pid = 413U, .creation_time_100ns = 9217U};
    auto controlled = std::make_unique<test::focus::controlled_source>(process,
        std::vector{focus::reading{.process = process, .state = focus::evidence::foreground}},
        2U);
    auto* const source = controlled.get();
    const auto observer =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x61U),
            std::move(controlled),
            &counting_clock::read,
            &clock);
    const source_release release{*source};

    REQUIRE(wait_for_publication(*observer, [](focus::snapshot published) {
        return published.revision == 1U && published.state == focus::evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());

    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{*observer, metadata};
    const auto connection = admit_unpublished(devices);
    const auto occupant = assign(service, connection);
    service.enable(FALSE);

    xinput::detail::service_test_access::prepared_read prepared{};
    clock.reset_calls();
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                prepared) == ERROR_SUCCESS);
    CHECK(clock.calls() == 1U);
    CHECK(prepared.focus_word.state == focus::evidence::foreground);
    CHECK(prepared.focus_word.revision == 1U);
    CHECK(!prepared.eligible_at_capture);
    CHECK(prepared.connection_pin.validate_live().has_value());

    XINPUT_STATE state{};
    REQUIRE(
        xinput::detail::service_test_access::commit(service, prepared, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);
    CHECK(clock.calls() == 1U);

    REQUIRE(devices.registry().retire(connection.connection.target).has_value());
    state.dwPacketNumber = 0xC0FFEEU;
    state.Gamepad.wButtons = 0xA55AU;
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == 0xC0FFEEU);
    CHECK(state.Gamepad.wButtons == 0xA55AU);

    source->release();
    observer->stop();
}

TEST_CASE("GetState keeps the captured foreground lease across a same-revision refresh",
    "[component][xinput][focus]") {
    test::focus::manual_clock clock;
    const focus::process_instance process{.pid = 423U, .creation_time_100ns = 9227U};
    auto controlled = std::make_unique<test::focus::controlled_source>(process,
        std::vector{focus::reading{.process = process, .state = focus::evidence::foreground}},
        2U);
    auto* const source = controlled.get();
    const auto observer =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x62U),
            std::move(controlled),
            &test::focus::manual_clock::read,
            &clock);
    const source_release release{*source};

    REQUIRE(wait_for_publication(*observer, [](focus::snapshot published) {
        return published.revision == 1U && published.state == focus::evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    REQUIRE(source->block_next_observation());

    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{*observer, metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);
    xinput::detail::service_test_access::prepared_read captured{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                captured) == ERROR_SUCCESS);
    REQUIRE(captured.connection_pin.validate_live().has_value());

    clock.advance(200ms);
    source->release_initial_observation();
    REQUIRE(source->wait_until_next_observation_blocked());
    source->release();
    const auto refreshed_at = clock.now();
    const auto refreshed =
        wait_for_publication(*observer, [refreshed_at](focus::snapshot published) {
            return published.completed_observations >= 3U &&
                   published.observation_started == refreshed_at &&
                   published.state == focus::evidence::foreground;
        });
    REQUIRE(refreshed);
    const auto refreshed_focus = observer->published();
    REQUIRE(refreshed_focus.revision == captured.focus_word.revision);
    REQUIRE(refreshed_focus.observation_started == refreshed_at);

    clock.advance(60ms);
    XINPUT_STATE state{};
    REQUIRE(
        xinput::detail::service_test_access::commit(service, captured, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    observer->stop();
}
