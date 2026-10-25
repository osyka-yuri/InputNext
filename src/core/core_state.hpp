#pragma once

#include "core.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace inputnext::core {

struct service::implementation final {
    struct reader_entry final {
        history_reader_handle native{};
        reader_focus focus{reader_focus::foreground_process};
        std::optional<history_batch> pending{std::nullopt};
        std::uint64_t stream{};
        status terminal{status::ok};
        bool occupied{};
        bool open_inflight{};
        bool read_inflight{};
        bool close_inflight{};
        bool closing{};
        bool close_completed{};
    };

    struct open_operation final {
        connection_target target{};
        reader_focus focus{reader_focus::foreground_process};
        std::optional<focus::foreground_epoch> epoch{std::nullopt};
        std::size_t slot{};
        history_reader_handle native{};
    };

    struct read_operation final {
        reader_handle handle{};
        std::size_t index{};
        std::size_t capacity{};
        connection_target target{};
        history_reader_handle native{};
        reader_focus focus{reader_focus::foreground_process};
        std::optional<history_batch> pending{std::nullopt};
        std::uint64_t stream{};
        std::optional<focus::foreground_epoch> epoch{std::nullopt};
    };

    implementation(runtime_id runtime_identity,
        std::uint64_t frequency,
        device_registry& registry_owner,
        gameinput_runtime& gameinput_owner,
        focus::observer& focus_owner,
        xinput::service& presentation_owner) noexcept;

    [[nodiscard]] result<device_catalogue> enumerate();
    [[nodiscard]] result<connection_target> resolve(const selector& selection);
    [[nodiscard]] result<connection_target> resolve_slot(const selector& selection) const;
    [[nodiscard]] static result<connection_target> resolve_exact_target(const selector& selection);
    [[nodiscard]] result<connection_target> resolve_fingerprint(const selector& selection);
    [[nodiscard]] result<connection_snapshot> read_latest(connection_target target) const;
    [[nodiscard]] result<reader_handle> open_reader(connection_target target, reader_focus mode);
    [[nodiscard]] reader_result read_events(reader_handle handle, std::size_t capacity);
    [[nodiscard]] status close_reader(reader_handle handle);
    [[nodiscard]] result<open_operation> prepare_open(connection_target target, reader_focus mode);
    [[nodiscard]] result<void> open_native(open_operation& operation);
    [[nodiscard]] status commit_open(open_operation& operation,
        const focus::observer::presentation_guard* foreground_guard);
    [[nodiscard]] result<read_operation> begin_read(reader_handle handle, std::size_t capacity);
    [[nodiscard]] status qualify_read(read_operation& operation) const;
    [[nodiscard]] result<history_batch> obtain_batch(read_operation& operation);
    [[nodiscard]] result<read_pin> capture_read_target(const read_operation& operation) const;
    [[nodiscard]] reader_result finish_read_without_delivery(const read_operation& operation,
        status outcome,
        bool retire_source);
    [[nodiscard]] reader_result commit_read(read_operation& operation, history_batch batch);
    [[nodiscard]] reader_result commit_read_state(read_operation& operation,
        history_batch batch,
        const focus::observer::presentation_guard* foreground_guard);
    [[nodiscard]] result<device_catalogue> publish_catalogue(device_catalogue candidate) noexcept;
    void close_admission() noexcept;
    [[nodiscard]] std::optional<std::size_t> find_reader_locked(std::uint64_t id) const noexcept;
    static void release_reader_if_drained_locked(reader_entry& entry) noexcept;
    [[nodiscard]] status close_native_reader(std::size_t index);
    void retry_deferred_closes();

    runtime_id identity{};
    std::uint64_t qpc_frequency{};
    device_registry& registry;
    gameinput_runtime& runtime;
    focus::observer& focus_observer;
    xinput::service& presentation;

    std::atomic<bool> admission_closed{false};

    std::mutex catalogue_gate;
    device_catalogue catalogue{};
    std::uint64_t catalogue_revision{};
    bool catalogue_published{};
    bool catalogue_exhausted{};

    std::mutex reader_gate;
    std::array<reader_entry, reader_capacity> readers{};
};

} // namespace inputnext::core
