#pragma once

#include "audio/native_port.hpp"
#include "audio/native_types.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>

namespace inputnext::test::audio {

inline constexpr std::size_t controlled_native_call_capacity = 512U;
inline constexpr std::size_t controlled_native_port_capacity = 32U;
inline constexpr std::uint32_t controlled_native_max_frames = 4800U;
inline constexpr std::size_t controlled_native_sample_capacity =
    controlled_native_max_frames * audio_detail::native_render_channels;

enum class controlled_factory_creation_mode : std::uint8_t {
    normal,
    fail,
    succeed_null,
};

struct controlled_native_call final {
    audio_detail::native_operation operation{audio_detail::native_operation::none};
    std::thread::id thread{};
};

struct controlled_native_port_state final {
    void block_on(const audio_detail::native_operation operation) noexcept {
        std::scoped_lock lock{mutex};
        blocked_operation = operation;
    }

    [[nodiscard]] bool wait_until_blocked(const audio_detail::native_operation operation,
        const std::chrono::milliseconds timeout = std::chrono::seconds{5}) noexcept {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, timeout, [&] {
            return is_blocked && blocked_operation == operation;
        });
    }

    void release_block() noexcept {
        {
            std::scoped_lock lock{mutex};
            is_blocked = false;
            blocked_operation = audio_detail::native_operation::none;
        }
        changed.notify_all();
    }

    void signal_buffer_event() noexcept {
        {
            std::scoped_lock lock{mutex};
            ++pending_buffer_events;
        }
        changed.notify_all();
    }

    void fail_on(const audio_detail::native_operation operation,
        const std::uint32_t native_code = 1U) noexcept {
        std::scoped_lock lock{mutex};
        const auto index = static_cast<std::size_t>(operation);
        if (index < failures.size()) {
            failures[index] =
                audio_detail::native_error{.operation = operation, .native_code = native_code};
        }
    }

    void clear_failure(const audio_detail::native_operation operation) noexcept {
        std::scoped_lock lock{mutex};
        const auto index = static_cast<std::size_t>(operation);
        if (index < failures.size()) {
            failures[index].reset();
        }
    }

    [[nodiscard]] std::size_t call_count() const noexcept {
        std::scoped_lock lock{mutex};
        return stored_call_count;
    }

    [[nodiscard]] std::optional<controlled_native_call> call_at(
        const std::size_t index) const noexcept {
        std::scoped_lock lock{mutex};
        if (index >= stored_call_count || index >= calls.size()) {
            return std::nullopt;
        }
        return calls[index];
    }

