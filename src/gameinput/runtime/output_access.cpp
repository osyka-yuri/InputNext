#include "output_access.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_raw_output_report.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "metadata/policy.hpp"
#include "owner_identity.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace inputnext::gameinput_detail {

gameinput_result<std::unique_ptr<runtime_output_access>> runtime_output_access::create(
    gameinput_runtime& runtime) {
    if (runtime.implementation_ == nullptr) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
    }

    std::unique_ptr<runtime_output_access> access;
    try {
        access.reset(new runtime_output_access{runtime});
    } catch (const std::bad_alloc&) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::resource_exhausted});
    }

    auto& implementation = *runtime.implementation_;
    std::unique_lock service_lock{implementation.service_gate};
    {
        std::scoped_lock table_lock{implementation.table_gate};
        if (implementation.state != gameinput_runtime::implementation::phase::created) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::already_started});
        }
        if (implementation.output_enabled) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::already_started});
        }
    }

    std::thread cleanup_worker;
    try {
        cleanup_worker = std::thread{[&implementation] { implementation.output_cleanup_loop(); }};
    } catch (const std::system_error&) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::resource_exhausted});
    } catch (const std::bad_alloc&) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::resource_exhausted});
    }

    {
        std::scoped_lock table_lock{implementation.table_gate};
        implementation.output_enabled = true;
        implementation.output_admission_open = true;
        implementation.output_cleanup_worker = std::move(cleanup_worker);
        access->active_ = true;
    }
    return std::move(access);
}

runtime_output_access::~runtime_output_access() noexcept {
    close_admission();
}

gameinput_result<output_description> runtime_output_access::describe(
    connection_target target) const {
    if (!target.valid() || runtime_ == nullptr || runtime_->implementation_ == nullptr) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::invalid_argument});
    }

    auto& implementation = *runtime_->implementation_;
    std::uint64_t slot_generation{};
    {
        std::scoped_lock table_lock{implementation.table_gate};
        if (implementation.state != gameinput_runtime::implementation::phase::running ||
            !implementation.output_admission_open) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
        }
        const auto* const owner = implementation.find_live_owner(target);
        if (owner == nullptr) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
        }
        slot_generation = owner->generation;
    }

    output_description description{.target = target,
        .catalogue_generation = 0U,
        .capabilities = std::nullopt,
        .model = std::nullopt};
    {
        std::scoped_lock catalogue_lock{implementation.catalogue_gate};
        const auto* const entry = implementation.find_catalogue(target, slot_generation);
        if (entry != nullptr) {
            description.catalogue_generation = entry->catalogue_generation;
            if (entry->facts.gameinput_capabilities.has_value()) {
                const auto& facts = entry->facts.gameinput_capabilities.value;
                description.capabilities = rumble_capabilities{
                    .low_frequency = facts.low_frequency_rumble,
                    .high_frequency = facts.high_frequency_rumble,
                };
            }
            const auto model = metadata_detail::classify_device_model(entry->facts);
            if (model.has_value()) {
                description.model = model.value;
            }
        }
    }

    {
        std::scoped_lock table_lock{implementation.table_gate};
        const auto* const owner = implementation.find_live_owner(target);
        if (implementation.state != gameinput_runtime::implementation::phase::running ||
            !implementation.output_admission_open || owner == nullptr ||
            owner->generation != slot_generation) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
        }
    }
    return description;
}

metadata_result<bool> runtime_output_access::qualify_dualsense_usb(const connection_target target,
    const std::uint64_t expected_catalogue_generation) const {
    if (!target.valid() || runtime_ == nullptr || runtime_->implementation_ == nullptr) {
        return std::unexpected(metadata_error::not_connected);
    }

    auto queried = runtime_->query_metadata(target);
    if (!queried.has_value()) {
        return std::unexpected(queried.error());
    }
    if (queried->target != target ||
        queried->catalogue_generation != expected_catalogue_generation) {
        return std::unexpected(metadata_error::stale);
    }
    if (queried->container_id_source == container_id_provenance::conflicting ||
        !queried->model.has_value() ||
        (queried->model.value != metadata_device_model::dualsense &&
            queried->model.value != metadata_device_model::dualsense_edge) ||
        !queried->transport.has_value() ||
        queried->transport.value.value != physical_transport::usb) {
        return false;
    }

    return queried->transport.value.provenance == transport_provenance::native_bus_type ||
           queried->transport.value.provenance == transport_provenance::native_enumerator;
}

gameinput_result<runtime_output_lease> runtime_output_access::reserve(connection_target target) {
    if (!target.valid() || runtime_ == nullptr || runtime_->implementation_ == nullptr) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::invalid_argument});
    }

    auto& implementation = *runtime_->implementation_;
    std::size_t slot_index{};
    std::uint64_t generation{};
    native_device_handle device{};
    {
        std::scoped_lock table_lock{implementation.table_gate};
        if (implementation.state != gameinput_runtime::implementation::phase::running ||
            !implementation.output_admission_open) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
        }
        const auto* const owner = implementation.find_live_owner(target);
        if (owner == nullptr) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
        }
        if (owner->output_leases != 0U) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::resource_exhausted});
        }
        slot_index = static_cast<std::size_t>(owner - implementation.owners.data());
        generation = owner->generation;
        device = owner->device;
    }

    auto pin = implementation.registry.capture(target);
    if (!pin.has_value()) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
    }

    std::scoped_lock table_lock{implementation.table_gate};
    if (implementation.state != gameinput_runtime::implementation::phase::running ||
        !implementation.output_admission_open || slot_index >= implementation.owners.size()) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
    }
    auto& owner = implementation.owners[slot_index];
    if (owner.generation != generation || owner.target != target || owner.device != device ||
        owner.state != gameinput_runtime::implementation::owner_phase::active ||
        owner.retire_requested) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::closed});
    }
    const bool native_handle_leased =
        std::ranges::any_of(implementation.owners, [device](const auto& candidate) {
            return candidate.state != gameinput_runtime::implementation::owner_phase::free &&
                   candidate.device == device && candidate.output_leases != 0U;
        });
    if (native_handle_leased) {
        return std::unexpected(gameinput_failure{.code = gameinput_error::resource_exhausted});
    }
    owner.output_leases = 1U;
    return runtime_output_lease{*runtime_,
        runtime_owner_identity{.slot_index = slot_index, .generation = generation},
        device,
        target,
        std::move(*pin)};
}

