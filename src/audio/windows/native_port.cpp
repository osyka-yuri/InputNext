#include "audio/native_port.hpp"
#include "audio/native_types.hpp"
#include "endpoint_snapshot.hpp"
#include "wave_format.hpp"

#include <inputnext/device_metadata.hpp>

#include <Windows.h>

#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace inputnext::audio_detail {
namespace {

static_assert(sizeof(wchar_t) == sizeof(char16_t));

[[nodiscard]] std::uint32_t hresult_code(const HRESULT value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t win32_code(const DWORD value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] native_error unexpected_thread_error(const native_operation operation) noexcept {
    return {.operation = operation, .native_code = hresult_code(RPC_E_WRONG_THREAD)};
}

[[nodiscard]] native_error unexpected_state_error(const native_operation operation) noexcept {
    return {.operation = operation, .native_code = hresult_code(E_UNEXPECTED)};
}

class windows_native_port final : public native_port {
public:
    windows_native_port() noexcept = default;
    windows_native_port(const windows_native_port&) = delete;
    windows_native_port& operator=(const windows_native_port&) = delete;
    windows_native_port(windows_native_port&&) = delete;
    windows_native_port& operator=(windows_native_port&&) = delete;

    ~windows_native_port() noexcept override {
        close_handle(buffer_event_);
        close_handle(stop_event_);
        close_handle(wake_event_);
    }

    [[nodiscard]] native_result<void> create_events() noexcept {
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_event_ == nullptr) {
            return std::unexpected(native_error{.operation = native_operation::create_wake_event,
                .native_code = win32_code(GetLastError())});
        }

        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event_ == nullptr) {
            return std::unexpected(native_error{.operation = native_operation::create_stop_event,
                .native_code = win32_code(GetLastError())});
        }

        buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (buffer_event_ == nullptr) {
            return std::unexpected(native_error{.operation = native_operation::create_buffer_event,
                .native_code = win32_code(GetLastError())});
        }
        return {};
    }

    [[nodiscard]] native_notification_result wake_worker() noexcept override {
        return signal(wake_event_, native_operation::wake_worker);
    }

    [[nodiscard]] native_notification_result request_stop() noexcept override {
        return signal(stop_event_, native_operation::request_stop);
    }