    [[nodiscard]] std::size_t count_calls(
        const audio_detail::native_operation operation) const noexcept {
        std::scoped_lock lock{mutex};
        std::size_t count = 0U;
        for (std::size_t index = 0U; index < stored_call_count; ++index) {
            count += calls[index].operation == operation ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] bool all_native_calls_on_worker() const noexcept {
        std::scoped_lock lock{mutex};
        if (worker_thread == std::thread::id{}) {
            return stored_call_count == 0U;
        }
        for (std::size_t index = 0U; index < stored_call_count; ++index) {
            const auto operation = calls[index].operation;
            if (operation == audio_detail::native_operation::wake_worker ||
                operation == audio_detail::native_operation::request_stop) {
                continue;
            }
            if (calls[index].thread != worker_thread) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::thread::id worker_thread_id() const noexcept {
        std::scoped_lock lock{mutex};
        return worker_thread;
    }

    mutable std::mutex mutex;
    std::condition_variable changed{};
    std::array<controlled_native_call, controlled_native_call_capacity> calls{};
    std::array<std::optional<audio_detail::native_error>, 64U> failures{};
    std::array<float, controlled_native_sample_capacity> render_samples{};
    std::size_t stored_call_count{};
    std::size_t pending_buffer_events{};
    std::uint32_t buffer_size{64U};
    std::uint32_t current_padding{};
    audio_detail::native_endpoint_snapshot endpoint{};
    audio_detail::native_audio_format mix_format{
        .encoding = audio_detail::native_sample_encoding::ieee_float,
        .channels = 4U,
        .sample_rate = 48000U,
        .bits_per_sample = 32U,
        .valid_bits_per_sample = 32U,
        .block_align = 16U,
        .average_bytes_per_second = 768000U,
        .channel_mask = 0x33U,
        .extension_size = 22U,
        .extensible = true};
    audio_detail::native_format_support_result format_support{
        .support = audio_detail::native_format_support::exact};
    std::thread::id worker_thread{};
    audio_detail::native_operation blocked_operation{audio_detail::native_operation::none};
    bool is_blocked{};
    bool wake_pending{};
    bool stop_pending{};
    bool render_buffer_acquired{};
    bool start_attempted{};
    bool stop_attempted{};
    bool apartment_attempted{};
    bool apartment_initialized{};
};

class controlled_native_port final : public audio_detail::native_port {
public:
    explicit controlled_native_port(std::shared_ptr<controlled_native_port_state> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] audio_detail::native_notification_result wake_worker() noexcept override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->wake_pending = true;
        }
        state_->changed.notify_all();
        return {.signaled = true};
    }

    [[nodiscard]] audio_detail::native_notification_result request_stop() noexcept override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->stop_pending = true;
        }
        state_->changed.notify_all();
        return {.signaled = true};
    }

    [[nodiscard]] audio_detail::native_result<void> begin_worker_sta() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::begin_worker_sta);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        state_->apartment_attempted = true;
        state_->apartment_initialized = true;
        return {};
    }

    [[nodiscard]] audio_detail::native_result<audio_detail::native_endpoint_snapshot>
    open_exact_endpoint(const audio_detail::fixed_haptic_endpoint_id& id) noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_device);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        if (state_->endpoint.id != id) {
            return std::unexpected{audio_detail::native_error{
                .operation = audio_detail::native_operation::get_endpoint_id,
                .native_code = 2U}};
        }
        return state_->endpoint;
    }

    [[nodiscard]] audio_detail::native_result<audio_detail::native_endpoint_snapshot>
    revalidate_exact_endpoint(const audio_detail::fixed_haptic_endpoint_id& id) noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_endpoint_state);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        if (state_->endpoint.id != id) {
            return std::unexpected{audio_detail::native_error{
                .operation = audio_detail::native_operation::get_endpoint_id,
                .native_code = 2U}};
        }
        return state_->endpoint;
    }

    [[nodiscard]] audio_detail::native_result<void> activate_audio_client() noexcept override {
        return run_void(audio_detail::native_operation::activate_audio_client);
    }

    [[nodiscard]] audio_detail::native_result<audio_detail::native_audio_format>
    get_mix_format() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_mix_format);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        return state_->mix_format;
    }

    [[nodiscard]] audio_detail::native_result<audio_detail::native_format_support_result>
    is_format_supported(const audio_detail::native_audio_format& format) noexcept override {
        static_cast<void>(format);
        if (const auto error = enter(audio_detail::native_operation::is_format_supported);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        return state_->format_support;
    }

    [[nodiscard]] audio_detail::native_result<void> initialize_event_driven_stream(
        const audio_detail::native_audio_format& format) noexcept override {
        static_cast<void>(format);
        return run_void(audio_detail::native_operation::initialize);
    }

    [[nodiscard]] audio_detail::native_result<void> acquire_render_service() noexcept override {
        return run_void(audio_detail::native_operation::get_render_service);
    }

    [[nodiscard]] audio_detail::native_result<std::uint32_t> get_buffer_size() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_buffer_size);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        return state_->buffer_size;
    }

    [[nodiscard]] audio_detail::native_result<void> start_stream() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::start); error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        state_->start_attempted = true;
        return {};
    }

    [[nodiscard]] audio_detail::native_result<void> stop_stream() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::stop); error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        state_->stop_attempted = true;
        return {};
    }

    [[nodiscard]] audio_detail::native_result<std::uint32_t>
    get_current_padding() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_current_padding);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        return state_->current_padding;
    }

    [[nodiscard]] audio_detail::native_result<std::span<float>> acquire_render_buffer(
        const std::uint32_t frame_count) noexcept override {
        if (const auto error = enter(audio_detail::native_operation::get_buffer);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        if (state_->render_buffer_acquired || frame_count == 0U ||
            frame_count > controlled_native_max_frames) {
            return std::unexpected{
                audio_detail::native_error{.operation = audio_detail::native_operation::get_buffer,
                    .native_code = 3U}};
        }
        state_->render_buffer_acquired = true;
        const auto samples =
            static_cast<std::size_t>(frame_count) * audio_detail::native_render_channels;
        return std::span<float>{state_->render_samples.data(), samples};
    }

    [[nodiscard]] audio_detail::native_result<void> release_render_buffer(
        const audio_detail::native_buffer_disposition disposition) noexcept override {
        static_cast<void>(disposition);
        const auto error = enter(audio_detail::native_operation::release_buffer);
        std::scoped_lock lock{state_->mutex};
        if (!state_->render_buffer_acquired) {
            return std::unexpected{audio_detail::native_error{
                .operation = audio_detail::native_operation::release_buffer,
                .native_code = 4U}};
        }
        state_->render_buffer_acquired = false;
        if (error.has_value()) {
            return std::unexpected{*error};
        }
        return {};
    }

    [[nodiscard]] audio_detail::native_wait_result wait_for_activity(
        const std::chrono::milliseconds timeout) noexcept override {
        if (const auto error = enter(audio_detail::native_operation::wait); error.has_value()) {
            return {.outcome = audio_detail::native_wait_outcome::failed, .error = *error};
        }
        std::unique_lock lock{state_->mutex};
        static_cast<void>(state_->changed.wait_for(lock, timeout, [&] {
            return state_->stop_pending || state_->wake_pending ||
                   state_->pending_buffer_events != 0U;
        }));
        if (state_->stop_pending) {
            state_->stop_pending = false;
            return {.outcome = audio_detail::native_wait_outcome::stop_requested};
        }
        if (state_->wake_pending) {
            state_->wake_pending = false;
            return {.outcome = audio_detail::native_wait_outcome::worker_wakeup};
        }
        if (state_->pending_buffer_events != 0U) {
            --state_->pending_buffer_events;
            return {.outcome = audio_detail::native_wait_outcome::buffer_ready};
        }
        return {.outcome = audio_detail::native_wait_outcome::timeout};
    }

    [[nodiscard]] audio_detail::native_mmcss_registration register_mmcss_audio() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::register_mmcss);
            error.has_value()) {
            return {.registered = false, .native_code = error->native_code};
        }
        return {.registered = true};
    }

    [[nodiscard]] audio_detail::native_result<audio_detail::native_cleanup_report>
    finish_worker_sta() noexcept override {
        if (const auto error = enter(audio_detail::native_operation::finish_worker_sta);
            error.has_value()) {
            return std::unexpected{*error};
        }
        std::scoped_lock lock{state_->mutex};
        if (state_->render_buffer_acquired) {
            return std::unexpected{audio_detail::native_error{
                .operation = audio_detail::native_operation::finish_worker_sta,
                .native_code = 5U}};
        }
        audio_detail::native_cleanup_report report{};
        if (state_->start_attempted && !state_->stop_attempted) {
            report.stop = {.attempted = true, .succeeded = true, .native_code = 0U};
            state_->stop_attempted = true;
        }
        report.buffer_event_close = {.attempted = true, .succeeded = true, .native_code = 0U};
        report.mmcss_revert = {.attempted = true, .succeeded = true, .native_code = 0U};
        state_->apartment_initialized = false;
        return report;
    }

