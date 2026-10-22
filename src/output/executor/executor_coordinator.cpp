#include "executor_state.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#ifdef INPUTNEXT_OUTPUT_TESTING
#    include "thread_start_hook.hpp"
#endif

#include <new>
#include <system_error>
#include <utility>

namespace inputnext::output {

execution_result<void> executor::implementation::start_coordinator() {
    try {
#ifdef INPUTNEXT_OUTPUT_TESTING
        detail::thread_start_testing::before_start(detail::thread_start_testing::role::coordinator);
#endif
        std::jthread thread{
            [this](const std::stop_token& stop_token) { coordinator_loop(stop_token); }};
        std::scoped_lock lock{lifecycle_gate};
        coordinator = std::move(thread);
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected{execution_error::resources};
    } catch (const std::system_error&) {
        return std::unexpected{execution_error::resources};
    } catch (...) {
        return std::unexpected{execution_error::resources};
    }
}

void executor::implementation::coordinator_loop(const std::stop_token& stop_token) noexcept {
    for (;;) {
        try {
            if (!engine->wait_for_refresh(stop_token)) {
                return;
            }
        } catch (...) {
            close_admission();
            return;
        }
        if (!refresh_current_backends()) {
            return;
        }
    }
}

bool executor::implementation::refresh_current_backends() noexcept {
    const auto batch = engine->current_backends();
    if (!batch.has_value()) {
        close_admission();
        return false;
    }
    for (std::size_t index = 0U; index < batch->count; ++index) {
        if (!refresh_backend(batch->handles[index])) {
            return false;
        }
    }
    return true;
}

bool executor::implementation::refresh_backend(const backend_handle backend) noexcept {
    std::optional<result<logical_snapshot>> snapshot;
    try {
        snapshot.emplace(owner.read_snapshot(backend.target));
    } catch (...) {
        close_admission();
        return false;
    }
    if (!snapshot->has_value()) {
        if (snapshot->error() != error::not_connected) {
            return true;
        }
        const auto retired = engine->retire(backend, retirement_reason::connection_retired);
        if (!retired.has_value() && retired.error() != execution_error::stale_backend) {
            close_admission();
            return false;
        }
        return true;
    }

    const auto published = engine->publish(backend, **snapshot);
    if (published.has_value() || published.error() == execution_error::stale_backend ||
        published.error() == execution_error::identity_exhausted ||
        published.error() == execution_error::resources) {
        return true;
    }
    close_admission();
    return false;
}

} // namespace inputnext::output
