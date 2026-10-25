#pragma once

#include "core/core.hpp"
#include "core/core_state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace inputnext::core::detail {

struct service_test_access final {
    using open_operation = service::implementation::open_operation;
    using read_operation = service::implementation::read_operation;

    static void set_catalogue_revision(service& owner, const std::uint64_t revision) noexcept {
        owner.implementation_->catalogue_revision = revision;
        owner.implementation_->catalogue_published = true;
    }

    [[nodiscard]] static result<open_operation>
    prepare_open(service& owner, const connection_target target, const reader_focus focus) {
        return owner.implementation_->prepare_open(target, focus);
    }

    [[nodiscard]] static result<void> open_native(service& owner, open_operation& operation) {
        return owner.implementation_->open_native(operation);
    }

    [[nodiscard]] static status commit_open(service& owner,
        open_operation& operation,
        const focus::observer::presentation_guard* foreground_guard) {
        return owner.implementation_->commit_open(operation, foreground_guard);
    }

    [[nodiscard]] static std::unique_lock<std::mutex> lock_reader_gate(service& owner) {
        return std::unique_lock<std::mutex>{owner.implementation_->reader_gate};
    }

    [[nodiscard]] static result<read_operation>
    begin_read(service& owner, const reader_handle reader, const std::size_t capacity) {
        return owner.implementation_->begin_read(reader, capacity);
    }

    [[nodiscard]] static status qualify_read(service& owner, read_operation& operation) {
        return owner.implementation_->qualify_read(operation);
    }

    [[nodiscard]] static result<history_batch> obtain_batch(service& owner,
        read_operation& operation) {
        return owner.implementation_->obtain_batch(operation);
    }

    [[nodiscard]] static reader_result commit_read_state(service& owner,
        read_operation& operation,
        const history_batch& batch,
        const focus::observer::presentation_guard* foreground_guard) {
        return owner.implementation_->commit_read_state(operation, batch, foreground_guard);
    }
};

} // namespace inputnext::core::detail
