#include "gameinput/reading_parts.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include <gameinput/core/publication_source.hpp>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

constexpr button_mask guide_mask = static_cast<button_mask>(gamepad_button::guide);

[[nodiscard]] runtime_id make_runtime() {
    runtime_id value{};
    value.bytes[0U] = std::byte{0xA5};
    return value;
}

[[nodiscard]] gamepad_state make_pad(button_mask buttons, float axis = 0.25F) {
    return gamepad_state{
        .buttons = buttons,
        .left_stick = {.x = axis, .y = -axis},
        .right_stick = {.x = 1.0F - axis, .y = axis - 1.0F},
        .left_trigger = axis,
        .right_trigger = 1.0F - axis,
    };
}

[[nodiscard]] reading_parts
pad_read(gamepad_state pad, std::uint64_t timestamp = 100U, std::uint64_t qpc = 1000U) {
    return reading_parts{
        .pad = pad,
        .guide = std::nullopt,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
    };
}

class publication_fixture final {
public:
    explicit publication_fixture(publication_epoch_seed seed = {}) : registry(make_runtime()) {
        auto candidate = std::make_unique<publication_source>(seed);
        source = candidate.get();
        std::unique_ptr<source_lifetime> owner{std::move(candidate)};
        auto admitted = registry.admit(owner);
        if (!admitted.has_value()) {
            return;
        }
        connection = admitted.value();
        bind_result = source->bind(connection.producer);
    }

    [[nodiscard]] bool ready() const noexcept { return bind_result == bind_outcome::bound; }

    device_registry registry;
    publication_source* source{};
    admitted_connection connection{};
    bind_outcome bind_result{bind_outcome::closed};
};

class start_gate final {
public:
    void arrive_and_wait() noexcept {
        ready_.fetch_add(1U, std::memory_order_release);
        ready_.notify_all();
        auto state = state_.load(std::memory_order_acquire);
        while (state == 0U) {
            state_.wait(0U, std::memory_order_acquire);
            state = state_.load(std::memory_order_acquire);
        }
    }

    void wait_for_ready(std::size_t count) noexcept {
        auto ready = ready_.load(std::memory_order_acquire);
        while (ready < count) {
            ready_.wait(ready, std::memory_order_acquire);
            ready = ready_.load(std::memory_order_acquire);
        }
    }

    void release() noexcept {
        state_.store(1U, std::memory_order_release);
        state_.notify_all();
    }

private:
    std::atomic<std::size_t> ready_{0U};
    std::atomic<std::uint8_t> state_{0U};
};

class worker_cleanup final {
public:
    worker_cleanup(start_gate& gate, std::array<std::jthread, 2U>& workers) noexcept
        : gate_(gate),
          workers_(workers) {}

    ~worker_cleanup() {
        gate_.release();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    start_gate& gate_;
    std::array<std::jthread, 2U>& workers_;
};

} // namespace

TEST_CASE("gameinput.publication.guide_before_pad_and_unknownness",
    "[integration][gameinput][publication]") {
    const publication_fixture fixture;
    REQUIRE(fixture.ready());

    const reading_parts guide_first{
        .pad = std::nullopt,
        .guide = true,
        .source_timestamp_us = 20U,
        .observation_qpc = 200U,
    };
    CHECK(fixture.source->on_callback(guide_first) == publication_outcome::partial_only);
    const auto before_pad = fixture.connection.reader.read();
    CHECK((!before_pad.has_value() && before_pad.error() == error::no_data));
    CHECK(fixture.source->status().guide_known);
    CHECK(fixture.source->status().pad_known == false);

    const auto pad_with_no_guide =
        pad_read(make_pad(static_cast<button_mask>(gamepad_button::south) | guide_mask));
    CHECK(fixture.source->on_callback(pad_with_no_guide) == publication_outcome::published);
    auto current = fixture.connection.reader.read();
    REQUIRE(current.has_value());
    CHECK(current->sample.state ==
          make_pad(static_cast<button_mask>(gamepad_button::south) | guide_mask));
    CHECK(current->sample.source_timestamp_us == 100U);
    CHECK(current->sample.observation_qpc == 1000U);
    CHECK(current->sample.revision == 1U);

    const auto before_repeated_reads = current->sample;
    for (std::size_t index = 0U; index < 5U; ++index) {
        current = fixture.connection.reader.read();
        REQUIRE(current.has_value());
        CHECK(current->sample == before_repeated_reads);
    }
    CHECK(fixture.source->status().guide_known);
    CHECK(fixture.source->status().pad_known);

    const publication_fixture unknown_guide;
    REQUIRE(unknown_guide.ready());
    CHECK(unknown_guide.source->on_callback(pad_read(make_pad(
              static_cast<button_mask>(gamepad_button::east)))) == publication_outcome::published);
    const auto unknown = unknown_guide.connection.reader.read();
    REQUIRE(unknown.has_value());
    CHECK(unknown_guide.source->status().guide_known == false);
    CHECK((unknown->sample.state.buttons & guide_mask) == 0U);
}

