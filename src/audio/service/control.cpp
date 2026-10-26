#include "audio/audio_service.hpp"
#include "detail.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>
#include <mutex>

namespace inputnext::audio_detail {
namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] bool contains_target(const connection_inventory& inventory,
    const connection_target target) noexcept {
    const auto end = inventory.entries.begin() + static_cast<std::ptrdiff_t>(inventory.size);
    return std::ranges::any_of(inventory.entries.begin(),
        end,
        [target](const gameinput_connection& row) { return row.target == target; });
}

} // namespace

bool audio_service::implementation::desired(const connection_target target) const noexcept {
    return accepting.load(std::memory_order_acquire) && contains_target(active_inventory, target);
}

bool audio_service::implementation::has_resident(const connection_target target) const noexcept {
    return std::ranges::any_of(slots, [target](const audio_slot& slot) {
        return slot.lifecycle.load(std::memory_order_acquire) != slot_lifecycle::vacant &&
               slot.target == target;
    });
}

bool audio_service::implementation::backend_is_current(const audio_slot& slot) const noexcept {
    const auto observed = connections.query_backend_progress(slot.target);
    return observed.has_value() && observed->phase == output::backend_phase::current &&
           observed->backend == slot.backend;
}

void audio_service::implementation::run_control() noexcept {
    while (true) {
        try {
            static_cast<void>(control_wakeup.wait_for(audio_control_reconcile_interval));
            process_inventory();
            for (auto& slot : slots) {
                if (slot.lifecycle.load(std::memory_order_acquire) != slot_lifecycle::vacant) {
                    process_slot(slot);
                }
            }
            if (accepting.load(std::memory_order_acquire)) {
                const auto end = active_inventory.entries.begin() +
                                 static_cast<std::ptrdiff_t>(active_inventory.size);
                for (auto row = active_inventory.entries.begin(); row != end; ++row) {
                    maybe_start_target(*row);
                }
            }
        } catch (...) {
            stopping.store(true, std::memory_order_release);
            accepting.store(false, std::memory_order_release);
            for (auto& slot : slots) {
                if (slot.lifecycle.load(std::memory_order_acquire) != slot_lifecycle::vacant) {
                    close_slot(slot);
                }
            }
        }

        publish_progress();
        const bool all_vacant = std::ranges::all_of(slots, [](const audio_slot& slot) {
            return slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::vacant;
        });
        if (stopping.load(std::memory_order_acquire) && all_vacant) {
            return;
        }
    }
}

void audio_service::implementation::process_inventory() noexcept {
    connection_inventory incoming{};
    {
        std::scoped_lock lock{publication_mutex};
        incoming = latest_inventory;
    }
    active_inventory = incoming;

    for (auto& slot : slots) {
        const auto state = slot.lifecycle.load(std::memory_order_acquire);
        if (state == slot_lifecycle::vacant) {
            continue;
        }
        if (stopping.load(std::memory_order_acquire) || !desired(slot.target) ||
            !backend_is_current(slot)) {
            close_slot(slot);
        }
    }

    for (auto& retry : retries) {
        if (retry.occupied && !contains_target(active_inventory, retry.target)) {
            retry = {};
        }
    }
}

void audio_service::implementation::process_slot(audio_slot& slot) noexcept {
    const auto state = slot.lifecycle.load(std::memory_order_acquire);
    if (state == slot_lifecycle::vacant) {
        return;
    }

    const auto identity_deadline = slot.identity_valid_until_ticks.load(std::memory_order_acquire);
    if (!slot.transport_closed.load(std::memory_order_acquire) && identity_deadline != 0 &&
        identity_deadline <= time_ticks(clock_type::now())) {
        close_slot(slot);
    }

    if (!stopping.load(std::memory_order_acquire) && desired(slot.target) &&
        !slot.transport_closed.load(std::memory_order_acquire) &&
        state != slot_lifecycle::retained && !backend_is_current(slot)) {
        close_slot(slot);
    }

    process_setup(slot);
    process_handoff(slot);

    if (slot.native_failed.load(std::memory_order_acquire) || !desired(slot.target) ||
        stopping.load(std::memory_order_acquire)) {
        close_slot(slot);
    }

    settle_attachment(slot);
    if (slot.retirement_settled.load(std::memory_order_acquire) && !slot.stop_notified) {
        slot.stop_notified = true;
        if (slot.port != nullptr) {
            const auto stopped = slot.port->request_stop();
            if (!stopped.signaled) {
                publish_native_error(slot, stopped.error);
            }
        }
    }

    collect_worker(slot);
}

void audio_service::implementation::schedule_retry(const connection_target target) noexcept {
    if (!target.valid() || !contains_target(active_inventory, target)) {
        return;
    }
    auto row = std::ranges::find_if(retries,
        [target](const retry_state& value) { return value.occupied && value.target == target; });
    if (row == retries.end()) {
        row =
            std::ranges::find_if(retries, [](const retry_state& value) { return !value.occupied; });
    }
    if (row == retries.end()) {
        return;
    }
    if (!row->occupied) {
        *row = retry_state{.target = target,
            .next_delay_index = 0U,
            .not_before = {},
            .occupied = true};
    }
    const auto delay_index =
        std::min<std::size_t>(row->next_delay_index, audio_retry_delays.size() - 1U);
    row->not_before = clock_type::now() + audio_retry_delays[delay_index];
    if (row->next_delay_index + 1U < audio_retry_delays.size()) {
        ++row->next_delay_index;
    }
}

void audio_service::implementation::clear_retry(const connection_target target) noexcept {
    for (auto& retry : retries) {
        if (retry.occupied && retry.target == target) {
            retry = {};
        }
    }
}

void audio_service::implementation::record_start_failure(
    const audio_start_failure failure) noexcept {
    schedule_retry(failure.target);
    const auto row = std::ranges::find_if(retries, [&failure](const retry_state& retry) {
        return retry.occupied && retry.target == failure.target;
    });
    if (row != retries.end()) {
        row->last_start_failure = failure;
    }
}

} // namespace inputnext::audio_detail