private:
    [[nodiscard]] std::optional<audio_detail::native_error> enter(
        const audio_detail::native_operation operation) noexcept {
        std::unique_lock lock{state_->mutex};
        if (operation != audio_detail::native_operation::wake_worker &&
            operation != audio_detail::native_operation::request_stop &&
            state_->worker_thread == std::thread::id{}) {
            state_->worker_thread = std::this_thread::get_id();
        }
        if (state_->stored_call_count < state_->calls.size()) {
            state_->calls[state_->stored_call_count++] = {.operation = operation,
                .thread = std::this_thread::get_id()};
        }
        const auto index = static_cast<std::size_t>(operation);
        if (state_->blocked_operation == operation) {
            state_->is_blocked = true;
            state_->changed.notify_all();
            state_->changed.wait(lock, [&] { return !state_->is_blocked; });
        }
        return index < state_->failures.size() ? state_->failures[index] : std::nullopt;
    }

    [[nodiscard]] audio_detail::native_result<void> run_void(
        const audio_detail::native_operation operation) noexcept {
        if (const auto error = enter(operation); error.has_value()) {
            return std::unexpected{*error};
        }
        return {};
    }

    std::shared_ptr<controlled_native_port_state> state_;
};

class controlled_native_port_factory final : public audio_detail::native_port_factory {
public:
    void set_creation_mode(const controlled_factory_creation_mode mode,
        const std::uint32_t failure_code = 1U) noexcept {
        std::scoped_lock lock{mutex_};
        creation_mode_ = mode;
        failure_code_ = failure_code;
    }