TEST_CASE("gameinput.publication.invalid_pad_does_not_discard_valid_guide",
    "[integration][gameinput][publication]") {
    const publication_fixture fixture;
    REQUIRE(fixture.ready());
    const auto valid_pad = make_pad(static_cast<button_mask>(gamepad_button::west), 0.5F);
    CHECK(fixture.source->on_callback(pad_read(valid_pad, 90U, 900U)) ==
          publication_outcome::published);

    auto invalid_pad = make_pad(static_cast<button_mask>(gamepad_button::north), 0.75F);
    invalid_pad.left_stick.x = 2.0F;
    const reading_parts partial{
        .pad = invalid_pad,
        .guide = true,
        .source_timestamp_us = 40U,
        .observation_qpc = 400U,
    };
    CHECK(fixture.source->on_callback(partial) == publication_outcome::published);
    auto current = fixture.connection.reader.read();
    REQUIRE(current.has_value());
    auto expected = valid_pad;
    expected.buttons |= guide_mask;
    CHECK(current->sample.state == expected);
    CHECK(current->sample.source_timestamp_us == 40U);
    CHECK(current->sample.observation_qpc == 400U);
    CHECK(current->sample.revision == 2U);

    const auto invalid_only = fixture.source->on_callback(pad_read(invalid_pad, 10U, 500U));
    CHECK(invalid_only == publication_outcome::invalid);
    const auto zero_qpc = fixture.source->on_callback(reading_parts{
        .pad = std::nullopt,
        .guide = false,
        .source_timestamp_us = 0U,
        .observation_qpc = 0U,
    });
    CHECK(zero_qpc == publication_outcome::invalid);

    current = fixture.connection.reader.read();
    REQUIRE(current.has_value());
    CHECK(current->sample.state == expected);
    CHECK(current->sample.source_timestamp_us == 40U);
    CHECK(current->sample.observation_qpc == 400U);
    CHECK(current->sample.revision == 2U);
}

