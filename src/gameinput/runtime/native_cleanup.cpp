#include "state.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/input_binding.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace inputnext {

void gameinput_runtime::implementation::notify_output_cleanup() noexcept {
    output_cleanup_wake.notify_one();
}

void gameinput_runtime::implementation::release_output_lease(owner_identity identity,
    read_pin residency) noexcept {
    residency.release();
    const auto reclaimed = registry.drain_retired();
    static_cast<void>(reclaimed);

    bool wake{};
    {
        std::scoped_lock table_lock{table_gate};
        if (identity.slot_index < owners.size()) {
            auto& owner = owners[identity.slot_index];
            if (owner.generation == identity.generation && owner.output_leases != 0U) {
                --owner.output_leases;
                if (output_enabled && owner.state == owner_phase::retiring &&
                    owner.registry_retired && !owner.release_started &&
                    !owner.reading_token.valid() && !owner.reading_registration_inflight &&
                    owner.history_leases == 0U && owner.output_leases == 0U) {
                    owner.state = owner_phase::releasing;
                    owner.release_started = true;
                    wake = true;
                }
            }
        }
    }
    if (wake) {
        notify_output_cleanup();
    }
    query_drained.notify_all();
}

gameinput_runtime::implementation::output_cleanup_work
gameinput_runtime::implementation::wait_for_output_cleanup_work(owner_identity& identity,
    gameinput_detail::native_device_handle& device,
    read_pin& residency,
    std::unique_ptr<gameinput_detail::native_gameinput>& final_native) {
    std::unique_lock table_lock{table_gate};
    output_cleanup_wake.wait(table_lock, [this] {
        const bool releasable = std::ranges::any_of(owners, [](const owner_slot& owner) {
            return owner.state == owner_phase::releasing && !owner.release_inflight;
        });
        return releasable || (native_cleanup_requested && !native_cleanup_started);
    });

    const auto found = std::ranges::find_if(owners, [](const owner_slot& owner) {
        return owner.state == owner_phase::releasing && !owner.release_inflight;
    });
    if (found != owners.end()) {
        const auto index = static_cast<std::size_t>(found - owners.begin());
        found->release_inflight = true;
        identity = owner_identity{.slot_index = index, .generation = found->generation};
        device = found->device;
        residency = std::move(found->residency);
        return output_cleanup_work::release_device;
    }

    native_cleanup_started = true;
    final_native = std::move(native);
    return output_cleanup_work::release_native_api;
}

void gameinput_runtime::implementation::finish_output_device_release(owner_identity identity,
    gameinput_detail::native_device_handle device,
    read_pin residency) noexcept {
    if (device.valid() && native != nullptr) {
        native->release(device);
    }
    residency.release();
    const auto reclaimed = registry.drain_retired();
    static_cast<void>(reclaimed);
    {
        std::scoped_lock table_lock{table_gate};
        if (identity.slot_index < owners.size()) {
            auto& owner = owners[identity.slot_index];
            if (owner.generation == identity.generation && owner.state == owner_phase::releasing &&
                owner.release_inflight) {
                owner = owner_slot{};
                owner.reading_owner_context = reading_owner{
                    .runtime = this,
                    .slot_index = identity.slot_index,
                    .generation = 0U,
                };
                owner.reading_context = gameinput_detail::native_reading_callback_context{
                    .owner = &owner.reading_owner_context,
                    .deliver = &reading_event,
                };
                if (state == phase::running) {
                    request_rescan_locked();
                }
            }
        }
    }
    query_drained.notify_all();
}

void gameinput_runtime::implementation::finish_output_native_cleanup(
    std::unique_ptr<gameinput_detail::native_gameinput> final_native) noexcept {
    final_native.reset();
    {
        std::scoped_lock table_lock{table_gate};
        native_cleanup_finished = true;
        state = phase::drained;
    }
    query_drained.notify_all();
}

void gameinput_runtime::implementation::output_cleanup_loop() noexcept {
    for (;;) {
        owner_identity release_owner{};
        gameinput_detail::native_device_handle release_device{};
        read_pin release_residency{};
        std::unique_ptr<gameinput_detail::native_gameinput> final_native;
        const auto work = wait_for_output_cleanup_work(release_owner,
            release_device,
            release_residency,
            final_native);
        if (work == output_cleanup_work::release_device) {
            finish_output_device_release(release_owner,
                release_device,
                std::move(release_residency));
            continue;
        }

        finish_output_native_cleanup(std::move(final_native));
        return;
    }
}

} // namespace inputnext
