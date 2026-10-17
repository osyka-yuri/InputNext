#include <expected>
#include <inputnext/device_registry.hpp>

#include "connection_record.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/source_lifetime.hpp>

namespace inputnext {
namespace {

constexpr std::size_t max_live_connections = 16U;
constexpr std::size_t max_resident_connections = 128U;

using record_type = detail::connection_record;

[[nodiscard]] bool has_target(record_type& record, connection_target target) {
    std::scoped_lock lock{record.gate};
    return record.phase == detail::connection_phase::live && record.target == target;
}

[[nodiscard]] record_type* find_free_record(
    std::array<record_type, max_resident_connections>& records) {
    for (auto& record : records) {
        std::scoped_lock lock{record.gate};
        if (record.phase == detail::connection_phase::free) {
            return &record;
        }
    }
    return nullptr;
}

[[nodiscard]] record_type* find_live_record(
    const std::array<std::atomic<record_type*>, max_live_connections>& slots,
    connection_target target,
    std::size_t& slot_index) {
    for (std::size_t index = 0; index < slots.size(); ++index) {
        auto* const record = slots[index].load(std::memory_order_acquire);
        if (record != nullptr && has_target(*record, target)) {
            slot_index = index;
            return record;
        }
    }
    return nullptr;
}

void initialize_reserved_record(record_type& record,
    connection_target target,
    std::unique_ptr<source_lifetime>& source) noexcept {
    std::scoped_lock lock{record.gate};
    record.phase = detail::connection_phase::reserved;
    record.target = target;
    record.pin_count = 0U;
    record.latest.emplace(0U);
    record.source = std::move(source);
}

void activate_record(record_type& record) noexcept {
    std::scoped_lock lock{record.gate};
    record.phase = detail::connection_phase::live;
}

[[nodiscard]] bool close_record(record_type& record, connection_target expected) {
    std::scoped_lock lock{record.gate};
    if (record.phase != detail::connection_phase::live || record.target != expected ||
        !record.latest.has_value()) {
        return false;
    }
    record.latest->close();
    record.phase = detail::connection_phase::retired;
    return true;
}

} // namespace

struct device_registry::storage final {
    storage(runtime_id runtime_value, identity_seed seed)
        : runtime(runtime_value),
          next_device(seed.next_device),
          next_connection(seed.next_connection) {
        if (!runtime.valid() || seed.next_device == 0U || seed.next_connection == 0U) {
            throw std::invalid_argument{"device_registry requires valid runtime identity seeds"};
        }
        for (auto& slot : live_slots) {
            slot.store(nullptr, std::memory_order_relaxed);
        }
    }

    storage(const storage&) = delete;
    storage& operator=(const storage&) = delete;
    storage(storage&&) = delete;
    storage& operator=(storage&&) = delete;
    ~storage() = default;

    [[nodiscard]] bool device_identity_available() const noexcept { return !device_exhausted; }

    [[nodiscard]] bool connection_identity_available() const noexcept {
        return !connection_exhausted;
    }

    [[nodiscard]] device_key consume_device_identity() noexcept {
        const auto value = next_device;
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            device_exhausted = true;
        } else {
            ++next_device;
        }
        return device_key{value};
    }