void runtime_output_access::close_admission() noexcept {
    if (!active_ || runtime_ == nullptr || runtime_->implementation_ == nullptr) {
        return;
    }
    auto& implementation = *runtime_->implementation_;
    {
        std::scoped_lock table_lock{implementation.table_gate};
        implementation.output_admission_open = false;
    }
    implementation.query_drained.notify_all();
}

runtime_output_lease::runtime_output_lease(gameinput_runtime& runtime,
    runtime_owner_identity identity,
    native_device_handle device,
    connection_target target,
    read_pin residency) noexcept
    : runtime_(&runtime),
      slot_index_(identity.slot_index),
      generation_(identity.generation),
      device_(device),
      target_(target),
      residency_(std::move(residency)) {
}

runtime_output_lease::~runtime_output_lease() {
    reset();
}

runtime_output_lease::runtime_output_lease(runtime_output_lease&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)),
      slot_index_(std::exchange(other.slot_index_, 0U)),
      generation_(std::exchange(other.generation_, 0U)),
      device_(std::exchange(other.device_, native_device_handle{})),
      target_(std::exchange(other.target_, connection_target{})),
      residency_(std::move(other.residency_)) {
}

runtime_output_lease& runtime_output_lease::operator=(runtime_output_lease&& other) noexcept {
    if (this != &other) {
        reset();
        runtime_ = std::exchange(other.runtime_, nullptr);
        slot_index_ = std::exchange(other.slot_index_, 0U);
        generation_ = std::exchange(other.generation_, 0U);
        device_ = std::exchange(other.device_, native_device_handle{});
        target_ = std::exchange(other.target_, connection_target{});
        residency_ = std::move(other.residency_);
    }
    return *this;
}

native_result<std::unique_ptr<native_rumble_endpoint>>
runtime_output_lease::create_rumble_endpoint() const {
    if (runtime_ == nullptr || runtime_->implementation_ == nullptr || !target_.valid() ||
        !device_.valid()) {
        return std::unexpected(native_failure{.cause = native_failure_cause::native_failure});
    }

    auto& implementation = *runtime_->implementation_;
    {
        std::scoped_lock table_lock{implementation.table_gate};
        if (slot_index_ >= implementation.owners.size()) {
            return std::unexpected(native_failure{.cause = native_failure_cause::native_failure});
        }
        const auto& owner = implementation.owners[slot_index_];
        if (owner.generation != generation_ || owner.target != target_ || owner.device != device_ ||
            owner.output_leases != 1U ||
            owner.state == gameinput_runtime::implementation::owner_phase::free ||
            !implementation.output_enabled || implementation.native == nullptr) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::device_disconnected});
        }
    }

    try {
        return implementation.native->create_rumble_endpoint(device_);
    } catch (const std::bad_alloc&) {
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    } catch (...) {
        return std::unexpected(native_failure{.cause = native_failure_cause::native_failure});
    }
}

raw_report_result runtime_output_lease::create_raw_output_report(
    const std::uint32_t report_id) const {
    if (runtime_ == nullptr || runtime_->implementation_ == nullptr || !target_.valid() ||
        !device_.valid()) {
        return std::unexpected(
            output::native_result{.status = output::native_status::disconnected});
    }

    auto& implementation = *runtime_->implementation_;
    {
        std::scoped_lock table_lock{implementation.table_gate};
        if (slot_index_ >= implementation.owners.size()) {
            return std::unexpected(
                output::native_result{.status = output::native_status::disconnected});
        }
        const auto& owner = implementation.owners[slot_index_];
        if (owner.generation != generation_ || owner.target != target_ || owner.device != device_ ||
            owner.output_leases != 1U ||
            owner.state == gameinput_runtime::implementation::owner_phase::free ||
            !implementation.output_enabled || implementation.native == nullptr) {
            return std::unexpected(
                output::native_result{.status = output::native_status::disconnected});
        }
    }

    try {
        return implementation.native->create_raw_output_report(device_, report_id);
    } catch (...) {
        return std::unexpected(
            output::native_result{.status = output::native_status::retryable_failure});
    }
}

void runtime_output_lease::reset() noexcept {
    if (runtime_ == nullptr) {
        return;
    }
    auto const* const runtime = std::exchange(runtime_, nullptr);
    const auto slot_index = std::exchange(slot_index_, 0U);
    const auto generation = std::exchange(generation_, 0U);
    device_ = {};
    target_ = {};
    if (runtime->implementation_ != nullptr) {
        runtime->implementation_->release_output_lease(
            gameinput_runtime::implementation::owner_identity{
                .slot_index = slot_index,
                .generation = generation,
            },
            std::move(residency_));
    } else {
        residency_.release();
    }
}

} // namespace inputnext::gameinput_detail
