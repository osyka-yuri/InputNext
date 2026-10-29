#include <inputnext/latest_value.hpp>

#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

namespace {

using namespace inputnext;

input_update valid_update() {
    return input_update{
        .state =
            gamepad_state{
                .buttons = 0x4123U,
                .left_stick = {.x = 0.25F, .y = -0.5F},
                .right_stick = {.x = 0.75F, .y = -1.0F},
                .left_trigger = 0.4F,
                .right_trigger = 0.9F,
            },
        .source_timestamp_us = 123456U,
        .observation_qpc = 987654U,
    };
}

input_update correlated_update(std::uint64_t id) {
    const auto phase = static_cast<float>(id % 1000U) / 1000.0F;
    return input_update{
        .state =
            gamepad_state{
                .buttons = static_cast<button_mask>(id),
                .left_stick = {.x = phase, .y = -phase},
                .right_stick = {.x = 1.0F - phase, .y = phase - 1.0F},
                .left_trigger = phase,
                .right_trigger = 1.0F - phase,
            },
        .source_timestamp_us = id * 13U,
        .observation_qpc = id * 29U,
    };
}

bool is_coherent(const published_sample& sample) {
    const auto id = sample.revision;
    const auto phase = static_cast<float>(id % 1000U) / 1000.0F;
    return id != 0U && sample.state.buttons == id && sample.state.left_stick.x == phase &&
           sample.state.left_stick.y == -phase && sample.state.right_stick.x == 1.0F - phase &&
           sample.state.right_stick.y == phase - 1.0F && sample.state.left_trigger == phase &&
           sample.state.right_trigger == 1.0F - phase && sample.source_timestamp_us == id * 13U &&
           sample.observation_qpc == id * 29U;
}

class worker_start_gate final {
public:
    explicit worker_start_gate(std::atomic<std::uint8_t>& state) noexcept : state_(state) {}

    ~worker_start_gate() { abort(); }

    worker_start_gate(const worker_start_gate&) = delete;
    worker_start_gate& operator=(const worker_start_gate&) = delete;
    worker_start_gate(worker_start_gate&&) = delete;
    worker_start_gate& operator=(worker_start_gate&&) = delete;

    void start() noexcept { release(1U); }

    void abort() noexcept { release(2U); }

private:
    void release(std::uint8_t state) noexcept {
        if (pending_) {
            state_.store(state, std::memory_order_release);
            state_.notify_all();
            pending_ = false;
        }
    }

    std::atomic<std::uint8_t>& state_;
    bool pending_{true};
};

bool wait_for_start(std::atomic<std::uint8_t> const& state) noexcept {
    auto observed = state.load(std::memory_order_acquire);
    while (observed == 0U) {
        state.wait(0U, std::memory_order_acquire);
        observed = state.load(std::memory_order_acquire);
    }
    return observed == 1U;
}

} // namespace

using namespace inputnext;

TEST_CASE("input.latest.initial_no_data", "[component][input]") {
    const latest_value latest;

    const auto read = latest.read();

    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == error::no_data);
}

TEST_CASE("input.latest.valid_complete_sample", "[component][input]") {
    latest_value latest;
    const auto update = valid_update();

    const auto published = latest.publish(update);
    REQUIRE(published.has_value());
    CHECK(published.value() == 1U);

    const auto read = latest.read();
    REQUIRE(read.has_value());
    CHECK(read->state == update.state);
    CHECK(read->source_timestamp_us == update.source_timestamp_us);
    CHECK(read->observation_qpc == update.observation_qpc);
    CHECK(read->revision == published.value());
}