    [[nodiscard]] connection_id consume_connection_identity() noexcept {
        const auto value = next_connection;
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            connection_exhausted = true;
        } else {
            ++next_connection;
        }
        return connection_id{value};
    }

    [[nodiscard]] std::size_t drain_reclaimable() {
        std::size_t reclaimed = 0U;
        for (auto& record : records) {
            std::unique_ptr<source_lifetime> source;
            {
                std::scoped_lock lock{record.gate};
                if (record.phase != detail::connection_phase::retired || record.pin_count != 0U) {
                    continue;
                }
                record.phase = detail::connection_phase::draining;
                source = std::move(record.source);
            }

            source.reset();

            {
                std::scoped_lock lock{record.gate};
                record.latest.reset();
                record.target = {};
                resident_count.fetch_sub(1U, std::memory_order_relaxed);
                record.phase = detail::connection_phase::free;
            }
            ++reclaimed;
        }
        return reclaimed;
    }

    void retire_live_connections() {
        std::size_t retired = 0U;
        for (auto& live_slot : live_slots) {
            auto* const record = live_slot.load(std::memory_order_acquire);
            if (record == nullptr) {
                continue;
            }
            connection_target target;
            {
                std::scoped_lock lock{record->gate};
                target = record->target;
            }
            if (close_record(*record, target)) {
                live_slot.store(nullptr, std::memory_order_release);
                ++retired;
            }
        }
        if (retired != 0U) {
            live_count.fetch_sub(retired, std::memory_order_relaxed);
        }
    }

    const runtime_id runtime;
    mutable std::mutex lifecycle;
    std::array<record_type, max_resident_connections> records{};
    std::array<std::atomic<record_type*>, max_live_connections> live_slots{};
    std::atomic<std::size_t> live_count{0U};
    std::atomic<std::size_t> resident_count{0U};
    std::uint64_t next_device{};
    std::uint64_t next_connection{};
    bool device_exhausted{};
    bool connection_exhausted{};
    bool closed{};
};

device_registry::device_registry(runtime_id runtime, identity_seed seed)
    : storage_(std::make_unique<storage>(runtime, seed)) {
}

device_registry::~device_registry() noexcept {
    shutdown();
}

result<admitted_connection> device_registry::admit(std::unique_ptr<source_lifetime>& candidate) {
    if (!candidate) {
        return std::unexpected{error::invalid_argument};
    }

    auto& state = *storage_;
    std::scoped_lock lifecycle_lock{state.lifecycle};
    if (state.closed) {
        return std::unexpected{error::closed};
    }
    if (state.live_count.load(std::memory_order_relaxed) >= max_live_connections) {
        return std::unexpected{error::capacity_exceeded};
    }
    if (!state.device_identity_available() || !state.connection_identity_available()) {
        return std::unexpected{error::identity_exhausted};
    }

    auto* record = find_free_record(state.records);
    if (record == nullptr ||
        state.resident_count.load(std::memory_order_relaxed) >= max_resident_connections) {
        static_cast<void>(state.drain_reclaimable());
        record = find_free_record(state.records);
    }
    if (record == nullptr ||
        state.resident_count.load(std::memory_order_relaxed) >= max_resident_connections) {
        return std::unexpected{error::capacity_exceeded};
    }

    std::size_t live_slot = max_live_connections;
    for (std::size_t index = 0; index < state.live_slots.size(); ++index) {
        if (state.live_slots[index].load(std::memory_order_acquire) == nullptr) {
            live_slot = index;
            break;
        }
    }
    if (live_slot == max_live_connections) {
        return std::unexpected{error::capacity_exceeded};
    }

    const connection_target target{
        .runtime = state.runtime,
        .device = device_key{state.next_device},
        .connection = connection_id{state.next_connection},
    };
    state.resident_count.fetch_add(1U, std::memory_order_relaxed);
    initialize_reserved_record(*record, target, candidate);
    static_cast<void>(state.consume_device_identity());
    static_cast<void>(state.consume_connection_identity());
    activate_record(*record);
    state.live_slots[live_slot].store(record, std::memory_order_release);
    state.live_count.fetch_add(1U, std::memory_order_relaxed);

    return admitted_connection{
        .target = target,
        .reader = read_binding{record, target},
        .producer = producer_binding{record, target},
    };
}

