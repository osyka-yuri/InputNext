#pragma once

#include "output/execution/engine.hpp"
#include "output/execution/engine_state.hpp"
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>

namespace inputnext::output::detail {

struct execution_engine_test_access final {
    struct backend_counter_seed final {
        std::uint64_t next_publication{};
        std::uint64_t next_attempt{};
    };

    static void seed_next_backend_id(execution_engine& engine, const std::uint64_t id) noexcept {
        std::scoped_lock lock{engine.implementation_->gate};
        engine.implementation_->next_backend_id = id;
    }

    static void seed_next_lane_generation(execution_engine& engine,
        const std::uint64_t generation) noexcept {
        std::scoped_lock lock{engine.implementation_->gate};
        engine.implementation_->next_lane_generation = generation;
    }

    static bool seed_backend_ids(execution_engine& engine,
        const backend_handle handle,
        const backend_counter_seed counters) noexcept {
        std::scoped_lock lock{engine.implementation_->gate};
        auto* const backend = engine.implementation_->find_backend(handle);
        if (backend == nullptr) {
            return false;
        }
        backend->next_publication_id = counters.next_publication;
        backend->next_attempt_id = counters.next_attempt;
        return true;
    }

    static bool set_backend_due(execution_engine& engine,
        const backend_handle handle,
        const execution_engine::clock::time_point due) noexcept {
        std::scoped_lock lock{engine.implementation_->gate};
        auto* const backend = engine.implementation_->find_backend(handle);
        if (backend == nullptr ||
            backend->state != execution_engine::implementation::backend_state::current) {
            return false;
        }
        backend->due = due;
        return true;
    }

    static bool seed_retryable_failures(execution_engine& engine,
        const backend_handle handle,
        const std::uint32_t count) noexcept {
        std::scoped_lock lock{engine.implementation_->gate};
        auto* const backend = engine.implementation_->find_backend(handle);
        if (backend == nullptr ||
            backend->state != execution_engine::implementation::backend_state::current) {
            return false;
        }
        backend->consecutive_retryable_failures = count;
        return true;
    }
};

} // namespace inputnext::output::detail
