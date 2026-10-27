#pragma once

#include "effects.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace inputnext::effects {

enum class context_phase : std::uint8_t {
    free,
    reserved,
    open,
    closed_retained,
    acknowledged_tombstone,
};

struct discrete_marker final {
    bool present{};
    publication_revision publication{};
    output::channel_mask selected_channels{};
    output::mutation_receipt receipt{};
};

struct body_marker final {
    bool present{};
    publication_revision publication{};
    output::body_playback_receipt receipt{};
};

struct context_record final {
    context_phase phase{context_phase::free};
    context_handle external_context{};
    connection_target target{};
    output::public_context native_context{};
    publication_revision close_publication{};
    std::optional<output::body_lease_identity> body_identity = std::nullopt;
    discrete_marker latest_discrete{};
    body_marker latest_body{};
    std::optional<close_receipt> external_close = std::nullopt;
    std::optional<output::close_receipt> native_close = std::nullopt;
    bool native_released{};
    bool cleanup_selected{};
};

struct service::implementation final {
    explicit implementation(service_dependencies supplied);
    ~implementation() noexcept = default;

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    // Record access requires gate; returned pointers remain protected by the caller's lock.
    [[nodiscard]] context_record* find(context_handle context) noexcept;
    [[nodiscard]] result<context_record*> find_open(context_handle context) noexcept;
    [[nodiscard]] bool target_retired(connection_target target) const noexcept;
    [[nodiscard]] std::size_t occupied_contexts() const noexcept;
    [[nodiscard]] std::size_t pending_cleanup() const noexcept;
    static void retain_native_close(context_record& record, output::close_receipt receipt) noexcept;
    static void retain_retired_close(context_record& record) noexcept;
    [[nodiscard]] status close_record(context_record& record) noexcept;
    [[nodiscard]] status release_native(context_record& record) noexcept;

    service_dependencies dependencies;
    const runtime_id identity;
    mutable std::mutex gate;
    bool accepting{true};
    std::array<context_record, context_capacity> contexts{};
};

} // namespace inputnext::effects
