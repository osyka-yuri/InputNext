#include "implementation.hpp"

#include "backend_observation.hpp"
#include "error_mapping.hpp"
#include "identity.hpp"

#include "effects/effects.hpp"

#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>

namespace inputnext::effects {

service::service(service_dependencies dependencies)
    : implementation_(std::make_unique<implementation>(dependencies)) {
}

service::~service() noexcept {
    if (implementation_ != nullptr) {
        static_cast<void>(revoke_for_shutdown());
    }
}

result<context_handle> service::open(const connection_target target) {
    auto& state = *implementation_;
    if (!target.valid() || target.runtime != state.identity) {
        return std::unexpected{status::stale_target};
    }

    {
        std::scoped_lock lock{state.gate};
        if (!state.accepting) {
            return std::unexpected{status::unavailable};
        }
    }

    gameinput_detail::managed_backend_observation backend{};
    const auto availability = current_backend(state.dependencies.connections, target, backend);
    if (availability != status::ok) {
        return std::unexpected{availability};
    }
    if (backend.execution.progress.represented_channels == 0U) {
        return std::unexpected{status::unsupported_target};
    }

    std::scoped_lock lock{state.gate};
    if (!state.accepting) {
        return std::unexpected{status::unavailable};
    }

    const auto slot = std::ranges::find_if(state.contexts, [](const context_record& record) {
        return record.phase == context_phase::free ||
               record.phase == context_phase::acknowledged_tombstone;
    });
    if (slot == state.contexts.end()) {
        return std::unexpected{status::capacity_exceeded};
    }

    const auto external_id = state.dependencies.identities.take_context();
    if (!external_id.has_value()) {
        return std::unexpected{external_id.error()};
    }
    const auto close_publication = state.dependencies.identities.take_publication();
    if (!close_publication.has_value()) {
        return std::unexpected{close_publication.error()};
    }

    *slot = context_record{.phase = context_phase::reserved,
        .external_context = context_handle{.runtime = state.identity, .id = *external_id},
        .target = target,
        .close_publication = *close_publication};

    try {
        const auto opened = state.dependencies.authority.open(target);
        if (!opened.has_value()) {
            *slot = context_record{};
            return std::unexpected{map_output_error(opened.error())};
        }
        const auto& native_context = *opened;
        slot->native_context = native_context;
        slot->phase = context_phase::open;
        return slot->external_context;
    } catch (const std::bad_alloc&) {
        *slot = context_record{};
        return std::unexpected{status::capacity_exceeded};
    } catch (...) {
        *slot = context_record{};
        return std::unexpected{status::io_failed};
    }
}

result<context_info> service::inspect(const context_handle context) {
    auto& state = *implementation_;
    if (!context.runtime.valid() || context.runtime != state.identity || context.id == 0U) {
        return std::unexpected{status::stale_context};
    }

    std::scoped_lock lock{state.gate};
    auto* const record = state.find(context);
    if (record == nullptr || record->phase == context_phase::reserved ||
        record->phase == context_phase::free) {
        return std::unexpected{status::stale_context};
    }
    if (record->phase == context_phase::acknowledged_tombstone) {
        return std::unexpected{status::stale_context};
    }
    if (record->phase == context_phase::closed_retained) {
        return context_info{.context = context, .state = context_state::revoked};
    }

    try {
        const auto inspected = state.dependencies.authority.inspect(record->native_context);
        if (inspected.has_value()) {
            return context_info{.context = context, .state = context_state::open};
        }
        if (inspected.error() == output::error::revoked) {
            const auto retained = state.close_record(*record);
            if (retained != status::ok) {
                return std::unexpected{retained};
            }
            return context_info{.context = context, .state = context_state::revoked};
        }
        if (inspected.error() == output::error::stale_context &&
            state.target_retired(record->target)) {
            service::implementation::retain_retired_close(*record);
            return context_info{.context = context, .state = context_state::revoked};
        }
        return std::unexpected{map_output_error(inspected.error())};
    } catch (const std::bad_alloc&) {
        return std::unexpected{status::capacity_exceeded};
    } catch (...) {
        return std::unexpected{status::io_failed};
    }
}

result<close_receipt> service::close(const context_handle context,
    const std::uint64_t external_close_key) {
    auto& state = *implementation_;
    if (!context.runtime.valid() || context.runtime != state.identity || context.id == 0U) {
        return std::unexpected{status::stale_context};
    }
    if (external_close_key != context.id) {
        return std::unexpected{status::invalid_argument};
    }

    std::scoped_lock lock{state.gate};
    auto* const record = state.find(context);
    if (record == nullptr || record->phase == context_phase::free ||
        record->phase == context_phase::reserved ||
        record->phase == context_phase::acknowledged_tombstone) {
        return std::unexpected{status::stale_context};
    }
    if (record->external_close.has_value()) {
        return *record->external_close;
    }

    const auto closed = state.close_record(*record);
    if (closed != status::ok) {
        return std::unexpected{closed};
    }
    if (!record->external_close.has_value()) {
        return std::unexpected{status::io_failed};
    }
    return *record->external_close;
}

status service::acknowledge(const close_receipt& receipt) {
    auto& state = *implementation_;
    if (!receipt.context.runtime.valid() || receipt.context.runtime != state.identity ||
        receipt.context.id == 0U || receipt.close_request_id != receipt.context.id ||
        !receipt.target.valid() || receipt.target.runtime != state.identity ||
        receipt.publication.value == 0U) {
        return status::invalid_argument;
    }

    std::scoped_lock lock{state.gate};
    auto* const record = state.find(receipt.context);
    if (record == nullptr || !record->external_close.has_value() ||
        *record->external_close != receipt) {
        return status::stale_context;
    }
    if (record->phase == context_phase::acknowledged_tombstone) {
        return status::ok;
    }
    if (record->phase != context_phase::closed_retained) {
        return status::stale_context;
    }

    const auto released = state.release_native(*record);
    if (released != status::ok) {
        return released;
    }
    record->phase = context_phase::acknowledged_tombstone;
    record->latest_discrete = {};
    record->latest_body = {};
    record->native_close.reset();
    record->body_identity.reset();
    return status::ok;
}

void service::close_admission() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    std::scoped_lock lock{implementation_->gate};
    implementation_->accepting = false;
}

} // namespace inputnext::effects