    void clear_creation_mode() noexcept {
        set_creation_mode(controlled_factory_creation_mode::normal);
    }

    [[nodiscard]] std::size_t create_attempt_count() const noexcept {
        std::scoped_lock lock{mutex_};
        return create_attempts_;
    }

    [[nodiscard]] std::shared_ptr<controlled_native_port_state> state_for(
        const std::size_t index) noexcept {
        std::scoped_lock lock{mutex_};
        if (index >= states_.size()) {
            return {};
        }
        if (states_[index] == nullptr) {
            try {
                states_[index] = std::make_shared<controlled_native_port_state>();
            } catch (...) {
                return {};
            }
        }
        return states_[index];
    }

    [[nodiscard]] std::size_t created_count() const noexcept {
        std::scoped_lock lock{mutex_};
        return next_;
    }

    [[nodiscard]] audio_detail::native_result<std::unique_ptr<audio_detail::native_port>>
    create() noexcept override {
        std::shared_ptr<controlled_native_port_state> state;
        {
            std::scoped_lock lock{mutex_};
            ++create_attempts_;
            if (creation_mode_ == controlled_factory_creation_mode::fail) {
                return std::unexpected{audio_detail::native_error{
                    .operation = audio_detail::native_operation::create_native_port,
                    .native_code = failure_code_}};
            }
            if (creation_mode_ == controlled_factory_creation_mode::succeed_null) {
                return std::unique_ptr<audio_detail::native_port>{};
            }
            if (next_ >= states_.size()) {
                return std::unexpected{audio_detail::native_error{
                    .operation = audio_detail::native_operation::create_native_port,
                    .native_code = 6U}};
            }
            if (states_[next_] == nullptr) {
                try {
                    states_[next_] = std::make_shared<controlled_native_port_state>();
                } catch (...) {
                    return std::unexpected{audio_detail::native_error{
                        .operation = audio_detail::native_operation::create_native_port,
                        .native_code = 7U}};
                }
            }
            state = states_[next_++];
        }
        auto port = std::unique_ptr<audio_detail::native_port>{
            new (std::nothrow) controlled_native_port{std::move(state)}};
        if (port == nullptr) {
            return std::unexpected{audio_detail::native_error{
                .operation = audio_detail::native_operation::create_native_port,
                .native_code = 7U}};
        }
        return port;
    }

private:
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<controlled_native_port_state>, controlled_native_port_capacity>
        states_{};
    std::size_t next_{};
    std::size_t create_attempts_{};
    controlled_factory_creation_mode creation_mode_{controlled_factory_creation_mode::normal};
    std::uint32_t failure_code_{1U};
};

} // namespace inputnext::test::audio
