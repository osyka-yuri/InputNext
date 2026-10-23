#include "focus/observer_test_access.hpp"

#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace inputnext;
using namespace inputnext::focus;

namespace {

class controlled_epoch_source final : public source {
public:
    explicit controlled_epoch_source(process_instance process) noexcept : process_(process) {}

    [[nodiscard]] process_instance process() const noexcept override { return process_; }

    [[nodiscard]] reading observe() noexcept override {
        std::unique_lock lock{mutex_};
        const auto call = ++calls_;
        if (call == blocked_call_ && !all_released_) {
            blocked_call_active_ = call;
            released_ = false;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        }
        changed_.notify_all();
        return reading{.process = process_, .state = evidence::foreground};
    }

    [[nodiscard]] bool wait_until_blocked(std::uint64_t call,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, call] {
            return blocked_call_active_ == call;
        });
    }

    void block_next_observation() noexcept {
        std::scoped_lock lock{mutex_};
        blocked_call_ = calls_ + 1U;
    }

    void release_current() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

    void release_all() noexcept {
        std::scoped_lock lock{mutex_};
        all_released_ = true;
        released_ = true;
        changed_.notify_all();
    }

private:
    const process_instance process_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t calls_{};
    std::uint64_t blocked_call_{2U};
    std::uint64_t blocked_call_active_{};
    bool released_{};
    bool all_released_{};
};

class source_release final {
public:
    explicit source_release(controlled_epoch_source& source) noexcept : source_(source) {}

    ~source_release() { source_.release_all(); }

    source_release(const source_release&) = delete;
    source_release& operator=(const source_release&) = delete;
    source_release(source_release&&) = delete;
    source_release& operator=(source_release&&) = delete;

private:
    controlled_epoch_source& source_;
};

[[nodiscard]] runtime_id make_incarnation(std::byte value) noexcept {
    runtime_id result{};
    result.bytes[0] = value;
    return result;
}

[[nodiscard]] bool wait_for_publication(observer& value,
    const auto& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate(value.published())) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate(value.published());
}

} // namespace

TEST_CASE("focus.epoch_source_cleanup_releases_current_and_future_waits",
    "[component][focus][concurrency]") {
    using namespace std::chrono_literals;
    controlled_epoch_source source{process_instance{.pid = 812U, .creation_time_100ns = 992U}};
    std::promise<void> completed;
    const auto completion = completed.get_future();
    std::jthread reader;
    const source_release cleanup{source};
    const auto observe = [&] {
        for (std::uint32_t index = 0U; index < 3U; ++index) {
            static_cast<void>(source.observe());
        }
        completed.set_value();
    };

    SECTION("release before entering the selected wait") {
        source.release_all();
        reader = std::jthread{observe};
    }
    SECTION("scope cleanup releases the current wait") {
        reader = std::jthread{observe};
        REQUIRE(source.wait_until_blocked(2U));
        {
            const source_release release{source};
        }
    }
    CHECK(completion.wait_for(2s) == std::future_status::ready);
    source.release_all();
    reader.join();
}

TEST_CASE("foreground epoch survives timestamp refresh while captured snapshot lease expires",
    "[component][focus][freshness]") {
    test::focus::manual_clock clock{};
    const process_instance process{.pid = 811U, .creation_time_100ns = 991U};
    auto source = std::make_unique<controlled_epoch_source>(process);
    auto* const source_view = source.get();
    const auto value =
        detail::observer_test_access::make_with_clock(make_incarnation(std::byte{81U}),
            std::move(source),
            &test::focus::manual_clock::read,
            &clock);
    const source_release cleanup{*source_view};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.state == evidence::foreground && published.revision == 1U;
    }));
    REQUIRE(source_view->wait_until_blocked(2U));
    snapshot captured{};
    foreground_epoch epoch{};
    {
        const auto guard = value->lock_presentation();
        captured = guard.published();
        const auto current = guard.current_foreground_epoch();
        CHECK(current.has_value());
        epoch = current.value_or(foreground_epoch{});
    }

    clock.advance(std::chrono::milliseconds{200});
    source_view->block_next_observation();
    source_view->release_current();
    REQUIRE(source_view->wait_until_blocked(3U));
    source_view->block_next_observation();
    source_view->release_current();
    REQUIRE(source_view->wait_until_blocked(4U));
    const auto refreshed_at = clock.now();
    REQUIRE(wait_for_publication(*value, [refreshed_at](snapshot published) {
        return published.completed_observations >= 3U &&
               published.observation_started == refreshed_at &&
               published.state == evidence::foreground;
    }));

    {
        const auto guard = value->lock_presentation();
        CHECK(guard.current_foreground_epoch() == epoch);
        clock.advance(std::chrono::milliseconds{60});
        CHECK(guard.matches_foreground_epoch(epoch));
        CHECK_FALSE(guard.matches_foreground(captured));
        CHECK(guard.published().state == evidence::foreground);
    }

    clock.advance(std::chrono::milliseconds{190});
    {
        const auto guard = value->lock_presentation();
        CHECK_FALSE(guard.matches_foreground_epoch(epoch));
        CHECK_FALSE(guard.current_foreground_epoch().has_value());
        CHECK(guard.published().state == evidence::unavailable);
    }
    source_view->release_all();
    value->stop();
}
