#include "engine_state.hpp"

#include "engine.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {

execution_engine::implementation::implementation(authority& authority_owner,
    const execution_owner owner)
    : owner(authority_owner),
      execution(owner) {
}

execution_engine::implementation::lane_record* execution_engine::implementation::find_lane(
    const lane_token token) noexcept {
    if (!token.valid() || token.slot >= lanes.size()) {
        return nullptr;
    }
    auto& lane = lanes[token.slot];
    return lane.phase != lane_phase::free && lane.token == token ? &lane : nullptr;
}

const execution_engine::implementation::lane_record* execution_engine::implementation::find_lane(
    const lane_token token) const noexcept {
    if (!token.valid() || token.slot >= lanes.size()) {
        return nullptr;
    }
    const auto& lane = lanes[token.slot];
    return lane.phase != lane_phase::free && lane.token == token ? &lane : nullptr;
}

std::optional<std::size_t> execution_engine::implementation::find_lane(const runtime_id runtime,
    const device_key device) const noexcept {
    for (std::size_t index = 0U; index < lanes.size(); ++index) {
        const auto& lane = lanes[index];
        if (lane.phase != lane_phase::free && lane.runtime == runtime && lane.device == device) {
            return index;
        }
    }
    return std::nullopt;
}

execution_engine::implementation::backend_record* execution_engine::implementation::find_backend(
    const backend_handle handle) noexcept {
    if (!handle.valid() || handle.incarnation.owner != execution) {
        return nullptr;
    }
    const auto iterator = std::ranges::find_if(backends, [handle](const auto& backend) {
        return backend.state != backend_state::free && backend.state != backend_state::reserved &&
               backend.handle == handle;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

const execution_engine::implementation::backend_record*
execution_engine::implementation::find_backend(const backend_handle handle) const noexcept {
    if (!handle.valid() || handle.incarnation.owner != execution) {
        return nullptr;
    }
    const auto iterator = std::ranges::find_if(backends, [handle](const auto& backend) {
        return backend.state != backend_state::free && backend.state != backend_state::reserved &&
               backend.handle == handle;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

execution_engine::implementation::backend_record* execution_engine::implementation::find_reserved(
    const backend_id id,
    const lane_token lane) noexcept {
    const auto iterator = std::ranges::find_if(backends, [id, lane](const auto& backend) {
        return backend.state == backend_state::reserved && backend.id == id && backend.lane == lane;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

execution_engine::implementation::backend_record* execution_engine::implementation::find_current(
    const lane_token lane) noexcept {
    const auto iterator = std::ranges::find_if(backends, [lane](const auto& backend) {
        return backend.state == backend_state::current && backend.lane == lane;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

const execution_engine::implementation::backend_record*
execution_engine::implementation::find_current(const lane_token lane) const noexcept {
    const auto iterator = std::ranges::find_if(backends, [lane](const auto& backend) {
        return backend.state == backend_state::current && backend.lane == lane;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

execution_engine::implementation::backend_record* execution_engine::implementation::find_retiring(
    const lane_token lane) noexcept {
    const auto iterator = std::ranges::find_if(backends, [lane](const auto& backend) {
        return backend.state == backend_state::retiring && backend.lane == lane;
    });
    return iterator == backends.end() ? nullptr : &*iterator;
}

bool execution_engine::implementation::lane_has_records(const lane_token lane) const noexcept {
    return std::ranges::any_of(backends, [lane](const auto& backend) {
        return backend.state != backend_state::free && backend.lane == lane;
    });
}

bool execution_engine::implementation::lane_has_reservations(const lane_token lane) const noexcept {
    return std::ranges::any_of(backends, [lane](const auto& backend) {
        return backend.state == backend_state::reserved && backend.lane == lane;
    });
}

std::size_t execution_engine::implementation::current_lane_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(lanes, [this](const auto& lane) {
        return lane.phase != lane_phase::free &&
               (find_current(lane.token) != nullptr || lane_has_reservations(lane.token));
    }));
}

std::size_t execution_engine::implementation::current_backend_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(backends,
        [](const auto& backend) { return backend.state == backend_state::current; }));
}

bool execution_engine::implementation::lane_has_pending_cleanup(
    const lane_token lane) const noexcept {
    return std::ranges::any_of(backends, [lane](const auto& backend) {
        return (backend.state == backend_state::retiring ||
                   backend.state == backend_state::cleaning) &&
               backend.lane == lane;
    });
}

bool execution_engine::implementation::lane_should_exit(const lane_token token) const noexcept {
    const auto* const lane = find_lane(token);
    if (lane == nullptr) {
        return true;
    }
    if (lane->phase == lane_phase::closing) {
        return !lane_has_pending_cleanup(token);
    }
    return lane->shutdown_requested && !lane_has_pending_cleanup(token);
}

bool execution_engine::implementation::lane_has_due_work(const lane_token lane,
    const clock::time_point now) const noexcept {
    if (lane_has_pending_cleanup(lane)) {
        return true;
    }
    return std::ranges::any_of(backends, [lane, now](const auto& backend) {
        return backend.state == backend_state::current && backend.lane == lane &&
               backend.delivery_needed && !backend.refresh_requested && !backend.write_in_flight &&
               backend.due <= now;
    });
}

std::optional<execution_engine::clock::time_point> execution_engine::implementation::next_due(
    const lane_token lane) const noexcept {
    std::optional<clock::time_point> next;
    for (const auto& backend : backends) {
        if (backend.state != backend_state::current || backend.lane != lane ||
            !backend.delivery_needed || backend.refresh_requested || backend.write_in_flight) {
            continue;
        }
        if (!next.has_value() || backend.due < *next) {
            next = backend.due;
        }
    }
    return next;
}

void execution_engine::implementation::notify_lane(const lane_token token) noexcept {
    if (auto* const lane = find_lane(token); lane != nullptr) {
        lane->changed.notify_all();
    }
}

void execution_engine::implementation::notify_coordinator_refresh() noexcept {
    coordinator_refresh_requested = true;
    refresh_changed.notify_all();
}

void execution_engine::implementation::retire_locked(backend_record& backend,
    const retirement_reason reason) noexcept {
    if (backend.state == backend_state::retiring || backend.state == backend_state::cleaning ||
        backend.state == backend_state::drained) {
        return;
    }
    const auto handle = backend.handle;
    backend.state = backend_state::retiring;
    backend.delivery_needed = false;
    backend.retry_requested = false;
    backend.refresh_requested = false;
    backend.receipt = retirement_receipt{.backend = handle, .reason = reason};
    notify_lane(backend.lane);
    notify_coordinator_refresh();
}

void execution_engine::implementation::maybe_close_lane_locked(const lane_token token) noexcept {
    auto* const lane = find_lane(token);
    if (lane == nullptr || lane_has_records(token) || lane_has_reservations(token)) {
        return;
    }
    if (lane->phase == lane_phase::starting) {
        lane->phase = lane_phase::closing;
        notify_lane(token);
        return;
    }
    if (lane->phase == lane_phase::running) {
        lane->phase = lane_phase::closing;
        notify_lane(token);
    }
}

execution_engine::execution_engine(authority& owner, const execution_owner attached_owner)
    : implementation_(std::make_unique<implementation>(owner, attached_owner)) {
    if (!attached_owner.valid()) {
        std::terminate();
    }
}

execution_engine::~execution_engine() noexcept {
    if (!implementation_) {
        return;
    }
    std::scoped_lock lock{implementation_->gate};
    if (std::ranges::any_of(implementation_->backends, [](const auto& backend) {
            return backend.state != implementation::backend_state::free || backend.sink;
        })) {
        std::terminate();
    }
    if (std::ranges::any_of(implementation_->lanes,
            [](const auto& lane) { return lane.phase != implementation::lane_phase::free; })) {
        std::terminate();
    }
}

} // namespace inputnext::output::detail