    [[nodiscard]] native_result<void> begin_worker_sta() noexcept override {
        if (worker_thread_id_ != 0U || finished_) {
            return std::unexpected(unexpected_state_error(native_operation::begin_worker_sta));
        }

        worker_thread_id_ = GetCurrentThreadId();
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::begin_worker_sta,
                .native_code = hresult_code(result)});
        }
        apartment_initialized_ = true;
        return {};
    }

    [[nodiscard]] native_result<native_endpoint_snapshot> open_exact_endpoint(
        const fixed_haptic_endpoint_id& id) noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_device); !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!apartment_initialized_ || enumerator_ != nullptr || device_ != nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_device));
        }

        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator_));
        if (FAILED(result)) {
            return std::unexpected(
                native_error{.operation = native_operation::create_device_enumerator,
                    .native_code = hresult_code(result)});
        }
        if (enumerator_ == nullptr) {
            return std::unexpected(
                unexpected_state_error(native_operation::create_device_enumerator));
        }

        std::array<wchar_t, haptic_endpoint_buffer_code_units> native_id{};
        for (std::size_t index = 0U; index < id.size(); ++index) {
            native_id[index] = static_cast<wchar_t>(id.terminated_data()[index]);
        }
        result = enumerator_->GetDevice(native_id.data(), &device_);
        if (FAILED(result)) {
            if (device_ != nullptr) {
                device_->Release();
                device_ = nullptr;
            }
            return std::unexpected(native_error{.operation = native_operation::get_device,
                .native_code = hresult_code(result)});
        }
        if (device_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_device));
        }

        return windows::inspect_endpoint(*device_);
    }

    [[nodiscard]] native_result<native_endpoint_snapshot> revalidate_exact_endpoint(
        const fixed_haptic_endpoint_id& id) noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_endpoint_id);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!apartment_initialized_ || enumerator_ == nullptr || device_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_endpoint_id));
        }

        auto snapshot = windows::inspect_endpoint(*device_);
        if (!snapshot.has_value()) {
            return std::unexpected(snapshot.error());
        }
        if (snapshot->id != id) {
            return std::unexpected(native_error{.operation = native_operation::get_endpoint_id,
                .native_code = hresult_code(HRESULT_FROM_WIN32(ERROR_NOT_FOUND))});
        }
        return snapshot;
    }

    [[nodiscard]] native_result<void> activate_audio_client() noexcept override {
        if (const auto guarded = guard_worker(native_operation::activate_audio_client);
            !guarded.has_value()) {
            return guarded;
        }
        if (!apartment_initialized_ || device_ == nullptr || audio_client_ != nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::activate_audio_client));
        }

        const HRESULT result = device_->Activate(__uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audio_client_));
        if (FAILED(result)) {
            if (audio_client_ != nullptr) {
                audio_client_->Release();
                audio_client_ = nullptr;
            }
            return std::unexpected(
                native_error{.operation = native_operation::activate_audio_client,
                    .native_code = hresult_code(result)});
        }
        if (audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::activate_audio_client));
        }
        return {};
    }

    [[nodiscard]] native_result<native_audio_format> get_mix_format() noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_mix_format);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!apartment_initialized_ || audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_mix_format));
        }

        WAVEFORMATEX* raw_format = nullptr;
        const HRESULT result = audio_client_->GetMixFormat(&raw_format);
        if (FAILED(result)) {
            if (raw_format != nullptr) {
                CoTaskMemFree(raw_format);
            }
            return std::unexpected(native_error{.operation = native_operation::get_mix_format,
                .native_code = hresult_code(result)});
        }
        if (raw_format == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_mix_format));
        }
        const native_audio_format format = windows::describe_wave_format(*raw_format);
        CoTaskMemFree(raw_format);
        return format;
    }

    [[nodiscard]] native_result<native_format_support_result> is_format_supported(
        const native_audio_format& format) noexcept override {
        if (const auto guarded = guard_worker(native_operation::is_format_supported);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!apartment_initialized_ || audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::is_format_supported));
        }
        const auto requested =
            windows::make_wave_format(format, native_operation::is_format_supported);
        if (!requested.has_value()) {
            return std::unexpected(requested.error());
        }

        WAVEFORMATEX* closest = nullptr;
        const HRESULT result = audio_client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
            &requested->Format,
            &closest);
        if (result == S_OK) {
            if (closest != nullptr) {
                CoTaskMemFree(closest);
            }
            return native_format_support_result{.support = native_format_support::exact};
        }
        if (result == S_FALSE) {
            if (closest == nullptr) {
                return std::unexpected(
                    unexpected_state_error(native_operation::is_format_supported));
            }
            const native_audio_format closest_format = windows::describe_wave_format(*closest);
            CoTaskMemFree(closest);
            return native_format_support_result{.support = native_format_support::closest,
                .closest_format = closest_format};
        }
        if (closest != nullptr) {
            CoTaskMemFree(closest);
        }
        return std::unexpected(native_error{.operation = native_operation::is_format_supported,
            .native_code = hresult_code(result)});
    }

    [[nodiscard]] native_result<void> initialize_event_driven_stream(
        const native_audio_format& format) noexcept override {
        if (const auto guarded = guard_worker(native_operation::initialize); !guarded.has_value()) {
            return guarded;
        }
        if (!apartment_initialized_ || audio_client_ == nullptr || buffer_event_ == nullptr ||
            initialized_) {
            return std::unexpected(unexpected_state_error(native_operation::initialize));
        }
        const auto native_format = windows::make_wave_format(format, native_operation::initialize);
        if (!native_format.has_value()) {
            return std::unexpected(native_format.error());
        }

        const HRESULT result = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0,
            0,
            &native_format->Format,
            nullptr);
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::initialize,
                .native_code = hresult_code(result)});
        }
        initialized_ = true;

        const HRESULT event_result = audio_client_->SetEventHandle(buffer_event_);
        if (FAILED(event_result)) {
            return std::unexpected(native_error{.operation = native_operation::set_event_handle,
                .native_code = hresult_code(event_result)});
        }
        event_bound_ = true;
        return {};
    }

    [[nodiscard]] native_result<void> acquire_render_service() noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_render_service);
            !guarded.has_value()) {
            return guarded;
        }
        if (!apartment_initialized_ || !initialized_ || !event_bound_ || audio_client_ == nullptr ||
            render_client_ != nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_render_service));
        }

        const HRESULT result = audio_client_->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&render_client_));
        if (FAILED(result)) {
            if (render_client_ != nullptr) {
                render_client_->Release();
                render_client_ = nullptr;
            }
            return std::unexpected(native_error{.operation = native_operation::get_render_service,
                .native_code = hresult_code(result)});
        }
        if (render_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_render_service));
        }
        return {};
    }

    [[nodiscard]] native_result<std::uint32_t> get_buffer_size() noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_buffer_size);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!initialized_ || audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_buffer_size));
        }

        UINT32 frames = 0U;
        const HRESULT result = audio_client_->GetBufferSize(&frames);
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::get_buffer_size,
                .native_code = hresult_code(result)});
        }
        return frames;
    }

    [[nodiscard]] native_result<void> start_stream() noexcept override {
        if (const auto guarded = guard_worker(native_operation::start); !guarded.has_value()) {
            return guarded;
        }
        if (!initialized_ || !event_bound_ || render_client_ == nullptr ||
            audio_client_ == nullptr || start_attempted_) {
            return std::unexpected(unexpected_state_error(native_operation::start));
        }

        start_attempted_ = true;
        const HRESULT result = audio_client_->Start();
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::start,
                .native_code = hresult_code(result)});
        }
        return {};
    }

    [[nodiscard]] native_result<void> stop_stream() noexcept override {
        if (const auto guarded = guard_worker(native_operation::stop); !guarded.has_value()) {
            return guarded;
        }
        if (!start_attempted_ || stop_attempted_ || audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::stop));
        }
        stop_attempted_ = true;
        const HRESULT result = audio_client_->Stop();
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::stop,
                .native_code = hresult_code(result)});
        }
        return {};
    }

    [[nodiscard]] native_result<std::uint32_t> get_current_padding() noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_current_padding);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!start_attempted_ || audio_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::get_current_padding));
        }

        UINT32 frames = 0U;
        const HRESULT result = audio_client_->GetCurrentPadding(&frames);
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::get_current_padding,
                .native_code = hresult_code(result)});
        }
        return frames;
    }

    [[nodiscard]] native_result<std::span<float>> acquire_render_buffer(
        const std::uint32_t frame_count) noexcept override {
        if (const auto guarded = guard_worker(native_operation::get_buffer); !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (!initialized_ || !event_bound_ || render_client_ == nullptr || buffer_acquired_ ||
            frame_count == 0U ||
            static_cast<std::size_t>(frame_count) >
                std::numeric_limits<std::size_t>::max() / native_render_channels) {
            return std::unexpected(native_error{.operation = native_operation::get_buffer,
                .native_code = hresult_code(E_INVALIDARG)});
        }

        BYTE* data = nullptr;
        const HRESULT result = render_client_->GetBuffer(frame_count, &data);
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::get_buffer,
                .native_code = hresult_code(result)});
        }
        buffer_acquired_ = true;
        acquired_frame_count_ = frame_count;
        if (data == nullptr) {
            const HRESULT release_result =
                render_client_->ReleaseBuffer(frame_count, AUDCLNT_BUFFERFLAGS_SILENT);
            buffer_acquired_ = false;
            acquired_frame_count_ = 0U;
            if (FAILED(release_result)) {
                return std::unexpected(native_error{.operation = native_operation::release_buffer,
                    .native_code = hresult_code(release_result)});
            }
            return std::unexpected(native_error{.operation = native_operation::get_buffer,
                .native_code = hresult_code(E_POINTER)});
        }

        const auto sample_count = static_cast<std::size_t>(frame_count) * native_render_channels;
        return std::span<float>{reinterpret_cast<float*>(data), sample_count};
    }

    [[nodiscard]] native_result<void> release_render_buffer(
        const native_buffer_disposition disposition) noexcept override {
        if (const auto guarded = guard_worker(native_operation::release_buffer);
            !guarded.has_value()) {
            return guarded;
        }
        if (!buffer_acquired_ || render_client_ == nullptr) {
            return std::unexpected(unexpected_state_error(native_operation::release_buffer));
        }

        const DWORD flags =
            disposition == native_buffer_disposition::silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0U;
        const HRESULT result = render_client_->ReleaseBuffer(acquired_frame_count_, flags);
        buffer_acquired_ = false;
        acquired_frame_count_ = 0U;
        if (FAILED(result)) {
            return std::unexpected(native_error{.operation = native_operation::release_buffer,
                .native_code = hresult_code(result)});
        }
        return {};
    }

    [[nodiscard]] native_wait_result wait_for_activity(
        const std::chrono::milliseconds timeout) noexcept override {
        if (const auto guarded = guard_worker(native_operation::wait); !guarded.has_value()) {
            return {.outcome = native_wait_outcome::failed, .error = guarded.error()};
        }
        if (!apartment_initialized_ || buffer_event_ == nullptr || buffer_acquired_ ||
            timeout.count() < 0) {
            return {.outcome = native_wait_outcome::failed,
                .error = native_error{.operation = native_operation::wait,
                    .native_code = hresult_code(E_INVALIDARG)}};
        }

        const std::array<HANDLE, 3U> events{stop_event_, wake_event_, buffer_event_};
        const auto capped_timeout = std::min<std::int64_t>(timeout.count(), MAXDWORD - 1LL);
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(capped_timeout);
        auto remaining = static_cast<DWORD>(capped_timeout);
        for (;;) {
            const DWORD result = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(events.size()),
                events.data(),
                remaining,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) {
                return {.outcome = native_wait_outcome::stop_requested};
            }
            if (result == WAIT_OBJECT_0 + 1U) {
                return {.outcome = native_wait_outcome::worker_wakeup};
            }
            if (result == WAIT_OBJECT_0 + 2U) {
                return {.outcome = native_wait_outcome::buffer_ready};
            }
            if (result == WAIT_TIMEOUT) {
                return {.outcome = native_wait_outcome::timeout};
            }
            if (result == WAIT_FAILED) {
                return {.outcome = native_wait_outcome::failed,
                    .error = native_error{.operation = native_operation::wait,
                        .native_code = win32_code(GetLastError())}};
            }
            if (result != WAIT_OBJECT_0 + static_cast<DWORD>(events.size())) {
                return {.outcome = native_wait_outcome::failed,
                    .error = unexpected_state_error(native_operation::wait)};
            }

            MSG message{};
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    return {.outcome = native_wait_outcome::failed,
                        .error = native_error{.operation = native_operation::wait,
                            .native_code = win32_code(ERROR_CANCELLED)}};
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return {.outcome = native_wait_outcome::timeout};
            }
            remaining = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD - 1ULL));
        }
    }

    [[nodiscard]] native_mmcss_registration register_mmcss_audio() noexcept override {
        if (const auto guarded = guard_worker(native_operation::register_mmcss);
            !guarded.has_value()) {
            return {.registered = false, .native_code = guarded.error().native_code};
        }
        if (!apartment_initialized_) {
            return {.registered = false, .native_code = hresult_code(E_UNEXPECTED)};
        }
        if (mmcss_handle_ != nullptr) {
            return {.registered = true, .native_code = 0U};
        }

        DWORD task_index = 0U;
        mmcss_handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        if (mmcss_handle_ == nullptr) {
            return {.registered = false, .native_code = win32_code(GetLastError())};
        }
        return {.registered = true, .native_code = 0U};
    }

    [[nodiscard]] native_result<native_cleanup_report> finish_worker_sta() noexcept override {
        if (const auto guarded = guard_worker(native_operation::finish_worker_sta);
            !guarded.has_value()) {
            return std::unexpected(guarded.error());
        }
        if (buffer_acquired_) {
            return std::unexpected(unexpected_state_error(native_operation::finish_worker_sta));
        }

        native_cleanup_report report{};
        if (start_attempted_ && !stop_attempted_) {
            report.stop.attempted = true;
            stop_attempted_ = true;
            const HRESULT stop_result =
                audio_client_ == nullptr ? E_POINTER : audio_client_->Stop();
            report.stop.succeeded = SUCCEEDED(stop_result);
            report.stop.native_code = hresult_code(stop_result);
        }

        release_com(render_client_);
        release_com(audio_client_);
        release_com(device_);
        release_com(enumerator_);

        if (buffer_event_ != nullptr) {
            report.buffer_event_close.attempted = true;
            const BOOL close_result = CloseHandle(buffer_event_);
            report.buffer_event_close.succeeded = close_result != FALSE;
            report.buffer_event_close.native_code =
                close_result != FALSE ? 0U : win32_code(GetLastError());
            if (close_result != FALSE) {
                buffer_event_ = nullptr;
            }
        }

        if (mmcss_handle_ != nullptr) {
            report.mmcss_revert.attempted = true;
            const BOOL revert_result = AvRevertMmThreadCharacteristics(mmcss_handle_);
            report.mmcss_revert.succeeded = revert_result != FALSE;
            report.mmcss_revert.native_code =
                revert_result != FALSE ? 0U : win32_code(GetLastError());
            mmcss_handle_ = nullptr;
        }

        if (apartment_initialized_) {
            CoUninitialize();
            apartment_initialized_ = false;
        }
        finished_ = true;
        return report;
    }

