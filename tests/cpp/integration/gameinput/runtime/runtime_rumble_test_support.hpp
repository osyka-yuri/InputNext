#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <semaphore>

namespace inputnext::gameinput_test_support {

using gameinput_detail::native_failure;
using gameinput_detail::native_failure_cause;
using gameinput_detail::native_result;
using gameinput_detail::native_rumble_endpoint;

class rumble_test_controls final {
public:
    void block_next_factory() noexcept {
        block_factory_.store(gate_state::armed, std::memory_order_release);
    }

    void fail_next_factory() noexcept { fail_factory_.store(true, std::memory_order_release); }

    void block_next_write() noexcept {
        block_write_.store(gate_state::armed, std::memory_order_release);
    }

    void block_next_device_release() noexcept {
        block_device_release_.store(gate_state::armed, std::memory_order_release);
    }

    void block_next_endpoint_release() noexcept {
        std::scoped_lock lock{endpoint_release_gate_};
        ++endpoint_releases_armed_;
    }

    void block_native_cleanup() noexcept {
        block_native_cleanup_.store(gate_state::armed, std::memory_order_release);
    }

    [[nodiscard]] bool wait_for_factory() { return factory_entered_.try_acquire_for(timeout); }

    [[nodiscard]] bool wait_for_write() { return write_entered_.try_acquire_for(timeout); }

    [[nodiscard]] bool wait_for_device_release() {
        return device_release_entered_.try_acquire_for(timeout);
    }

    [[nodiscard]] bool wait_for_endpoint_release() {
        std::unique_lock lock{endpoint_release_gate_};
        const auto expected = endpoint_release_waited_ + 1U;
        if (!endpoint_release_changed_.wait_for(lock, timeout, [this, expected] {
                return endpoint_release_entered_ >= expected;
            })) {
            return false;
        }
        ++endpoint_release_waited_;
        return true;
    }

    [[nodiscard]] bool wait_for_endpoint_release_count(std::size_t expected) {
        std::unique_lock lock{endpoint_release_gate_};
        return endpoint_release_changed_.wait_for(lock, timeout, [this, expected] {
            return endpoint_release_entered_ >= expected;
        });
    }

    [[nodiscard]] bool wait_for_native_cleanup() {
        return native_cleanup_entered_.try_acquire_for(timeout);
    }

    void release_factory() noexcept { release_gate(block_factory_); }

    void release_write() noexcept { release_gate(block_write_); }

    void release_device_release() noexcept { release_gate(block_device_release_); }

    void release_endpoint_release() noexcept { release_endpoint_releases(1U); }

    void release_endpoint_releases(std::size_t count) noexcept {
        std::scoped_lock lock{endpoint_release_gate_};
        endpoint_release_permits_ += count;
        endpoint_release_changed_.notify_all();
    }

    void release_native_cleanup() noexcept { release_gate(block_native_cleanup_); }

    void release_all() noexcept {
        release_factory();
        release_write();
        release_device_release();
        release_native_cleanup();
        {
            std::scoped_lock lock{endpoint_release_gate_};
            endpoint_releases_armed_ = 0U;
            endpoint_release_permits_ = endpoint_release_waiting_;
            endpoint_release_changed_.notify_all();
        }
    }

