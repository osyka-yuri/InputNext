#pragma once

#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "gameinput/reading_parts.hpp"
#include "input/touch_resolver.hpp"

#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>

namespace inputnext::gameinput_detail {

inline history_time scripted_now{};

[[nodiscard]] inline history_time read_scripted_now() noexcept {
    return scripted_now;
}

struct source_history_cursor_test_access final {
    static void use_scripted_clock(source_history_cursor& cursor) noexcept {
        cursor.clock_ = &read_scripted_now;
    }
};

[[nodiscard]] inline native_history_step
reading_step(button_mask buttons, std::uint64_t timestamp, std::uint64_t qpc) {
    return native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = gamepad_state{.buttons = buttons},
                .source_timestamp_us = timestamp,
                .observation_qpc = qpc,
            },
    };
}

[[nodiscard]] inline native_history_step caught_up_step(std::uint64_t qpc) {
    return native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = qpc,
    };
}

[[nodiscard]] inline native_history_step provider_failure_step(std::uint32_t native_code) {
    return native_history_step{
        .kind = native_history_step_kind::provider_failure,
        .native_code = native_code,
    };
}

struct scripted_history final {
    static constexpr std::size_t step_capacity = 8U;

    native_history_seed seed{
        .baseline =
            input_update{
                .state = gamepad_state{},
                .source_timestamp_us = 100U,
                .observation_qpc = 100U,
            },
        .qpc_frequency = 10'000'000U,
    };
    std::array<native_history_step, step_capacity> steps{};
    std::size_t step_count{};
    std::size_t start_count{};
    std::size_t next_count{};
    std::uint64_t quiet_checkpoint_qpc{100U};
    class test_gate* next_gate{};
};

class test_gate final {
public:
    void block() noexcept {
        std::unique_lock lock{mutex_};
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool wait_until_entered_for(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

class scripted_native_cursor final : public native_history_cursor {
public:
    explicit scripted_native_cursor(scripted_history& history) noexcept : history_(history) {}

    [[nodiscard]] native_result<native_history_seed> start() noexcept override {
        ++history_.start_count;
        step_offset_ = 0U;
        return history_.seed;
    }

    [[nodiscard]] native_history_step next() noexcept override {
        ++history_.next_count;
        if (history_.next_gate != nullptr) {
            history_.next_gate->block();
        }
        if (step_offset_ < history_.step_count) {
            return history_.steps[step_offset_++];
        }
        return caught_up_step(history_.quiet_checkpoint_qpc);
    }

private:
    scripted_history& history_;
    std::size_t step_offset_{};
};

class scripted_native final : public native_gameinput {
public:
    scripted_native() = default;

    scripted_history history{};
    std::size_t create_failures{};
    std::size_t create_count{};
    std::uint64_t frequency{10'000'000U};
    test_gate* frequency_gate{};
    bool allow_composite_history{};

    [[nodiscard]] native_result<void> initialize(
        const std::filesystem::path& runtime_library) override {
        static_cast<void>(runtime_library);
        return {};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) override {
        static_cast<void>(device);
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] bool unregister_callback(native_callback_token token) noexcept override {
        static_cast<void>(token);
        return true;
    }

    [[nodiscard]] native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) override {
        static_cast<void>(device);
        static_cast<void>(lane);
        return std::unexpected(native_failure{
            .cause = native_failure_cause::no_current_reading,
        });
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device) override {
        static_cast<void>(device);
        ++create_count;
        if (create_failures != 0U) {
            --create_failures;
            return std::unexpected(native_failure{
                .cause = native_failure_cause::resource_exhausted,
            });
        }
        return std::unique_ptr<native_history_cursor>{
            std::make_unique<scripted_native_cursor>(history)};
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device,
        const native_input_profile& profile,
        const input::touch_snapshot& touch_context) override {
        static_cast<void>(touch_context);
        if (profile.history == input_history_path::composite && allow_composite_history) {
            return create_history_cursor(device);
        }
        return native_gameinput::create_history_cursor(device, profile, touch_context);
    }

    [[nodiscard]] native_result<std::uint64_t> performance_frequency() noexcept override {
        if (frequency_gate != nullptr) {
            frequency_gate->block();
        }
        return frequency;
    }

    [[nodiscard]] native_result<native_device_facts> facts(native_device_handle device) override {
        static_cast<void>(device);
        return native_device_facts{};
    }

    [[nodiscard]] bool connected(native_device_handle device) noexcept override {
        static_cast<void>(device);
        return true;
    }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>> create_rumble_endpoint(
        native_device_handle device) override {
        static_cast<void>(device);
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    }

    void retain(native_device_handle device) noexcept override { static_cast<void>(device); }

    void release(native_device_handle device) noexcept override { static_cast<void>(device); }
};

} // namespace inputnext::gameinput_detail