TEST_CASE("gameinput.publication.callback_wins_equal_timestamp_seed_race",
    "[integration][gameinput][publication][concurrency]") {
    publication_fixture fixture;
    REQUIRE(fixture.ready());
    const auto ticket = fixture.source->before_seed();
    const auto timestamp = 77U;
    const auto qpc = 770U;
    const auto seed_pad = make_pad(static_cast<button_mask>(gamepad_button::east), 0.2F);
    const auto callback_pad = make_pad(static_cast<button_mask>(gamepad_button::south), 0.8F);
    const auto seed = pad_read(seed_pad, timestamp, qpc);
    const auto callback = pad_read(callback_pad, timestamp, qpc);

    start_gate gate;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{gate, workers};
    auto seed_outcome = publication_outcome::closed;
    auto callback_outcome = publication_outcome::closed;
    bool seed_completed = false;
    bool callback_completed = false;
    bool seed_threw = false;
    bool callback_threw = false;

    workers[0U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            seed_outcome = fixture.source->on_seed(ticket, seed);
            seed_completed = true;
        } catch (...) {
            seed_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            callback_outcome = fixture.source->on_callback(callback);
            callback_completed = true;
        } catch (...) {
            callback_threw = true;
        }
    }};
    gate.wait_for_ready(workers.size());
    gate.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(seed_threw == false);
    CHECK(callback_threw == false);
    CHECK(seed_completed);
    CHECK(callback_completed);
    CHECK(((seed_outcome == publication_outcome::published) ||
           (seed_outcome == publication_outcome::stale_seed)));
    CHECK(callback_outcome == publication_outcome::published);

    const auto current = fixture.connection.reader.read();
    REQUIRE(current.has_value());
    CHECK(current->sample.state == callback_pad);
    CHECK(current->sample.source_timestamp_us == timestamp);
    CHECK(current->sample.observation_qpc == qpc);
    CHECK(((current->sample.revision == 1U) || (current->sample.revision == 2U)));
}

TEST_CASE("gameinput.publication.seed_uses_uncontended_guide_lane",
    "[integration][gameinput][publication][concurrency]") {
    publication_fixture fixture;
    REQUIRE(fixture.ready());
    const auto ticket = fixture.source->before_seed();
    const auto timestamp = 88U;
    const auto qpc = 880U;
    const reading_parts seed{
        .pad = std::nullopt,
        .guide = true,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
    };
    const auto callback_pad = make_pad(static_cast<button_mask>(gamepad_button::dpad_right), 0.6F);
    const auto callback = pad_read(callback_pad, timestamp, qpc);

    start_gate gate;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{gate, workers};
    auto seed_outcome = publication_outcome::closed;
    auto callback_outcome = publication_outcome::closed;
    bool seed_completed = false;
    bool callback_completed = false;
    bool seed_threw = false;
    bool callback_threw = false;
    workers[0U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            seed_outcome = fixture.source->on_seed(ticket, seed);
            seed_completed = true;
        } catch (...) {
            seed_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            callback_outcome = fixture.source->on_callback(callback);
            callback_completed = true;
        } catch (...) {
            callback_threw = true;
        }
    }};
    gate.wait_for_ready(workers.size());
    gate.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(seed_threw == false);
    CHECK(callback_threw == false);
    CHECK(seed_completed);
    CHECK(callback_completed);
    CHECK(((seed_outcome == publication_outcome::partial_only) ||
           (seed_outcome == publication_outcome::published)));
    CHECK(callback_outcome == publication_outcome::published);

    const auto current = fixture.connection.reader.read();
    REQUIRE(current.has_value());
    auto expected = callback_pad;
    expected.buttons |= guide_mask;
    CHECK(current->sample.state == expected);
    CHECK(current->sample.source_timestamp_us == timestamp);
    CHECK(current->sample.observation_qpc == qpc);
    CHECK(fixture.source->status().pad_epoch == 1U);
    CHECK(fixture.source->status().guide_epoch == 0U);
}

TEST_CASE("gameinput.publication.close_serializes_with_publish",
    "[integration][gameinput][publication][concurrency]") {
    publication_fixture fixture;
    REQUIRE(fixture.ready());
    const auto callback =
        pad_read(make_pad(static_cast<button_mask>(gamepad_button::menu)), 123U, 456U);

    start_gate gate;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{gate, workers};
    auto callback_outcome = publication_outcome::closed;
    bool callback_completed = false;
    bool callback_threw = false;
    bool close_threw = false;
    workers[0U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            callback_outcome = fixture.source->on_callback(callback);
            callback_completed = true;
        } catch (...) {
            callback_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        gate.arrive_and_wait();
        try {
            fixture.source->close();
        } catch (...) {
            close_threw = true;
        }
    }};
    gate.wait_for_ready(workers.size());
    gate.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(callback_threw == false);
    CHECK(close_threw == false);
    CHECK(callback_completed);
    CHECK(((callback_outcome == publication_outcome::published) ||
           (callback_outcome == publication_outcome::closed)));
    CHECK(fixture.source->status().closed);
    const auto current = fixture.connection.reader.read();
    if (callback_outcome == publication_outcome::published) {
        REQUIRE(current.has_value());
        CHECK(current->sample.revision == 1U);
    } else {
        CHECK((!current.has_value() && current.error() == error::no_data));
    }
}

