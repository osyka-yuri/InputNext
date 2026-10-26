#include "audio/control_notification.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <thread>

namespace inputnext::audio_detail {
namespace {

using namespace std::chrono_literals;

TEST_CASE("audio.control_notification.retains_and_coalesces_signals",
    "[unit][audio][notification]") {
    control_notification notification;
    notification.notify();
    CHECK(notification.wait_for(50ms));

    for (std::size_t index = 0U; index < 64U; ++index) {
        notification.notify();
    }
    CHECK(notification.wait_for(50ms));
    CHECK(!notification.wait_for(20ms));
}

TEST_CASE("audio.control_notification.repeated_producer_consumer_transitions",
    "[unit][audio][notification]") {
    control_notification notification;
    std::atomic<std::size_t> published{};
    std::barrier rendezvous{2};
    const std::jthread producer{[&] {
        for (std::size_t index = 1U; index <= 8U; ++index) {
            rendezvous.arrive_and_wait();
            published.store(index, std::memory_order_relaxed);
            notification.notify();
            rendezvous.arrive_and_wait();
        }
    }};
    for (std::size_t index = 1U; index <= 8U; ++index) {
        CHECK(!notification.wait_for(0ms));
        rendezvous.arrive_and_wait();
        CHECK(notification.wait_for(500ms));
        CHECK(published.load(std::memory_order_relaxed) == index);
        rendezvous.arrive_and_wait();
    }
}

} // namespace
} // namespace inputnext::audio_detail
