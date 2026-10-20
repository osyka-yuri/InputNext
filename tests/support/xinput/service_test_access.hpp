#pragma once

#include "xinput/service_state.hpp"
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::xinput::detail {

struct service_test_access final {
    using prepared_read = service::implementation::prepared_state_read;

    [[nodiscard]] static DWORD
    prepare(service& instance, DWORD user_index, bool include_guide, prepared_read& prepared) {
        return instance.implementation_->prepare_state_read(user_index, include_guide, prepared);
    }

    [[nodiscard]] static DWORD
    commit(service& instance, const prepared_read& prepared, XINPUT_STATE* output) {
        return instance.implementation_->commit_state_read(prepared, output);
    }

    [[nodiscard]] static bool seed_packet(service& instance,
        const assignment& expected,
        std::uint32_t packet,
        const XINPUT_GAMEPAD& previous) {
        const auto guard = instance.implementation_->focus.lock_presentation();
        if (expected.user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        auto& position = instance.implementation_->positions[expected.user_index];
        if (!position.occupant.has_value() || *position.occupant != expected) {
            return false;
        }
        position.packet_initialized = true;
        position.packet_number = packet;
        position.canonical_gamepad = previous;
        return true;
    }

    static void seed_event_enable_generation(service& instance, std::uint64_t generation) {
        const auto guard = instance.implementation_->focus.lock_presentation();
        instance.implementation_->event_enable_generation = generation;
        instance.implementation_->event_identity_exhausted = false;
    }

    static void seed_guide_ticket_id(service& instance, std::uint64_t next_id) {
        const auto guard = instance.implementation_->focus.lock_presentation();
        instance.implementation_->next_guide_ticket_id = next_id;
        instance.implementation_->guide_ticket_ids_exhausted = false;
    }

    [[nodiscard]] static std::uint64_t event_enable_generation(const service& instance) {
        const auto guard = instance.implementation_->focus.lock_presentation();
        return instance.implementation_->event_enable_generation;
    }

    [[nodiscard]] static bool guide_awaiter_active(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        const auto& record = instance.implementation_->positions[user_index].events.guide_wait;
        return record.has_value() && record->awaiter_active;
    }

    [[nodiscard]] static std::uint8_t guide_wait_state_value(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return 0xFFU;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        const auto& record = instance.implementation_->positions[user_index].events.guide_wait;
        return record.has_value() ? static_cast<std::uint8_t>(record->state) : 0xFFU;
    }

    [[nodiscard]] static bool guide_wait_pending(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        const auto& record = instance.implementation_->positions[user_index].events.guide_wait;
        return record.has_value() &&
               record->state == service::implementation::guide_wait_state::pending;
    }

    [[nodiscard]] static bool guide_wait_signalled(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        const auto& record = instance.implementation_->positions[user_index].events.guide_wait;
        return record.has_value() &&
               record->state == service::implementation::guide_wait_state::signalled;
    }

    [[nodiscard]] static bool event_operation_reserved(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        return instance.implementation_->positions[user_index].events.operation_reserved;
    }

    [[nodiscard]] static bool event_cleanup_pending(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        return instance.implementation_->positions[user_index].events.cleanup_pending;
    }

    [[nodiscard]] static std::optional<history_reader_handle> event_reader(const service& instance,
        std::uint32_t user_index) {
        if (user_index >= instance.implementation_->positions.size()) {
            return std::nullopt;
        }
        const auto guard = instance.implementation_->focus.lock_presentation();
        return instance.implementation_->positions[user_index].events.reader;
    }
};

} // namespace inputnext::xinput::detail