private:
    [[nodiscard]] static native_notification_result signal(const HANDLE event,
        const native_operation operation) noexcept {
        if (event == nullptr) {
            return {.signaled = false,
                .error = native_error{.operation = operation,
                    .native_code = win32_code(ERROR_INVALID_HANDLE)}};
        }
        if (SetEvent(event) == FALSE) {
            return {.signaled = false,
                .error = native_error{.operation = operation,
                    .native_code = win32_code(GetLastError())}};
        }
        return {.signaled = true};
    }

    [[nodiscard]] native_result<void> guard_worker(
        const native_operation operation) const noexcept {
        if (worker_thread_id_ == 0U || GetCurrentThreadId() != worker_thread_id_) {
            return std::unexpected(unexpected_thread_error(operation));
        }
        if (finished_) {
            return std::unexpected(unexpected_state_error(operation));
        }
        return {};
    }

    template <typename T>
    static void release_com(T*& value) noexcept {
        if (value != nullptr) {
            value->Release();
            value = nullptr;
        }
    }

    static void close_handle(HANDLE& value) noexcept {
        if (value != nullptr) {
            static_cast<void>(CloseHandle(value));
            value = nullptr;
        }
    }

    HANDLE wake_event_{};
    HANDLE stop_event_{};
    HANDLE buffer_event_{};
    DWORD worker_thread_id_{};
    IMMDeviceEnumerator* enumerator_{};
    IMMDevice* device_{};
    IAudioClient* audio_client_{};
    IAudioRenderClient* render_client_{};
    HANDLE mmcss_handle_{};
    std::uint32_t acquired_frame_count_{};
    bool apartment_initialized_{};
    bool initialized_{};
    bool event_bound_{};
    bool start_attempted_{};
    bool stop_attempted_{};
    bool buffer_acquired_{};
    bool finished_{};
};

class windows_native_port_factory final : public native_port_factory {
public:
    [[nodiscard]] native_result<std::unique_ptr<native_port>> create() noexcept override {
        auto* const raw_port = new (std::nothrow) windows_native_port();
        if (raw_port == nullptr) {
            return std::unexpected(native_error{.operation = native_operation::create_native_port,
                .native_code = win32_code(ERROR_NOT_ENOUGH_MEMORY)});
        }
        std::unique_ptr<native_port> port{raw_port};
        if (auto created = raw_port->create_events(); !created.has_value()) {
            return std::unexpected(created.error());
        }
        return std::move(port);
    }
};

} // namespace

std::unique_ptr<native_port_factory> make_windows_native_port_factory() noexcept {
    return std::unique_ptr<native_port_factory>{new (std::nothrow) windows_native_port_factory()};
}

} // namespace inputnext::audio_detail
