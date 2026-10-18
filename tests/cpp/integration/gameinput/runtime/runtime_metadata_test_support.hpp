#pragma once

#include <inputnext/device_metadata.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <semaphore>

#include <metadata/provider.hpp>

namespace inputnext::gameinput_test_support {

class fake_metadata_provider final : public metadata_provider {
public:
    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const metadata_request& request) override {
        auto blocks = blocked_queries_.load(std::memory_order_acquire);
        while (blocks > 0U && !blocked_queries_.compare_exchange_weak(blocks,
                                  blocks - 1U,
                                  std::memory_order_acq_rel)) {
        }
        if (blocks > 0U) {
            blocked_.release();
            release_.acquire();
        }

        metadata_snapshot result{};
        result.target = request.target;
        result.catalogue_generation = request.catalogue_generation;
        result.native = request.facts;
        result.container_id = request.facts.container_id;
        result.container_id_source = request.facts.container_id.has_value()
                                         ? container_id_provenance::gameinput_native
                                         : container_id_provenance::unavailable;
        return result;
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
    std::atomic<std::size_t> blocked_queries_;
    std::counting_semaphore<8> blocked_{0};
    std::counting_semaphore<8> release_{0};
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
