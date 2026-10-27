#include "executor_state.hpp"

#include "output/authority/execution.hpp"
#include "output/execution/engine.hpp"
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/output_observation.hpp>

#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

namespace inputnext::output {

executor::implementation::implementation(authority& authority_owner,
    const execution_owner attached_owner_value)
    : owner(authority_owner),
      attached_owner(attached_owner_value),
      engine(std::make_unique<detail::execution_engine>(authority_owner, attached_owner_value)) {
}

executor::implementation::operation::operation(implementation& state) noexcept : state_(&state) {
    std::scoped_lock lock{state.lifecycle_gate};
    if (state.accepting) {
        ++state.active_operations;
        entered_ = true;
    }
}

executor::implementation::operation::~operation() noexcept {
    if (entered_) {
        state_->finish_operation();
    }
}

void executor::implementation::finish_operation() noexcept {
    {
        std::scoped_lock lock{lifecycle_gate};
        if (active_operations == 0U) {
            std::terminate();
        }
        --active_operations;
    }
    lifecycle_changed.notify_all();
}

void executor::implementation::close_admission() noexcept {
    {
        std::scoped_lock lock{lifecycle_gate};
        accepting = false;
    }
    engine->close_all();
    {
        std::scoped_lock lock{lifecycle_gate};
        engine_closed = true;
    }
    lifecycle_changed.notify_all();
}

executor::executor(std::unique_ptr<implementation> implementation_value) noexcept
    : implementation_(std::move(implementation_value)) {
}

executor::~executor() noexcept {
    shutdown();
}

execution_result<std::unique_ptr<executor>> executor::create(authority& owner) {
    execution_result<execution_owner> attached = std::unexpected{execution_error::resources};
    try {
        attached = detail::authority_execution_access::attach(owner);
    } catch (const std::bad_alloc&) {
        return std::unexpected{execution_error::resources};
    } catch (const std::system_error&) {
        return std::unexpected{execution_error::resources};
    }
    if (!attached.has_value()) {
        return std::unexpected{attached.error()};
    }

    std::unique_ptr<implementation> state;
    try {
        state = std::make_unique<implementation>(owner, *attached);
    } catch (const std::bad_alloc&) {
        const auto detached = detail::authority_execution_access::detach(owner, *attached);
        if (!detached.has_value()) {
            std::terminate();
        }
        return std::unexpected{execution_error::resources};
    } catch (const std::system_error&) {
        const auto detached = detail::authority_execution_access::detach(owner, *attached);
        if (!detached.has_value()) {
            std::terminate();
        }
        return std::unexpected{execution_error::resources};
    }

    std::unique_ptr<executor> created;
    try {
        created.reset(new executor{std::move(state)});
    } catch (const std::bad_alloc&) {
        state.reset();
        const auto detached = detail::authority_execution_access::detach(owner, *attached);
        if (!detached.has_value()) {
            std::terminate();
        }
        return std::unexpected{execution_error::resources};
    }

    const auto coordinator = created->implementation_->start_coordinator();
    if (!coordinator.has_value()) {
        created->shutdown();
        return std::unexpected{coordinator.error()};
    }
    return std::move(created);
}

execution_result<void> executor::request_retry(const backend_handle backend) {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->engine->request_retry(backend);
}

void executor::request_drive() noexcept {
    const implementation::operation active{*implementation_};
    if (active.entered()) {
        implementation_->engine->request_drive();
    }
}

execution_result<retirement_receipt> executor::retire(const backend_handle backend) {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->engine->retire(backend);
}

execution_result<backend_progress> executor::progress(const backend_handle backend) const {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->engine->progress(backend);
}

execution_result<backend_observation> executor::observe(const backend_handle backend) const {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->engine->observe(backend);
}

execution_result<bool> executor::qualifies_route(const body_route_ack& acknowledgement) const {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->engine->qualifies_route(acknowledgement);
}

execution_result<void> executor::acknowledge_retirement(const retirement_receipt& receipt) {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }

    const auto lane = implementation_->engine->lane_for(receipt.backend);
    if (!lane.has_value()) {
        return std::unexpected{lane.error()};
    }
    const auto acknowledged = implementation_->engine->acknowledge_retirement(receipt);
    if (!acknowledged.has_value()) {
        return acknowledged;
    }
    if (!implementation_->engine->worker_should_exit(*lane)) {
        return {};
    }
    {
        std::scoped_lock lock{implementation_->lifecycle_gate};
        if (!implementation_->accepting) {
            return {};
        }
    }
    return implementation_->stop_and_join_worker(*lane);
}

void executor::shutdown() noexcept {
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

} // namespace inputnext::output
