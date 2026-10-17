#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/source_lifetime.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace inputnext::test::devices {

struct destruction_observer final {
    destruction_observer() = default;
    ~destruction_observer() = default;

    destruction_observer(const destruction_observer&) = delete;
    destruction_observer& operator=(const destruction_observer&) = delete;
    destruction_observer(destruction_observer&&) = delete;
    destruction_observer& operator=(destruction_observer&&) = delete;

    void record() noexcept {
        std::scoped_lock lock{mutex};
        ++calls;
        last_thread = std::this_thread::get_id();
    }

    [[nodiscard]] std::size_t call_count() const {
        std::scoped_lock lock{mutex};
        return calls;
    }

    [[nodiscard]] std::thread::id thread_id() const {
        std::scoped_lock lock{mutex};
        return last_thread;
    }

    mutable std::mutex mutex;
    std::size_t calls{};
    std::thread::id last_thread{};
};

class test_source final : public source_lifetime {
public:
    explicit test_source(destruction_observer* observer = nullptr) noexcept : observer_(observer) {}

    ~test_source() noexcept override {
        if (observer_ != nullptr) {
            observer_->record();
        }
    }

    test_source(const test_source&) = delete;
    test_source& operator=(const test_source&) = delete;
    test_source(test_source&&) = delete;
    test_source& operator=(test_source&&) = delete;

private:
    destruction_observer* observer_{};
};

[[nodiscard]] inline runtime_id make_runtime(std::byte first_byte = std::byte{1}) noexcept {
    runtime_id value{};
    value.bytes[0] = first_byte;
    return value;
}

[[nodiscard]] inline input_update make_update(std::uint64_t id) noexcept {
    const auto phase = static_cast<float>(id % 1000U) / 1000.0F;
    return input_update{
        .state =
            gamepad_state{
                .buttons = static_cast<button_mask>(id & supported_button_mask),
                .left_stick = {.x = phase, .y = -phase},
                .right_stick = {.x = 1.0F - phase, .y = phase - 1.0F},
                .left_trigger = phase,
                .right_trigger = 1.0F - phase,
            },
        .source_timestamp_us = id * 13U,
        .observation_qpc = (id * 29U) + 1U,
    };
}

[[nodiscard]] inline bool coherent(const connection_snapshot& snapshot) noexcept {
    const auto id = snapshot.sample.revision;
    const auto expected = make_update(id);
    return id != 0U && snapshot.sample.state == expected.state &&
           snapshot.sample.source_timestamp_us == expected.source_timestamp_us &&
           snapshot.sample.observation_qpc == expected.observation_qpc;
}

[[nodiscard]] inline std::unique_ptr<source_lifetime> make_source(
    destruction_observer* observer = nullptr) {
    return std::make_unique<test_source>(observer);
}

} // namespace inputnext::test::devices
