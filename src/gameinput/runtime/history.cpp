#include "state.hpp"

#include "factory.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <cassert>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

namespace inputnext {

std::optional<history_failure> gameinput_runtime::implementation::begin_history_call(
    bool allow_stopping,
    history_open_hook_snapshot* hook_snapshot) noexcept {
    std::scoped_lock lock{table_gate};
    if (state != phase::running && (!allow_stopping || state != phase::stopping)) {
        return history_failure{.code = history_error::closed};
    }
    if (active_history_calls == std::numeric_limits<std::size_t>::max()) {
        return history_failure{.code = history_error::capacity_exceeded};
    }
    ++active_history_calls;
    if (hook_snapshot != nullptr) {
        *hook_snapshot = history_open_hook_snapshot{
            .hook = before_history_open_test_hook,
            .context = before_history_open_test_context,
        };
    }
    return std::nullopt;
}

void gameinput_runtime::implementation::end_history_call() noexcept {
    {
        std::scoped_lock lock{table_gate};
        assert(active_history_calls != 0U);
        if (active_history_calls != 0U) {
            --active_history_calls;
        }
    }
    query_drained.notify_all();
}

void gameinput_runtime::implementation::release_history_lease(owner_identity owner) noexcept {
    {
        std::scoped_lock lock{table_gate};
        assert(owner.slot_index < owners.size());
        if (owner.slot_index < owners.size()) {
            auto& slot = owners[owner.slot_index];
            assert(slot.generation == owner.generation);
            assert(slot.history_leases != 0U);
            if (slot.generation == owner.generation && slot.history_leases != 0U) {
                --slot.history_leases;
            }
        }
    }
    query_drained.notify_all();
}

history_result<history_reader_handle>
gameinput_runtime::implementation::open_history(connection_target target, history_time now) {
    history_open_hook_snapshot hook_snapshot{};
    if (const auto failure = begin_history_call(false, &hook_snapshot); failure.has_value()) {
        return std::unexpected(*failure);
    }
    const auto finish_call = [](implementation* runtime) noexcept {
        runtime->end_history_call();
    };
    const std::unique_ptr<implementation, decltype(finish_call)> call_guard{this, finish_call};

    if (hook_snapshot.hook != nullptr) {
        hook_snapshot.hook(hook_snapshot.context);
    }

    try {
        return history.open(target, now);
    } catch (const std::bad_alloc&) {
        return std::unexpected(history_failure{.code = history_error::capacity_exceeded});
    } catch (...) {
        return std::unexpected(history_failure{.code = history_error::provider_failure});
    }
}

history_batch gameinput_runtime::implementation::read_history(history_reader_handle reader,
    history_time now) {
    if (const auto failure = begin_history_call(false); failure.has_value()) {
        return history_batch{
            .domain = history_domain{.target = reader.target},
            .failure = *failure,
        };
    }
    const auto finish_call = [](implementation* runtime) noexcept {
        runtime->end_history_call();
    };
    const std::unique_ptr<implementation, decltype(finish_call)> call_guard{this, finish_call};

    try {
        return history.read(reader, now);
    } catch (const std::bad_alloc&) {
        return history_batch{
            .domain = history_domain{.target = reader.target},
            .failure = history_failure{.code = history_error::capacity_exceeded},
        };
    } catch (...) {
        return history_batch{
            .domain = history_domain{.target = reader.target},
            .failure = history_failure{.code = history_error::provider_failure},
        };
    }
}

history_result<void> gameinput_runtime::implementation::close_history(
    history_reader_handle reader) {
    if (const auto failure = begin_history_call(true); failure.has_value()) {
        return std::unexpected(*failure);
    }
    const auto finish_call = [](implementation* runtime) noexcept {
        runtime->end_history_call();
    };
    const std::unique_ptr<implementation, decltype(finish_call)> call_guard{this, finish_call};

    try {
        return history.close(reader);
    } catch (const std::bad_alloc&) {
        return std::unexpected(history_failure{.code = history_error::capacity_exceeded});
    } catch (...) {
        return std::unexpected(history_failure{.code = history_error::provider_failure});
    }
}

void gameinput_detail::runtime_factory::set_before_history_open_test_hook(
    gameinput_runtime& runtime,
    void (*hook)(void*) noexcept,
    void* context) noexcept {
    auto& implementation = *runtime.implementation_;
    std::scoped_lock lock{implementation.table_gate};
    implementation.before_history_open_test_hook = hook;
    implementation.before_history_open_test_context = context;
}

history_detail::history_store& gameinput_detail::runtime_factory::history_for_test(
    gameinput_runtime& runtime) noexcept {
    return runtime.implementation_->history;
}

} // namespace inputnext