result<admitted_connection> device_registry::replace(connection_target expected,
    std::unique_ptr<source_lifetime>& candidate) {
    if (!candidate || !expected.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    auto& state = *storage_;
    std::scoped_lock lifecycle_lock{state.lifecycle};
    if (state.closed) {
        return std::unexpected{error::closed};
    }
    if (expected.runtime != state.runtime) {
        return std::unexpected{error::not_connected};
    }
    if (!state.connection_identity_available()) {
        return std::unexpected{error::identity_exhausted};
    }

    std::size_t live_slot = max_live_connections;
    auto* const old_record = find_live_record(state.live_slots, expected, live_slot);
    if (old_record == nullptr) {
        return std::unexpected{error::not_connected};
    }

    auto* new_record = find_free_record(state.records);
    if (new_record == nullptr ||
        state.resident_count.load(std::memory_order_relaxed) >= max_resident_connections) {
        static_cast<void>(state.drain_reclaimable());
        new_record = find_free_record(state.records);
    }
    if (new_record == nullptr ||
        state.resident_count.load(std::memory_order_relaxed) >= max_resident_connections) {
        return std::unexpected{error::capacity_exceeded};
    }

    const connection_target target{
        .runtime = state.runtime,
        .device = expected.device,
        .connection = connection_id{state.next_connection},
    };
    state.resident_count.fetch_add(1U, std::memory_order_relaxed);
    initialize_reserved_record(*new_record, target, candidate);

    static_cast<void>(close_record(*old_record, expected));
    state.live_slots[live_slot].store(nullptr, std::memory_order_release);
    activate_record(*new_record);
    state.live_slots[live_slot].store(new_record, std::memory_order_release);
    static_cast<void>(state.consume_connection_identity());

    return admitted_connection{
        .target = target,
        .reader = read_binding{new_record, target},
        .producer = producer_binding{new_record, target},
    };
}

result<void> device_registry::retire(connection_target target) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    auto& state = *storage_;
    std::scoped_lock lifecycle_lock{state.lifecycle};
    if (state.closed) {
        return std::unexpected{error::closed};
    }
    if (target.runtime != state.runtime) {
        return std::unexpected{error::not_connected};
    }

    std::size_t live_slot = max_live_connections;
    auto* const record = find_live_record(state.live_slots, target, live_slot);
    if (record == nullptr) {
        return std::unexpected{error::not_connected};
    }
    if (!close_record(*record, target)) {
        return std::unexpected{error::not_connected};
    }
    state.live_slots[live_slot].store(nullptr, std::memory_order_release);
    state.live_count.fetch_sub(1U, std::memory_order_relaxed);
    return {};
}

void device_registry::retire_all() {
    auto& state = *storage_;
    std::scoped_lock lifecycle_lock{state.lifecycle};
    if (state.closed) {
        return;
    }
    state.retire_live_connections();
    static_cast<void>(state.drain_reclaimable());
}

result<read_pin> device_registry::capture(connection_target target) const {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    const auto& state = *storage_;
    if (target.runtime != state.runtime) {
        return std::unexpected{error::not_connected};
    }
    for (const auto& slot : state.live_slots) {
        auto* const record = slot.load(std::memory_order_acquire);
        if (record == nullptr) {
            continue;
        }
        auto pin = read_binding{record, target}.capture();
        if (pin.has_value()) {
            return pin;
        }
        if (pin.error() != error::not_connected) {
            return std::unexpected{pin.error()};
        }
    }
    return std::unexpected{error::not_connected};
}

result<connection_snapshot> device_registry::read(connection_target target) const {
    auto pin = capture(target);
    if (!pin.has_value()) {
        return std::unexpected{pin.error()};
    }
    return pin->read();
}

std::size_t device_registry::drain_retired() {
    auto& state = *storage_;
    std::scoped_lock lifecycle_lock{state.lifecycle};
    return state.drain_reclaimable();
}

void device_registry::shutdown() {
    auto& state = *storage_;
    std::unique_lock lifecycle_lock{state.lifecycle};
    if (state.closed) {
        return;
    }
    state.closed = true;
    state.retire_live_connections();

    for (auto& record : state.records) {
        std::unique_lock record_lock{record.gate};
        if (record.phase == detail::connection_phase::retired && record.pin_count != 0U) {
            record.pins_changed.wait(record_lock, [&record] { return record.pin_count == 0U; });
        }
    }
    static_cast<void>(state.drain_reclaimable());
}

std::size_t device_registry::live_count() const noexcept {
    return storage_->live_count.load(std::memory_order_relaxed);
}

std::size_t device_registry::resident_count() const noexcept {
    return storage_->resident_count.load(std::memory_order_relaxed);
}

} // namespace inputnext
