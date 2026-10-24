#include "gameinput/output/usb/qualification.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

using clock = usb_output_qualification::clock;
using namespace std::chrono_literals;

[[nodiscard]] connection_target make_target(const std::uint8_t suffix) noexcept {
    connection_target target{};
    target.runtime.bytes[0U] = std::byte{0x41};
    target.runtime.bytes[1U] = static_cast<std::byte>(suffix);
    target.device.value = 2U;
    target.connection.value = 3U;
    return target;
}

class counting_sink final : public output::native_sink {
public:
    [[nodiscard]] output::native_result write(
        [[maybe_unused]] const output::native_command& command) noexcept override {
        writes.fetch_add(1U, std::memory_order_relaxed);
        return output::native_result{.status = output::native_status::accepted};
    }

    std::atomic<std::size_t> writes{0U};
};

TEST_CASE("gameinput.output.usb_qualification_expires_and_renews_from_query_start",
    "[gameinput][output][qualification][unit]") {
    const auto target = make_target(1U);
    const auto started = clock::time_point{100s};
    usb_output_qualification qualification{target, 7U, started};

    CHECK(qualification.admits(target, started));
    CHECK_FALSE(qualification.admits(make_target(2U), started));
    CHECK_FALSE(qualification.admits(target, started + 256ms));
    CHECK_FALSE(qualification.renew({.started = started + 64ms, .completed = started + 256ms}));

    usb_output_qualification renewed{target, 7U, started};
    CHECK(renewed.renew({.started = started + 64ms, .completed = started + 80ms}));
    CHECK(renewed.admits(target, started + 319ms));
    CHECK_FALSE(renewed.admits(target, started + 320ms));
}

TEST_CASE("gameinput.output.usb_qualification_revocation_is_irreversible",
    "[gameinput][output][qualification][unit]") {
    const auto target = make_target(1U);
    const auto started = clock::time_point{100s};
    usb_output_qualification qualification{target, 9U, started};
    qualification.revoke();

    CHECK_FALSE(qualification.admits(target, started));
    CHECK_FALSE(qualification.renew({.started = started + 64ms, .completed = started + 80ms}));
    CHECK_FALSE(qualification.admits(target, started + 81ms));
}

TEST_CASE("gameinput.output.usb_qualification_expiry_observation_blocks_stale_completion",
    "[gameinput][output][qualification][unit][concurrency]") {
    const auto target = make_target(1U);
    const auto started = clock::time_point{100s};
    usb_output_qualification qualification{target, 10U, started};
    std::barrier turn{2};
    bool renewed{};

    std::thread renewal{[&] {
        turn.arrive_and_wait();
        turn.arrive_and_wait();
        renewed = qualification.renew({.started = started + 64ms, .completed = started + 255ms});
    }};
    turn.arrive_and_wait();
    const auto expired = qualification.admits(target, started + 256ms);
    turn.arrive_and_wait();
    renewal.join();

    CHECK_FALSE(expired);
    CHECK_FALSE(renewed);
    CHECK_FALSE(qualification.admits(target, started + 257ms));
}

TEST_CASE("gameinput.output.usb_qualification_revocation_wins_against_concurrent_renewal",
    "[gameinput][output][qualification][unit][concurrency]") {
    const auto target = make_target(1U);
    const auto started = clock::time_point{100s};
    usb_output_qualification qualification{target, 11U, started};
    std::thread renewal{[&] {
        static_cast<void>(
            qualification.renew({.started = started + 64ms, .completed = started + 80ms}));
    }};
    qualification.revoke();
    renewal.join();

    CHECK_FALSE(qualification.admits(target, started + 81ms));
}

TEST_CASE("gameinput.output.usb_qualification_sink_suppresses_invalid_native_writes",
    "[gameinput][output][qualification][unit]") {
    const auto target = make_target(1U);
    const auto started = clock::now() + 1s;
    auto sink = std::make_unique<counting_sink>();
    auto const* const observation = sink.get();
    const auto permit = std::make_shared<usb_output_qualification>(target, 13U, started);
    auto qualified = make_qualified_usb_sink(std::move(sink), permit);
    REQUIRE(qualified.has_value());

    output::native_command command{};
    command.backend.target = target;
    CHECK(qualified.value()->write(command).status == output::native_status::accepted);
    CHECK(observation->writes.load(std::memory_order_relaxed) == 1U);

    command.backend.target = make_target(2U);
    CHECK(qualified.value()->write(command).status == output::native_status::retryable_failure);
    CHECK(observation->writes.load(std::memory_order_relaxed) == 1U);
    command.backend.target = target;

    permit->revoke();
    CHECK(qualified.value()->write(command).status == output::native_status::retryable_failure);
    CHECK(observation->writes.load(std::memory_order_relaxed) == 1U);

    auto expired_sink = std::make_unique<counting_sink>();
    auto const* const expired_observation = expired_sink.get();
    auto expired_permit = std::make_shared<usb_output_qualification>(target,
        14U,
        clock::now() - usb_output_qualification::validity - 1ms);
    auto expired = make_qualified_usb_sink(std::move(expired_sink), std::move(expired_permit));
    REQUIRE(expired.has_value());
    command.backend.target = target;
    CHECK(expired.value()->write(command).status == output::native_status::retryable_failure);
    CHECK(expired_observation->writes.load(std::memory_order_relaxed) == 0U);
}

} // namespace
} // namespace inputnext::gameinput_detail
