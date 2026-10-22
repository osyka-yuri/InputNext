#include "output/controlled_native_sink.hpp"

#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace {

using inputnext::output::native_result;
using inputnext::output::native_status;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;

class release_write_guard final {
public:
    explicit release_write_guard(controlled_native_state& state) noexcept : state_(state) {}

    ~release_write_guard() noexcept { state_.release_write(); }

    release_write_guard(const release_write_guard&) = delete;
    release_write_guard& operator=(const release_write_guard&) = delete;
    release_write_guard(release_write_guard&&) = delete;
    release_write_guard& operator=(release_write_guard&&) = delete;

private:
    controlled_native_state& state_;
};

TEST_CASE("output.test_sink_release_before_write_is_durable", "[unit][output][test_support]") {
    const auto state = std::make_shared<controlled_native_state>();
    controlled_native_sink sink{state};
    state->block_next_write();
    state->release_write();
    state->release_write();

    const auto result = sink.write({});

    CHECK(result.status == native_status::accepted);
    CHECK(state->call_count() == 1U);
}

TEST_CASE("output.test_sink_rearming_requires_a_new_release", "[unit][output][test_support]") {
    const auto state = std::make_shared<controlled_native_state>();
    controlled_native_sink sink{state};
    state->block_next_write();
    state->release_write();
    state->block_next_write();

    native_result result{};
    std::atomic_bool completed{};
    std::jthread worker{[&] {
        result = sink.write({});
        completed.store(true);
    }};
    const release_write_guard cleanup{*state};
    const bool blocked = state->wait_for_blocked_write();
    const bool completed_before_release = completed.load();
    state->release_write();
    worker.join();

    CHECK(blocked);
    CHECK(!completed_before_release);
    CHECK(result.status == native_status::accepted);
    CHECK(state->call_count() == 1U);
}

} // namespace
