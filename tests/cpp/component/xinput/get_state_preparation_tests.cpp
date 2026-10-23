#include "support/focus/controlled_source.hpp"

#include "focus/observer_test_access.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

using namespace std::chrono_literals;

class pause_clock final {
public:
    using time_point = std::chrono::steady_clock::time_point;

    [[nodiscard]] static time_point read(const void* context) noexcept {
        return static_cast<const pause_clock*>(context)->now();
    }

    void arm() noexcept {
        std::scoped_lock lock{mutex_};
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    [[nodiscard]] bool wait_until_paused() const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() noexcept {
        std::scoped_lock lock{mutex_};
        armed_ = false;
        released_ = true;
        changed_.notify_all();
    }

private:
    [[nodiscard]] time_point now() const noexcept {
        std::unique_lock lock{mutex_};
        if (armed_) {
            armed_ = false;
            entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        }
        return {};
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    mutable bool armed_{};
    mutable bool entered_{};
    mutable bool released_{};
};

class stable_focus final {
public:
    stable_focus() {
        const focus::process_instance process{.pid = 812U, .creation_time_100ns = 19031U};
        auto source = std::make_unique<test::focus::controlled_source>(process,
            std::vector{focus::reading{.process = process, .state = focus::evidence::foreground}},
            2U);
        source_ = source.get();
        observer_ = focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x71U),
            std::move(source),
            &pause_clock::read,
            &clock_);
    }

    ~stable_focus() {
        clock_.release();
        source_->release();
        observer_->stop();
    }

    stable_focus(const stable_focus&) = delete;
    stable_focus& operator=(const stable_focus&) = delete;
    stable_focus(stable_focus&&) = delete;
    stable_focus& operator=(stable_focus&&) = delete;

    [[nodiscard]] bool wait_until_ready() { return source_->wait_until_blocked(); }

    [[nodiscard]] focus::observer& observer() noexcept { return *observer_; }

    [[nodiscard]] pause_clock& clock() noexcept { return clock_; }

private:
    pause_clock clock_;
    test::focus::controlled_source* source_{};
    std::unique_ptr<focus::observer> observer_;
};

class release_clock final {
public:
    explicit release_clock(pause_clock& clock) noexcept : clock_(clock) {}

    ~release_clock() { clock_.release(); }

    release_clock(const release_clock&) = delete;
    release_clock& operator=(const release_clock&) = delete;
    release_clock(release_clock&&) = delete;
    release_clock& operator=(release_clock&&) = delete;

private:
    pause_clock& clock_;
};

} // namespace

TEST_CASE("GetState preparation waits for the presentation guard and captures coherent state",
    "[component][xinput][concurrency]") {
    stable_focus focus_state;
    REQUIRE(focus_state.wait_until_ready());
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());

    xinput::detail::service_test_access::prepared_read prepared{};
    std::promise<void> started;
    const auto started_future = started.get_future();
    std::promise<DWORD> completed;
    auto completed_future = completed.get_future();
    std::jthread reader;
    {
        [[maybe_unused]] const auto guard = focus_state.observer().lock_presentation();
        reader = std::jthread([&] {
            started.set_value();
            completed.set_value(xinput::detail::service_test_access::prepare(service,
                occupant->user_index,
                false,
                prepared));
        });
        REQUIRE(started_future.wait_for(2s) == std::future_status::ready);
        CHECK(completed_future.wait_for(50ms) == std::future_status::timeout);
    }
    reader.join();
    REQUIRE(completed_future.get() == ERROR_SUCCESS);
    CHECK(prepared.occupant == *occupant);
    CHECK(prepared.eligible_at_capture);
    REQUIRE(prepared.sample_pin.has_value());
    const auto target =
        prepared.sample_pin.transform([](const read_pin& pin) { return pin.target(); });
    CHECK(target == connection.connection.target);
}

TEST_CASE("GetState initial capture excludes concurrent assignment replacement",
    "[component][xinput][concurrency]") {
    stable_focus focus_state;
    REQUIRE(focus_state.wait_until_ready());
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto occupant = service.assign(first.connection.target, first.connection.reader);
    REQUIRE(occupant.has_value());

    xinput::detail::service_test_access::prepared_read prepared{};
    std::promise<DWORD> read_completed;
    auto read_future = read_completed.get_future();
    std::promise<void> writer_started;
    const auto writer_started_future = writer_started.get_future();
    std::promise<result<xinput::assignment>> replacement_completed;
    auto replacement_future = replacement_completed.get_future();
    std::jthread reader;
    std::jthread writer;
    const release_clock cleanup{focus_state.clock()};

    focus_state.clock().arm();
    reader = std::jthread([&] {
        read_completed.set_value(xinput::detail::service_test_access::prepare(service,
            occupant->user_index,
            false,
            prepared));
    });
    REQUIRE(focus_state.clock().wait_until_paused());
    writer = std::jthread([&] {
        writer_started.set_value();
        const auto removed = service.unassign(*occupant);
        if (!removed.has_value()) {
            replacement_completed.set_value(std::unexpected{removed.error()});
            return;
        }
        replacement_completed.set_value(
            service.assign(second.connection.target, second.connection.reader));
    });
    REQUIRE(writer_started_future.wait_for(2s) == std::future_status::ready);
    CHECK(replacement_future.wait_for(50ms) == std::future_status::timeout);
    focus_state.clock().release();
    reader.join();
    writer.join();

    REQUIRE(read_future.get() == ERROR_SUCCESS);
    const auto replacement = replacement_future.get();
    REQUIRE(replacement.has_value());
    CHECK(prepared.occupant == *occupant);
    CHECK(prepared.eligible_at_capture);
    REQUIRE(prepared.sample_pin.has_value());
    const auto original_target =
        prepared.sample_pin.transform([](const read_pin& pin) { return pin.target(); });
    CHECK(original_target == first.connection.target);
    CHECK(replacement->user_index == occupant->user_index);
    CHECK(replacement->id != occupant->id);

    XINPUT_STATE stale{.dwPacketNumber = 0xC0FFEEU, .Gamepad = {}};
    stale.Gamepad.wButtons = 0xA55AU;
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &stale) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(stale.dwPacketNumber == 0xC0FFEEU);
    CHECK(stale.Gamepad.wButtons == 0xA55AU);

    xinput::detail::service_test_access::prepared_read current{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                replacement->user_index,
                false,
                current) == ERROR_SUCCESS);
    CHECK(current.occupant == *replacement);
    REQUIRE(current.sample_pin.has_value());
    const auto current_target =
        current.sample_pin.transform([](const read_pin& pin) { return pin.target(); });
    CHECK(current_target == second.connection.target);
}
