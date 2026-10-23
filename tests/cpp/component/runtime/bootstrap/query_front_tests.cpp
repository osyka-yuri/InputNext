#include "runtime/bootstrap/bootstrap_owner.hpp"

#include "runtime/abi/runtime_api.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/bootstrap/query_front.hpp"
#include "runtime/session/native_session.hpp"
#include <inputnext/focus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {

using namespace inputnext;
using namespace inputnext::runtime_detail;

class foreground_source final : public focus::source {
public:
    [[nodiscard]] focus::process_instance process() const noexcept override {
        return {.pid = 2468U, .creation_time_100ns = 13579U};
    }

    [[nodiscard]] focus::reading observe() noexcept override {
        return {.process = process(), .state = focus::evidence::foreground};
    }
};

[[nodiscard]] std::unique_ptr<native_session> no_session([[maybe_unused]] void* context) {
    return nullptr;
}

struct query_front_fixture final {
    std::atomic<std::uint32_t> pin_calls;
    std::atomic<std::uint32_t> owner_calls;
    std::atomic<std::uint32_t> stable_api_calls;
    std::atomic<std::uint32_t> dependency_constructions;
    std::atomic<std::uint32_t> dependency_destructions;
    std::atomic<bool> pin_succeeds{true};
    std::atomic<bool> reentered;
    std::atomic<bool> reenter_stable_api;
    std::atomic<bool> stable_api_reentry_returned_null;
    std::mutex gate;
    std::condition_variable changed;
    bool block_creation{};
    bool creation_entered{};
    bool creation_released{};
    bool fail_owner_once{};
    std::unique_ptr<bootstrap_owner> owner;
    query_front front;

    query_front_fixture()
        : front(query_front_operations{.pin_module = &pin_module,
                    .create_owner = &create_owner,
                    .stable_api = &stable_api},
              this) {}

    [[nodiscard]] bool wait_for_creation(std::chrono::milliseconds timeout) {
        std::unique_lock lock{gate};
        return changed.wait_for(lock, timeout, [this] { return creation_entered; });
    }

    void release_creation() {
        {
            std::scoped_lock lock{gate};
            creation_released = true;
        }
        changed.notify_all();
    }

    [[nodiscard]] const input_next_runtime_api_v1* query(const std::uint32_t abi = 1U) noexcept {
        return query_runtime_api(front, abi);
    }

    static BOOL pin_module(void* const context) noexcept {
        auto& fixture = *static_cast<query_front_fixture*>(context);
        ++fixture.pin_calls;
        return fixture.pin_succeeds.load(std::memory_order_relaxed) ? TRUE : FALSE;
    }

    static bootstrap_owner* create_owner(void* const context) {
        auto& fixture = *static_cast<query_front_fixture*>(context);
        const auto attempt = ++fixture.owner_calls;
        if (attempt == 1U) {
            fixture.reentered.store(fixture.query() == nullptr, std::memory_order_release);
        }
        if (fixture.block_creation) {
            std::unique_lock lock{fixture.gate};
            fixture.creation_entered = true;
            fixture.changed.notify_all();
            fixture.changed.wait(lock, [&fixture] { return fixture.creation_released; });
        }
        if (fixture.fail_owner_once) {
            fixture.fail_owner_once = false;

            struct scoped_dependency final {
                explicit scoped_dependency(query_front_fixture& owner) : fixture{owner} {
                    ++fixture.dependency_constructions;
                }

                ~scoped_dependency() { ++fixture.dependency_destructions; }

                scoped_dependency(const scoped_dependency&) = delete;
                scoped_dependency& operator=(const scoped_dependency&) = delete;
                scoped_dependency(scoped_dependency&&) = delete;
                scoped_dependency& operator=(scoped_dependency&&) = delete;

                query_front_fixture& fixture;
            };

            const scoped_dependency dependency{fixture};

            auto partial_owner =
                std::make_unique<bootstrap_owner>(std::make_unique<foreground_source>(),
                    &no_session,
                    nullptr);
            partial_owner.reset();
            throw std::runtime_error{"test owner factory failure"};
        }
        fixture.owner = std::make_unique<bootstrap_owner>(std::make_unique<foreground_source>(),
            &no_session,
            nullptr);
        return fixture.owner.get();
    }

    static const input_next_runtime_api_v1* stable_api(void* const context,
        [[maybe_unused]] bootstrap_owner* owner) noexcept {
        auto& fixture = *static_cast<query_front_fixture*>(context);
        ++fixture.stable_api_calls;
        if (fixture.reenter_stable_api.exchange(false, std::memory_order_acq_rel)) {
            fixture.stable_api_reentry_returned_null.store(fixture.query() == nullptr,
                std::memory_order_release);
        }
        return runtime_api_v1();
    }
};

struct release_creation_on_exit final {
    explicit release_creation_on_exit(query_front_fixture& value) noexcept : fixture{value} {}

    ~release_creation_on_exit() { fixture.release_creation(); }

    release_creation_on_exit(const release_creation_on_exit&) = delete;
    release_creation_on_exit& operator=(const release_creation_on_exit&) = delete;
    release_creation_on_exit(release_creation_on_exit&&) = delete;
    release_creation_on_exit& operator=(release_creation_on_exit&&) = delete;