TEST_CASE("input.latest.invalid_float_publication_preserves_current", "[component][input]") {
    const auto field = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
    const auto value = GENERATE(-1.01F,
        1.01F,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity());
    latest_value latest;
    const auto valid = valid_update();
    REQUIRE(latest.publish(valid).has_value());

    auto invalid = valid;
    const std::array<float*, 6U> float_fields{
        &invalid.state.left_stick.x,
        &invalid.state.left_stick.y,
        &invalid.state.right_stick.x,
        &invalid.state.right_stick.y,
        &invalid.state.left_trigger,
        &invalid.state.right_trigger,
    };
    *float_fields[field] = value;
    const auto rejected = latest.publish(invalid);
    CAPTURE(field, value);

    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::invalid_argument);

    const auto current = latest.read();
    REQUIRE(current.has_value());
    CHECK(current->state == valid.state);
    CHECK(current->source_timestamp_us == valid.source_timestamp_us);
    CHECK(current->observation_qpc == valid.observation_qpc);
    CHECK(current->revision == 1U);
}

TEST_CASE("input.latest.invalid_button_publication_preserves_current", "[component][input]") {
    const auto invalid_buttons = GENERATE(supported_button_mask + 1U, 1U << 31U);
    latest_value latest;
    const auto valid = valid_update();
    REQUIRE(latest.publish(valid).has_value());

    auto invalid = valid;
    invalid.state.buttons = invalid_buttons;
    const auto rejected = latest.publish(invalid);
    CAPTURE(invalid_buttons);

    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::invalid_argument);

    const auto current = latest.read();
    REQUIRE(current.has_value());
    CHECK(current->state == valid.state);
    CHECK(current->source_timestamp_us == valid.source_timestamp_us);
    CHECK(current->observation_qpc == valid.observation_qpc);
    CHECK(current->revision == 1U);
}

TEST_CASE("input.latest.zero_observation_qpc_preserves_current", "[component][input]") {
    latest_value latest;
    const auto valid = valid_update();
    REQUIRE(latest.publish(valid).has_value());

    auto invalid = valid;
    invalid.observation_qpc = 0U;
    const auto rejected = latest.publish(invalid);

    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::invalid_argument);

    const auto current = latest.read();
    REQUIRE(current.has_value());
    CHECK(current->state == valid.state);
    CHECK(current->source_timestamp_us == valid.source_timestamp_us);
    CHECK(current->observation_qpc == valid.observation_qpc);
    CHECK(current->revision == 1U);
}

TEST_CASE("input.latest.read_preserves_metadata", "[component][input]") {
    latest_value latest;
    const auto update = valid_update();
    REQUIRE(latest.publish(update).has_value());

    for (std::size_t read_index = 0; read_index < 4U; ++read_index) {
        const auto read = latest.read();
        REQUIRE(read.has_value());
        CHECK(read->state == update.state);
        CHECK(read->source_timestamp_us == update.source_timestamp_us);
        CHECK(read->observation_qpc == update.observation_qpc);
        CHECK(read->revision == 1U);
    }

    const auto repeated_publication = latest.publish(update);
    REQUIRE(repeated_publication.has_value());
    CHECK(repeated_publication.value() == 2U);
    const auto after_republication = latest.read();
    REQUIRE(after_republication.has_value());
    CHECK(after_republication->source_timestamp_us == update.source_timestamp_us);
    CHECK(after_republication->observation_qpc == update.observation_qpc);
    CHECK(after_republication->revision == 2U);
}

TEST_CASE("input.latest.equal_timestamps_remain_ordered", "[component][input]") {
    latest_value latest;
    auto first = valid_update();
    first.source_timestamp_us = 0U;
    auto second = first;
    second.state.buttons ^= static_cast<button_mask>(gamepad_button::guide);

    const auto first_revision = latest.publish(first);
    const auto second_revision = latest.publish(second);

    REQUIRE(first_revision.has_value());
    REQUIRE(second_revision.has_value());
    CHECK(first_revision.value() == 1U);
    CHECK(second_revision.value() == 2U);
    CHECK(first.source_timestamp_us == 0U);
    CHECK(first.source_timestamp_us == second.source_timestamp_us);
    CHECK(first.observation_qpc == second.observation_qpc);

    const auto current = latest.read();
    REQUIRE(current.has_value());
    CHECK(current->state == second.state);
    CHECK(current->source_timestamp_us == first.source_timestamp_us);
    CHECK(current->observation_qpc == first.observation_qpc);
    CHECK(current->revision == 2U);
}

