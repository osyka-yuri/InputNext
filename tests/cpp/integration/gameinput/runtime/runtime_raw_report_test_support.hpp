#pragma once

#include "gameinput/native/native_raw_output_report.hpp"
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <array>
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
#include <span>
#include <utility>
#include <vector>

namespace inputnext::gameinput_test_support {

using inputnext::gameinput_detail::native_raw_output_report;
using inputnext::gameinput_detail::raw_report_result;

class raw_report_test_gate final {
public:
    void arm() {
        std::scoped_lock lock{mutex_};
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    [[nodiscard]] bool wait_for_entry() {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void arrive() {
        std::unique_lock lock{mutex_};
        if (!armed_) {
            return;
        }
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        armed_ = false;
    }

    void release() noexcept {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    static constexpr auto timeout = std::chrono::seconds{2};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool armed_{};
    bool entered_{};
    bool released_{};
};

class raw_report_test_controls final {
public:
    void set_capacity(const std::size_t capacity) {
        std::scoped_lock lock{mutex_};
        capacity_ = capacity;
    }

    void fail_next_factory(const output::native_result result = output::native_result{
                               .status = output::native_status::retryable_failure}) {
        std::scoped_lock lock{mutex_};
        next_factory_failure_ = result;
    }

    void return_null_next_factory() noexcept {
        std::scoped_lock lock{mutex_};
        null_next_factory_ = true;
    }

    void fail_next_fill() noexcept {
        std::scoped_lock lock{mutex_};
        fail_next_fill_ = true;
    }

    void send_next_with(const output::native_result result) {
        std::scoped_lock lock{mutex_};
        next_send_result_ = result;
    }

    void block_next_factory() { factory_gate_.arm(); }

    void block_next_fill() { fill_gate_.arm(); }

    void block_next_send() { send_gate_.arm(); }

    void block_next_report_release() { report_release_gate_.arm(); }

    [[nodiscard]] bool wait_for_factory() { return factory_gate_.wait_for_entry(); }

    [[nodiscard]] bool wait_for_fill() { return fill_gate_.wait_for_entry(); }

    [[nodiscard]] bool wait_for_send() { return send_gate_.wait_for_entry(); }

    [[nodiscard]] bool wait_for_report_release() { return report_release_gate_.wait_for_entry(); }

    void release_factory() noexcept { factory_gate_.release(); }

    void release_fill() noexcept { fill_gate_.release(); }

    void release_send() noexcept { send_gate_.release(); }

    void release_report_release() noexcept { report_release_gate_.release(); }

    void release_all() noexcept {
        release_factory();
        release_fill();
        release_send();
        release_report_release();
    }

    [[nodiscard]] std::size_t factory_calls() const noexcept {
        return factory_calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t fill_calls() const noexcept {
        return fill_calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t send_calls() const noexcept {
        return send_calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t report_creations() const noexcept {
        return report_creations_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t report_releases() const noexcept {
        return report_releases_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t report_release_order() const noexcept {
        return report_release_order_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t device_release_order() const noexcept {
        return device_release_order_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t device_releases() const noexcept {
        return device_releases_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<std::uint32_t> requested_report_ids() const {
        std::scoped_lock lock{mutex_};
        return requested_report_ids_;
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> fill_payloads() const {
        std::scoped_lock lock{mutex_};
        return fill_payloads_;
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> sent_payloads() const {
        std::scoped_lock lock{mutex_};
        return sent_payloads_;
    }

    [[nodiscard]] raw_report_result create_report(const std::uint32_t report_id) {
        factory_calls_.fetch_add(1U, std::memory_order_relaxed);
        factory_gate_.arrive();

        std::size_t capacity{};
        std::optional<output::native_result> failure;
        bool return_null{};
        {
            std::scoped_lock lock{mutex_};
            requested_report_ids_.push_back(report_id);
            capacity = capacity_;
            failure = std::exchange(next_factory_failure_, std::nullopt);
            return_null = std::exchange(null_next_factory_, false);
        }
        if (failure.has_value()) {
            return std::unexpected(*failure);
        }
        if (return_null) {
            return std::unique_ptr<native_raw_output_report>{};
        }

        try {
            auto report = std::make_unique<raw_report_test_report>(*this, capacity);
            report_creations_.fetch_add(1U, std::memory_order_relaxed);
            return std::unique_ptr<native_raw_output_report>{std::move(report)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                output::native_result{.status = output::native_status::retryable_failure});
        }
    }

    [[nodiscard]] bool fill(const std::span<const std::byte> data) noexcept {
        fill_calls_.fetch_add(1U, std::memory_order_relaxed);
        fill_gate_.arrive();
        bool fail{};
        try {
            std::scoped_lock lock{mutex_};
            fill_payloads_.emplace_back(data.begin(), data.end());
            fail = std::exchange(fail_next_fill_, false);
        } catch (...) {
            return false;
        }
        return !fail;
    }

    [[nodiscard]] output::native_result send(const std::span<const std::byte> data) noexcept {
        send_calls_.fetch_add(1U, std::memory_order_relaxed);
        send_gate_.arrive();
        std::optional<output::native_result> result;
        try {
            std::scoped_lock lock{mutex_};
            sent_payloads_.emplace_back(data.begin(), data.end());
            result = std::exchange(next_send_result_, std::nullopt);
        } catch (...) {
            return output::native_result{.status = output::native_status::retryable_failure};
        }
        return result.value_or(output::native_result{.status = output::native_status::accepted});
    }

    void report_destroyed() noexcept {
        report_release_gate_.arrive();
        report_releases_.fetch_add(1U, std::memory_order_relaxed);
        report_release_order_.store(next_order(), std::memory_order_release);
    }

    void native_device_released() noexcept {
        device_releases_.fetch_add(1U, std::memory_order_relaxed);
        device_release_order_.store(next_order(), std::memory_order_release);
    }

    void native_adapter_destroyed() noexcept {}

private:
    class raw_report_test_report final : public native_raw_output_report {
    public:
        raw_report_test_report(raw_report_test_controls& controls,
            const std::size_t capacity) noexcept
            : controls_(controls),
              capacity_(capacity) {}

        ~raw_report_test_report() noexcept override { controls_.report_destroyed(); }

        raw_report_test_report(const raw_report_test_report&) = delete;
        raw_report_test_report& operator=(const raw_report_test_report&) = delete;
        raw_report_test_report(raw_report_test_report&&) = delete;
        raw_report_test_report& operator=(raw_report_test_report&&) = delete;

        [[nodiscard]] std::size_t capacity() const noexcept override { return capacity_; }

        [[nodiscard]] bool set_data(const std::span<const std::byte> data) noexcept override {
            if (data.size() != capacity_ || data.size() > data_.size() || !controls_.fill(data)) {
                return false;
            }
            std::ranges::copy(data, data_.begin());
            data_size_ = data.size();
            return true;
        }

        [[nodiscard]] output::native_result send() noexcept override {
            return controls_.send(std::span<const std::byte>{data_.data(), data_size_});
        }

    private:
        raw_report_test_controls& controls_;
        std::size_t capacity_{};
        std::array<std::byte, 64U> data_{};
        std::size_t data_size_{};
    };

    [[nodiscard]] std::size_t next_order() noexcept {
        return sequence_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    }

    mutable std::mutex mutex_;
    std::size_t capacity_{64U};
    std::optional<output::native_result> next_factory_failure_;
    std::optional<output::native_result> next_send_result_;
    bool null_next_factory_{};
    bool fail_next_fill_{};
    std::vector<std::uint32_t> requested_report_ids_;
    std::vector<std::vector<std::byte>> fill_payloads_;
    std::vector<std::vector<std::byte>> sent_payloads_;
    raw_report_test_gate factory_gate_;
    raw_report_test_gate fill_gate_;
    raw_report_test_gate send_gate_;
    raw_report_test_gate report_release_gate_;
    std::atomic<std::size_t> factory_calls_;
    std::atomic<std::size_t> fill_calls_;
    std::atomic<std::size_t> send_calls_;
    std::atomic<std::size_t> report_creations_;
    std::atomic<std::size_t> report_releases_;
    std::atomic<std::size_t> device_releases_;
    std::atomic<std::size_t> sequence_;
    std::atomic<std::size_t> report_release_order_;
    std::atomic<std::size_t> device_release_order_;
};

class scoped_raw_report_release final {
public:
    explicit scoped_raw_report_release(raw_report_test_controls& controls) noexcept
        : controls_(controls) {}

    ~scoped_raw_report_release() { controls_.release_all(); }

    scoped_raw_report_release(const scoped_raw_report_release&) = delete;
    scoped_raw_report_release& operator=(const scoped_raw_report_release&) = delete;
    scoped_raw_report_release(scoped_raw_report_release&&) = delete;
    scoped_raw_report_release& operator=(scoped_raw_report_release&&) = delete;

private:
    raw_report_test_controls& controls_;
};

} // namespace inputnext::gameinput_test_support