    [[nodiscard]] std::size_t factory_calls() const noexcept {
        return factory_calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t writes() const noexcept {
        return writes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t device_releases() const noexcept {
        return device_releases_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t endpoint_releases() const noexcept {
        return endpoint_releases_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool native_cleanup_complete() const noexcept {
        return native_cleanup_complete_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<output::rumble_value> last_value() const {
        std::scoped_lock lock{value_mutex_};
        return last_value_;
    }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>> create_endpoint() {
        factory_calls_.fetch_add(1U, std::memory_order_relaxed);
        wait_at_gate(block_factory_, factory_entered_);
        if (fail_factory_.exchange(false, std::memory_order_acq_rel)) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::resource_exhausted});
        }
        try {
            return std::unique_ptr<native_rumble_endpoint>{std::make_unique<endpoint>(*this)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::resource_exhausted});
        }
    }

    void native_adapter_destroyed() noexcept {
        wait_at_gate(block_native_cleanup_, native_cleanup_entered_);
        native_cleanup_complete_.store(true, std::memory_order_release);
    }

    void native_device_released() noexcept {
        wait_at_gate(block_device_release_, device_release_entered_);
        device_releases_.fetch_add(1U, std::memory_order_relaxed);
    }

private:
    enum class gate_state : std::uint8_t { idle, armed, blocked, released };

    void wait_at_gate(std::atomic<gate_state>& gate, std::binary_semaphore& entered) noexcept {
        auto expected = gate_state::armed;
        if (gate.compare_exchange_strong(expected,
                gate_state::blocked,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            entered.release();
            std::unique_lock lock{gate_mutex_};
            gate_released_.wait(lock,
                [&gate] { return gate.load(std::memory_order_acquire) == gate_state::released; });
            gate.store(gate_state::idle, std::memory_order_release);
        } else if (expected == gate_state::released) {
            gate.store(gate_state::idle, std::memory_order_release);
        }
    }

    void release_gate(std::atomic<gate_state>& gate) noexcept {
        gate.store(gate_state::released, std::memory_order_release);
        gate_released_.notify_all();
    }

    class endpoint final : public native_rumble_endpoint {
    public:
        explicit endpoint(rumble_test_controls& controls) noexcept : controls_(controls) {}

        ~endpoint() noexcept override { controls_.release_endpoint_reference(); }

        endpoint(const endpoint&) = delete;
        endpoint& operator=(const endpoint&) = delete;
        endpoint(endpoint&&) = delete;
        endpoint& operator=(endpoint&&) = delete;

        [[nodiscard]] output::native_result set(output::rumble_value value) noexcept override {
            controls_.wait_at_gate(controls_.block_write_, controls_.write_entered_);
            {
                std::scoped_lock lock{controls_.value_mutex_};
                controls_.last_value_ = value;
            }
            controls_.writes_.fetch_add(1U, std::memory_order_relaxed);
            return output::native_result{.status = output::native_status::accepted};
        }

    private:
        rumble_test_controls& controls_;
    };

    void release_endpoint_reference() noexcept {
        {
            std::unique_lock lock{endpoint_release_gate_};
            if (endpoint_releases_armed_ > 0U) {
                --endpoint_releases_armed_;
                ++endpoint_release_waiting_;
                ++endpoint_release_entered_;
                endpoint_release_changed_.notify_all();
                endpoint_release_changed_.wait(lock,
                    [this] { return endpoint_release_permits_ > 0U; });
                --endpoint_release_permits_;
                --endpoint_release_waiting_;
            }
        }
        endpoint_releases_.fetch_add(1U, std::memory_order_relaxed);
        endpoint_release_changed_.notify_all();
    }

    static constexpr auto timeout = std::chrono::seconds{2};
    std::atomic<gate_state> block_factory_{gate_state::idle};
    std::atomic<bool> fail_factory_;
    std::atomic<gate_state> block_write_{gate_state::idle};
    std::atomic<gate_state> block_device_release_{gate_state::idle};
    std::atomic<gate_state> block_native_cleanup_{gate_state::idle};
    std::atomic<std::size_t> factory_calls_;
    std::atomic<std::size_t> writes_;
    std::atomic<std::size_t> device_releases_;
    std::atomic<std::size_t> endpoint_releases_;
    std::atomic<bool> native_cleanup_complete_;
    mutable std::mutex value_mutex_;
    std::mutex gate_mutex_;
    std::condition_variable gate_released_;
    std::optional<output::rumble_value> last_value_{std::nullopt};
    std::binary_semaphore factory_entered_{0};
    std::binary_semaphore write_entered_{0};
    std::binary_semaphore device_release_entered_{0};
    std::binary_semaphore native_cleanup_entered_{0};
    std::mutex endpoint_release_gate_;
    std::condition_variable endpoint_release_changed_;
    std::size_t endpoint_releases_armed_{};
    std::size_t endpoint_release_waiting_{};
    std::size_t endpoint_release_permits_{};
    std::size_t endpoint_release_entered_{};
    std::size_t endpoint_release_waited_{};
};

class scoped_rumble_release final {
public:
    explicit scoped_rumble_release(rumble_test_controls& controls) noexcept : controls_(controls) {}

    ~scoped_rumble_release() { controls_.release_all(); }

    scoped_rumble_release(const scoped_rumble_release&) = delete;
    scoped_rumble_release& operator=(const scoped_rumble_release&) = delete;
    scoped_rumble_release(scoped_rumble_release&&) = delete;
    scoped_rumble_release& operator=(scoped_rumble_release&&) = delete;

private:
    rumble_test_controls& controls_;
};

} // namespace inputnext::gameinput_test_support