TEST_CASE("input.latest.exhaustion_preserves_last_sample", "[component][input]") {
    constexpr auto maximum_revision = std::numeric_limits<std::uint64_t>::max();
    latest_value latest{maximum_revision - 1U};
    const auto final_update = valid_update();

    const auto final_revision = latest.publish(final_update);
    REQUIRE(final_revision.has_value());
    CHECK(final_revision.value() == maximum_revision);

    const auto rejected = latest.publish(correlated_update(3U));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == error::revision_exhausted);

    const auto current = latest.read();
    REQUIRE(current.has_value());
    CHECK(current->state == final_update.state);
    CHECK(current->source_timestamp_us == final_update.source_timestamp_us);
    CHECK(current->observation_qpc == final_update.observation_qpc);
    CHECK(current->revision == maximum_revision);
}

TEST_CASE("input.latest.closed_rejects_reads_and_publications", "[component][input]") {
    latest_value empty;
    CHECK_FALSE(empty.is_closed());
    empty.close();
    CHECK(empty.is_closed());
    empty.close();
    CHECK(empty.is_closed());

    const auto empty_read = empty.read();
    const auto empty_publish = empty.publish(valid_update());
    REQUIRE_FALSE(empty_read.has_value());
    CHECK(empty_read.error() == error::not_connected);
    REQUIRE_FALSE(empty_publish.has_value());
    CHECK(empty_publish.error() == error::not_connected);

    latest_value populated;
    REQUIRE(populated.publish(valid_update()).has_value());
    CHECK_FALSE(populated.is_closed());
    populated.close();
    CHECK(populated.is_closed());

    const auto closed_read = populated.read();
    const auto closed_publish = populated.publish(valid_update());
    REQUIRE_FALSE(closed_read.has_value());
    CHECK(closed_read.error() == error::not_connected);
    REQUIRE_FALSE(closed_publish.has_value());
    CHECK(closed_publish.error() == error::not_connected);
}

TEST_CASE("input.latest.coherent_concurrent_copy", "[component][input][concurrency]") {
    constexpr std::uint64_t last_id = 128U;
    latest_value latest;
    REQUIRE(latest.publish(correlated_update(1U)).has_value());

    std::atomic<bool> publisher_ok{true};
    std::atomic<bool> reader_ok{true};
    std::barrier phase_barrier{2U};
    std::atomic<std::uint8_t> start_state{0U};
    std::jthread publisher;
    std::jthread reader;
    worker_start_gate start_gate{start_state};

    publisher = std::jthread{[&] {
        if (!wait_for_start(start_state)) {
            return;
        }
        for (std::uint64_t id = 2U; id <= last_id; ++id) {
            phase_barrier.arrive_and_wait();
            try {
                const auto result = latest.publish(correlated_update(id));
                if (!result.has_value() || result.value() != id) {
                    publisher_ok.store(false, std::memory_order_relaxed);
                }
            } catch (...) {
                publisher_ok.store(false, std::memory_order_relaxed);
            }
            phase_barrier.arrive_and_wait();
        }
    }};
    reader = std::jthread{[&] {
        if (!wait_for_start(start_state)) {
            return;
        }
        for (std::uint64_t phase = 2U; phase <= last_id; ++phase) {
            phase_barrier.arrive_and_wait();
            try {
                const auto result = latest.read();
                if (!result.has_value() || !is_coherent(result.value())) {
                    reader_ok.store(false, std::memory_order_relaxed);
                }
            } catch (...) {
                reader_ok.store(false, std::memory_order_relaxed);
            }
            phase_barrier.arrive_and_wait();
        }
    }};
    start_gate.start();

    publisher.join();
    reader.join();

    CHECK(publisher_ok.load(std::memory_order_relaxed));
    CHECK(reader_ok.load(std::memory_order_relaxed));

    const auto final_sample = latest.read();
    REQUIRE(final_sample.has_value());
    CHECK(final_sample->revision == last_id);
    CHECK(is_coherent(final_sample.value()));
}
