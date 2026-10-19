#include "state.hpp"

#include "gameinput/core/publication_source.hpp"
#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "history/cursor.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext {
namespace {

[[nodiscard]] history_failure map_registry_failure(error failure) noexcept {
    switch (failure) {
    case error::invalid_argument:
        return history_failure{.code = history_error::invalid_argument};
    case error::not_connected:
        return history_failure{.code = history_error::disconnected};
    case error::capacity_exceeded:
        return history_failure{.code = history_error::capacity_exceeded};
    case error::identity_exhausted:
        return history_failure{.code = history_error::identity_exhausted};
    case error::no_data:
        return history_failure{.code = history_error::no_data};
    case error::closed:
        return history_failure{.code = history_error::closed};
    case error::revision_exhausted:
        return history_failure{.code = history_error::provider_failure};
    }
    return history_failure{.code = history_error::provider_failure};
}

} // namespace

gameinput_runtime::implementation::history_provider::history_provider(
    implementation& runtime) noexcept
    : runtime(runtime) {
}

history_result<std::unique_ptr<history_detail::history_cursor>>
gameinput_runtime::implementation::history_provider::open_cursor(connection_target target) {
    if (!target.valid()) {
        return std::unexpected(history_failure{.code = history_error::invalid_argument});
    }

    owner_identity identity{};
    gameinput_detail::native_device_handle device{};
    gameinput_detail::publication_source* source{};
    read_pin residency;
    {
        std::scoped_lock lock{runtime.table_gate};
        if (runtime.state != phase::running) {
            return std::unexpected(history_failure{.code = history_error::closed});
        }
        auto* const owner = runtime.find_live_owner(target);
        if (owner == nullptr || owner->source == nullptr || !owner->device.valid()) {
            return std::unexpected(history_failure{.code = history_error::disconnected});
        }
        if (owner->history_leases == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(history_failure{.code = history_error::capacity_exceeded});
        }

        auto captured = runtime.registry.capture(target);
        if (!captured.has_value()) {
            if (captured.error() == error::not_connected || captured.error() == error::closed) {
                owner->retire_requested = true;
                owner->admission_retry_at.reset();
                owner->registration_retry_at.reset();
                owner->poll_at.reset();
                owner->facts_retry_at.reset();
            }
            return std::unexpected(map_registry_failure(captured.error()));
        }

        residency = std::move(*captured);
        ++owner->history_leases;
        identity = owner_identity{
            .slot_index = static_cast<std::size_t>(owner - runtime.owners.data()),
            .generation = owner->generation,
        };
        device = owner->device;
        source = owner->source;
    }

    history_lease lease{runtime, identity, std::move(residency)};
    try {
        auto cursor = std::make_unique<gameinput_detail::source_history_cursor>(*runtime.native,
            device,
            source->history());
        auto leased = std::make_unique<leased_history_cursor>(std::move(lease), std::move(cursor));
        return std::unique_ptr<history_detail::history_cursor>{std::move(leased)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(history_failure{.code = history_error::capacity_exceeded});
    } catch (...) {
        return std::unexpected(history_failure{.code = history_error::provider_failure});
    }
}

bool gameinput_runtime::implementation::history_provider::is_live(
    connection_target target) noexcept {
    if (!target.valid()) {
        return false;
    }

    read_pin residency;
    try {
        std::scoped_lock lock{runtime.table_gate};
        if (runtime.state != phase::running) {
            return false;
        }
        auto* const owner = runtime.find_live_owner(target);
        if (owner == nullptr || owner->source == nullptr || !owner->device.valid()) {
            return false;
        }

        auto captured = runtime.registry.capture(target);
        if (!captured.has_value()) {
            if (captured.error() == error::not_connected || captured.error() == error::closed) {
                owner->retire_requested = true;
                owner->admission_retry_at.reset();
                owner->registration_retry_at.reset();
                owner->poll_at.reset();
                owner->facts_retry_at.reset();
            }
            return false;
        }
        residency = std::move(*captured);
    } catch (...) {
        return false;
    }

    residency.release();
    return true;
}

gameinput_runtime::implementation::history_lease::history_lease(implementation& runtime,
    owner_identity owner,
    read_pin residency) noexcept
    : runtime(&runtime),
      owner(owner),
      residency(std::move(residency)) {
}

gameinput_runtime::implementation::history_lease::~history_lease() {
    release();
}

gameinput_runtime::implementation::history_lease::history_lease(history_lease&& other) noexcept
    : runtime(std::exchange(other.runtime, nullptr)),
      owner(other.owner),
      residency(std::move(other.residency)) {
}

gameinput_runtime::implementation::history_lease&
gameinput_runtime::implementation::history_lease::operator=(history_lease&& other) noexcept {
    if (this != &other) {
        release();
        runtime = std::exchange(other.runtime, nullptr);
        owner = other.owner;
        residency = std::move(other.residency);
    }
    return *this;
}

void gameinput_runtime::implementation::history_lease::release() noexcept {
    if (runtime == nullptr) {
        return;
    }
    auto* const owner_runtime = std::exchange(runtime, nullptr);
    residency.release();
    owner_runtime->release_history_lease(owner);
}

gameinput_runtime::implementation::leased_history_cursor::leased_history_cursor(history_lease lease,
    std::unique_ptr<gameinput_detail::source_history_cursor> cursor) noexcept
    : lease_(std::move(lease)),
      cursor_(std::move(cursor)) {
}

gameinput_runtime::implementation::leased_history_cursor::~leased_history_cursor() {
    cursor_.reset();
    lease_.release();
}

history_result<history_detail::cursor_seed>
gameinput_runtime::implementation::leased_history_cursor::start(history_time now) noexcept {
    if (cursor_ == nullptr) {
        return std::unexpected(history_failure{.code = history_error::provider_failure});
    }
    return cursor_->start(now);
}

history_detail::cursor_step gameinput_runtime::implementation::leased_history_cursor::next(
    history_time now) noexcept {
    if (cursor_ == nullptr) {
        return history_detail::cursor_step{
            .kind = history_detail::cursor_step_kind::provider_failure,
        };
    }
    return cursor_->next(now);
}

} // namespace inputnext