    query_front_fixture& fixture;
};

static_assert(std::is_trivially_destructible_v<query_front>);

} // namespace

TEST_CASE("runtime.bootstrap.invalid_abi_and_rejected_pin_create_no_owner",
    "[component][runtime][bootstrap]") {
    query_front_fixture fixture;
    CHECK(fixture.query(2U) == nullptr);
    CHECK(fixture.pin_calls.load() == 0U);
    CHECK(fixture.owner_calls.load() == 0U);

    fixture.pin_succeeds.store(false, std::memory_order_relaxed);
    CHECK(fixture.query() == nullptr);
    CHECK(fixture.pin_calls.load() == 1U);
    CHECK(fixture.owner_calls.load() == 0U);
    CHECK(fixture.front.owner.load(std::memory_order_acquire) == nullptr);
    CHECK_FALSE(fixture.front.module_pinned.load(std::memory_order_acquire));
    CHECK(fixture.dependency_constructions.load() == 0U);
    CHECK(fixture.dependency_destructions.load() == 0U);

    fixture.pin_succeeds.store(true, std::memory_order_relaxed);
    const auto* const api = fixture.query();
    CHECK(api == runtime_api_v1());
    CHECK(fixture.pin_calls.load() == 2U);
    CHECK(fixture.owner_calls.load() == 1U);
    CHECK(fixture.stable_api_calls.load() == 1U);
    CHECK(fixture.front.module_pinned.load(std::memory_order_acquire));
    REQUIRE(fixture.owner != nullptr);
    CHECK(fixture.owner->state() == bootstrap_state::cold);
}

TEST_CASE("runtime.bootstrap.owner_retry_rolls_back_after_one_successful_pin",
    "[component][runtime][bootstrap]") {
    query_front_fixture fixture;
    fixture.fail_owner_once = true;

    CHECK(fixture.query() == nullptr);
    CHECK(fixture.front.owner.load(std::memory_order_acquire) == nullptr);
    CHECK(fixture.front.module_pinned.load(std::memory_order_acquire));
    CHECK(fixture.pin_calls.load() == 1U);
    CHECK(fixture.owner_calls.load() == 1U);
    CHECK(fixture.dependency_constructions.load() == 1U);
    CHECK(fixture.dependency_destructions.load() == 1U);

    const auto* const api = fixture.query();
    CHECK(api == runtime_api_v1());
    CHECK(fixture.pin_calls.load() == 1U);
    CHECK(fixture.owner_calls.load() == 2U);
    CHECK(fixture.front.owner.load(std::memory_order_acquire) != nullptr);
    CHECK(fixture.stable_api_calls.load() == 1U);
}

TEST_CASE("runtime.bootstrap.stable_front_fastpath_skips_pin_and_creation",
    "[component][runtime][bootstrap]") {
    query_front_fixture fixture;
    const auto* const first = fixture.query();
    REQUIRE(first == runtime_api_v1());
    const auto pins = fixture.pin_calls.load();
    const auto owners = fixture.owner_calls.load();
    const auto stable_calls = fixture.stable_api_calls.load();

    fixture.pin_succeeds.store(false, std::memory_order_relaxed);
    CHECK(fixture.query() == first);
    CHECK(fixture.pin_calls.load() == pins);
    CHECK(fixture.owner_calls.load() == owners);
    CHECK(fixture.stable_api_calls.load() == stable_calls + 1U);
}

TEST_CASE("runtime.bootstrap.stable_api_callback_reentry_fails_closed",
    "[component][runtime][bootstrap]") {
    query_front_fixture fixture;
    const auto* const api = fixture.query();
    REQUIRE(api == runtime_api_v1());

    fixture.reenter_stable_api.store(true, std::memory_order_release);
    CHECK(fixture.query() == api);
    CHECK(fixture.stable_api_reentry_returned_null.load(std::memory_order_acquire));
    CHECK(fixture.stable_api_calls.load() == 2U);
}

TEST_CASE("runtime.bootstrap.concurrent_front_creation_is_single_and_reentry_fails_closed",
    "[component][runtime][bootstrap][concurrency]") {
    query_front_fixture fixture;
    fixture.block_creation = true;
    const input_next_runtime_api_v1* first_result{};
    const input_next_runtime_api_v1* second_result{};
    std::jthread first;
    std::jthread second;
    const release_creation_on_exit release{fixture};

    first = std::jthread([&] { first_result = fixture.query(); });

    const bool owner_callback_entered = fixture.wait_for_creation(std::chrono::seconds{2});
    second = std::jthread([&] { second_result = fixture.query(); });
    fixture.release_creation();
    first.join();
    second.join();

    CHECK(owner_callback_entered);
    CHECK(first_result == runtime_api_v1());
    CHECK(second_result == runtime_api_v1());
    CHECK(fixture.reentered.load(std::memory_order_acquire));
    CHECK(fixture.owner_calls.load() == 1U);
    CHECK(fixture.stable_api_calls.load() == 2U);
    CHECK(fixture.pin_calls.load() == 1U);
}
