#pragma once

#include "metadata/provider.hpp"
#include <inputnext/device_metadata.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>
#include <utility>

namespace inputnext::gameinput_test_support {

class fake_metadata_provider final : public metadata_provider {
public:
    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const metadata_request& request) override {
        query_calls_.fetch_add(1U, std::memory_order_relaxed);
        if (busy_next_query_.exchange(false, std::memory_order_acq_rel)) {
            completed_queries_.fetch_add(1U, std::memory_order_relaxed);
            return std::unexpected(metadata_error::busy);
        }

        metadata_snapshot result{};
        std::optional<metadata_error> failure;
        {
            std::scoped_lock lock{metadata_mutex_};
            result.target = request.target;
            result.catalogue_generation = request.catalogue_generation;
            result.native = request.facts;
            result.container_id = request.facts.container_id;
            if (conflicting_identity) {
                result.container_id_source = container_id_provenance::conflicting;
            } else if (request.facts.container_id.has_value()) {
                result.container_id_source = container_id_provenance::gameinput_native;
            }
            result.model = model;
            result.transport = transport;
            if (thread_failure_remaining_ != 0U &&
                thread_failure_target_ == std::this_thread::get_id()) {
                --thread_failure_remaining_;
                ++thread_failure_observed_;
                failure = thread_failure_;
            } else {
                failure = std::exchange(next_failure_, std::nullopt);
            }
        }

        auto blocks = blocked_queries_.load(std::memory_order_acquire);
        while (blocks > 0U && !blocked_queries_.compare_exchange_weak(blocks,
                                  blocks - 1U,
                                  std::memory_order_acq_rel)) {
        }
        if (blocks > 0U) {
            blocked_.release();
            release_.acquire();
        }

        if (failure.has_value()) {
            completed_queries_.fetch_add(1U, std::memory_order_relaxed);
            return std::unexpected{failure.value()};
        }
        completed_queries_.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }

    // Use set_observation when queries can run concurrently.
    metadata_field<metadata_device_model> model{};
    metadata_field<transport_metadata> transport{};
    bool conflicting_identity{};

    [[nodiscard]] std::size_t query_calls() const noexcept {
        return query_calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t completed_queries() const noexcept {
        return completed_queries_.load(std::memory_order_relaxed);
    }

    void make_next_query_busy() noexcept {
        busy_next_query_.store(true, std::memory_order_release);
    }

    void set_observation(metadata_field<metadata_device_model> next_model,
        metadata_field<transport_metadata> next_transport,
        bool next_conflicting_identity) {
        std::scoped_lock lock{metadata_mutex_};
        model = next_model;
        transport = next_transport;
        conflicting_identity = next_conflicting_identity;
    }

    void fail_next_query(metadata_error failure) {
        std::scoped_lock lock{metadata_mutex_};
        next_failure_ = failure;
    }

    void fail_queries_on(std::thread::id thread, metadata_error failure, const std::size_t count) {
        std::scoped_lock lock{metadata_mutex_};
        thread_failure_target_ = thread;
        thread_failure_ = failure;
        thread_failure_remaining_ = count;
        thread_failure_observed_ = 0U;
    }

    void clear_thread_query_failures() noexcept {
        std::scoped_lock lock{metadata_mutex_};
        thread_failure_target_ = {};
        thread_failure_remaining_ = 0U;
    }

    [[nodiscard]] std::size_t observed_thread_failure_count() const noexcept {
        std::scoped_lock lock{metadata_mutex_};
        return thread_failure_observed_;
    }

    void block_next_queries(std::size_t count) noexcept {
        blocked_queries_.store(count, std::memory_order_release);
    }

    [[nodiscard]] bool wait_for_blocked(std::size_t count) {
        for (std::size_t index = 0U; index < count; ++index) {
            if (!blocked_.try_acquire_for(std::chrono::seconds{2})) {
                return false;
            }
        }
        return true;
    }

    void release_queries(std::size_t count) noexcept {
        for (std::size_t index = 0U; index < count; ++index) {
            release_.release();
        }
    }

private:
    std::atomic<std::size_t> query_calls_{0U};
    std::atomic<std::size_t> completed_queries_{0U};
    std::atomic<bool> busy_next_query_;
    std::atomic<std::size_t> blocked_queries_;
    std::counting_semaphore<8> blocked_{0};
    std::counting_semaphore<8> release_{0};
    mutable std::mutex metadata_mutex_;
    std::optional<metadata_error> next_failure_;
    std::thread::id thread_failure_target_{};
    metadata_error thread_failure_{metadata_error::busy};
    std::size_t thread_failure_remaining_{};
    std::size_t thread_failure_observed_{};
};

class scoped_query_release final {
public:
    scoped_query_release(fake_metadata_provider& provider, std::size_t count) noexcept
        : provider_(provider),
          count_(count) {}

    ~scoped_query_release() { release(); }

    scoped_query_release(const scoped_query_release&) = delete;
    scoped_query_release& operator=(const scoped_query_release&) = delete;
    scoped_query_release(scoped_query_release&&) = delete;
    scoped_query_release& operator=(scoped_query_release&&) = delete;

    void release() noexcept {
        if (active_) {
            provider_.release_queries(count_);
            active_ = false;
        }
    }

private:
    fake_metadata_provider& provider_;
    std::size_t count_{};
    bool active_{true};
};

} // namespace inputnext::gameinput_test_support