TEST_CASE("gameinput.publication.stale_binding_cannot_publish_into_replacement",
    "[integration][gameinput][publication]") {
    publication_fixture fixture;
    REQUIRE(fixture.ready());
    const auto old_target = fixture.connection.target;
    const auto old_reader = fixture.connection.reader;

    auto replacement_candidate = std::make_unique<publication_source>();
    auto* const replacement_source = replacement_candidate.get();
    std::unique_ptr<source_lifetime> replacement_owner{std::move(replacement_candidate)};
    auto replacement = fixture.registry.replace(old_target, replacement_owner);
    REQUIRE(replacement.has_value());
    CHECK(replacement->target.runtime == old_target.runtime);
    CHECK(replacement->target.device == old_target.device);
    CHECK(replacement->target.connection != old_target.connection);
    CHECK(replacement_source->bind(replacement->producer) == bind_outcome::bound);

    CHECK(fixture.source->on_callback(pad_read(make_pad(
              static_cast<button_mask>(gamepad_button::view)))) == publication_outcome::closed);
    CHECK(fixture.source->status().closed);
    const auto old_read = old_reader.read();
    CHECK((!old_read.has_value() && old_read.error() == error::not_connected));
    const auto replacement_read = replacement->reader.read();
    CHECK((!replacement_read.has_value() && replacement_read.error() == error::no_data));

    const auto replacement_pad = make_pad(static_cast<button_mask>(gamepad_button::dpad_up));
    const auto replacement_update = pad_read(replacement_pad);
    CHECK(replacement_source->on_callback(replacement_update) == publication_outcome::published);
    const auto replacement_current = replacement->reader.read();
    REQUIRE(replacement_current.has_value());
    CHECK(replacement_current->sample.state == replacement_pad);
    CHECK(replacement_current->sample.revision == 1U);
}

TEST_CASE("gameinput.publication.callback_epoch_exhaustion_is_terminal_and_atomic",
    "[integration][gameinput][publication]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto exhausted_lane =
        GENERATE(publication_fault::pad_epoch_exhausted, publication_fault::guide_epoch_exhausted);
    const publication_fixture fixture{publication_epoch_seed{
        .pad_epoch = exhausted_lane == publication_fault::pad_epoch_exhausted ? maximum : 0U,
        .guide_epoch = exhausted_lane == publication_fault::guide_epoch_exhausted ? maximum : 0U,
    }};
    REQUIRE(fixture.ready());

    reading_parts reading{};
    if (exhausted_lane == publication_fault::pad_epoch_exhausted) {
        reading = pad_read(make_pad(static_cast<button_mask>(gamepad_button::west)));
    } else {
        reading.guide = true;
        reading.source_timestamp_us = 100U;
        reading.observation_qpc = 1000U;
    }
    CHECK(fixture.source->on_callback(reading) == publication_outcome::epoch_exhausted);
    CHECK(fixture.source->status().fault == exhausted_lane);
    CHECK(fixture.source->on_callback(reading) == publication_outcome::epoch_exhausted);

    const auto current = fixture.connection.reader.read();
    CHECK((!current.has_value() && current.error() == error::no_data));
    CHECK(fixture.source->status().pad_known == false);
    CHECK(fixture.source->status().guide_known == false);
    CHECK(fixture.source->status().pad_epoch ==
          (exhausted_lane == publication_fault::pad_epoch_exhausted ? maximum : 0U));
    CHECK(fixture.source->status().guide_epoch ==
          (exhausted_lane == publication_fault::guide_epoch_exhausted ? maximum : 0U));
}
