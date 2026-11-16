#include "ownership.hpp"
#include "lease.hpp"
#include "lease_state.hpp"

#include "core/core.hpp"
#include "dinput/catalogue/devices.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/property_abi.h"
#include "dinput/system_input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <inputnext/focus.hpp>

namespace inputnext::dinput {
namespace {

inline constexpr std::size_t actor_capacity = 16U;
inline constexpr DWORD shutdown_warning_ms = 5'000U;
using std::chrono::steady_clock;
inline constexpr auto probe_period = std::chrono::milliseconds{25};

[[nodiscard]] bool background(const exclusive_request& request) noexcept {
    return request.background_window.handle != nullptr;
}

[[nodiscard]] bool background_window_valid(const background_exclusive_window& window) noexcept {
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(window.handle, &process);
    return thread != 0U && thread == window.owner_thread && process == GetCurrentProcessId() &&
           GetAncestor(window.handle, GA_ROOT) == window.handle;
}

void join_worker_for_shutdown(std::thread& worker) noexcept {
    if (!worker.joinable()) {
        return;
    }
    const HANDLE handle = worker.native_handle();
    if (WaitForSingleObject(handle, shutdown_warning_ms) == WAIT_TIMEOUT) {
        std::array<char, 160U> message{};
        const auto length = std::snprintf(message.data(),
            message.size(),
            "InputNext: shutdown is still waiting for DirectInput exclusive worker %lu "
            "after %lu ms.\n",
            GetThreadId(handle),
            shutdown_warning_ms);
        if (length > 0) {
            OutputDebugStringA(message.data());
        }
    }
    worker.join();
}

[[nodiscard]] HRESULT last_error_result() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

[[nodiscard]] std::optional<std::size_t> bounded_path_length(const wchar_t* value) noexcept;

[[nodiscard]] bool same_path(const wchar_t* left, const wchar_t* right) noexcept {
    const auto left_length = bounded_path_length(left);
    const auto right_length = bounded_path_length(right);
    if (!left_length.has_value() || !right_length.has_value() ||
        *left_length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        *right_length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left,
               static_cast<int>(*left_length),
               right,
               static_cast<int>(*right_length),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::optional<std::size_t> bounded_path_length(const wchar_t* value) noexcept {
    constexpr std::size_t capacity = std::size(DIPROPGUIDANDPATH{}.wszPath);
    for (std::size_t index = 0U; index < capacity; ++index) {
        if (value[index] == L'\0') {
            return index == 0U ? std::nullopt : std::optional<std::size_t>{index};
        }
    }
    return std::nullopt;
}

[[nodiscard]] HRESULT validate_request(const exclusive_request& request) noexcept {
    const bool window_valid =
        background(request)
            ? request.window == focus::window_epoch{} &&
                  request.background_window.owner_thread != 0U
            : request.background_window.owner_thread == 0U && request.window.window != 0U &&
                  request.window.revision != 0U && request.window.process.revision != 0U &&
                  request.window.process.observer_incarnation.valid() &&
                  request.window.process.process.pid != 0U;
    if (!request.target.valid() || IsEqualGUID(request.native_instance, GUID_NULL) != 0 ||
        !window_valid) {
        return DIERR_INPUTLOST;
    }
    const bool path_valid =
        request.native_path.diph.dwSize == static_cast<DWORD>(sizeof(DIPROPGUIDANDPATH)) &&
        request.native_path.diph.dwHeaderSize == static_cast<DWORD>(sizeof(DIPROPHEADER)) &&
        request.native_path.diph.dwHow == DIPH_DEVICE && request.native_path.diph.dwObj == 0U &&
        bounded_path_length(request.native_path.wszPath).has_value();
    return path_valid && request.factory.supported() ? DI_OK : DIERR_UNSUPPORTED;
}

[[nodiscard]] bool same_identity(const exclusive_request& left,
    const exclusive_request& right) noexcept {
    return IsEqualGUID(left.native_instance, right.native_instance) != 0 ||
           same_path(left.native_path.wszPath, right.native_path.wszPath);
}

[[nodiscard]] HRESULT read_opened_path(IDirectInputDevice8W& device,
    const DIPROPGUIDANDPATH& expected) noexcept {
    DIPROPGUIDANDPATH actual{};
    actual.diph.dwSize = static_cast<DWORD>(sizeof(actual));
    actual.diph.dwHeaderSize = static_cast<DWORD>(sizeof(actual.diph));
    actual.diph.dwHow = DIPH_DEVICE;
    const HRESULT status =
        inputnext_dinput_get_property_w(&device, &DIPROP_GUIDANDPATH, &actual.diph);
    if (FAILED(status)) {
        return status;
    }
    const auto length = bounded_path_length(actual.wszPath);
    if (actual.diph.dwSize != static_cast<DWORD>(sizeof(actual)) ||
        actual.diph.dwHeaderSize != static_cast<DWORD>(sizeof(DIPROPHEADER)) ||
        actual.diph.dwHow != DIPH_DEVICE || actual.diph.dwObj != 0U || !length.has_value() ||
        IsEqualGUID(actual.guidClass, expected.guidClass) == 0 ||
        !same_path(actual.wszPath, expected.wszPath)) {
        return DIERR_INPUTLOST;
    }
    return DI_OK;
}

[[nodiscard]] bool pump_actor_messages() noexcept {
    MSG message{};
    // Recheck cancellation and deadlines even when messages arrive continuously.
    for (unsigned count = 0U;
        count < 32U && PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE;
        ++count) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

[[nodiscard]] DWORD actor_wait(std::span<const HANDLE> events,
    const std::optional<steady_clock::time_point> deadline = std::nullopt) noexcept {
    const auto count = static_cast<DWORD>(events.size());
    for (;;) {
        DWORD timeout = INFINITE;
        if (deadline.has_value()) {
            const auto remaining = *deadline - steady_clock::now();
            if (remaining <= steady_clock::duration::zero()) {
                return WAIT_TIMEOUT;
            }
            timeout =
                static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(count,
            events.data(),
            timeout,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) {
            return WAIT_FAILED;
        }
        if (result < WAIT_OBJECT_0 + count) {
            return result;
        }
        if (result == WAIT_OBJECT_0 + count) {
            if (!pump_actor_messages()) {
                return WAIT_FAILED;
            }
            continue;
        }
        if (result == WAIT_TIMEOUT && deadline.has_value()) {
            continue;
        }
        return WAIT_FAILED;
    }
}

[[nodiscard]] bool request_eligible(const input_access::call& call,
    const connection_target target,
    const focus::window_epoch& window) noexcept {
    if (!call) {
        return false;
    }
    const auto latest = call.core().read_latest(target);
    if (!latest.has_value() && latest.error() != core::status::no_data) {
        return false;
    }
    if (latest.has_value() && latest->target != target) {
        return false;
    }
    if (window == focus::window_epoch{}) {
        return true;
    }
    const auto presentation = call.focus().lock_presentation();
    return presentation.matches_foreground_window_epoch(window);
}

[[nodiscard]] bool request_eligible(const input_access::call& call,
    const exclusive_request& request) noexcept {
    return request_eligible(call, request.target, request.window);
}

} // namespace

bool native_input_factory::supported() const noexcept {
    if (create == nullptr || instance == nullptr) {
        return false;
    }
    if (create == &create_system_input) {
        return context == nullptr;
    }
    return lifetime != nullptr;
}

struct exclusive_input::implementation final {
    enum class slot_phase : std::uint8_t { free, reserving, joinable, joining };

    struct slot final {
        slot_phase phase{slot_phase::free};
        exclusive_request request{};
        std::shared_ptr<exclusive_lease::state> receipt;
        std::thread worker;
    };

    struct receipt_bundle final {
        std::shared_ptr<exclusive_lease::state> receipt;
        std::shared_ptr<exclusive_lease> lease;
    };

    explicit implementation(input_access& access_value) noexcept : access(access_value) {}

    [[nodiscard]] std::expected<std::shared_ptr<exclusive_lease>, HRESULT> reserve(
        exclusive_request request,
        proof_clock now,
        std::shared_ptr<const void> context) noexcept;
    [[nodiscard]] HRESULT reserve_slot(exclusive_request&& request,
        std::size_t& reserved_index) noexcept;
    [[nodiscard]] static std::expected<receipt_bundle, HRESULT> make_receipt(proof_clock now,
        std::shared_ptr<const void> context) noexcept;
    [[nodiscard]] HRESULT launch_actor(std::size_t reserved_index,
        const std::shared_ptr<exclusive_lease::state>& receipt) noexcept;
    void dispose_slot(std::size_t index) noexcept;
    void close_admission() noexcept;
    void drain() noexcept;
    void refresh_eligibility(const input_access::call& call) noexcept;
    void run_actor(std::size_t index,
        const std::shared_ptr<exclusive_lease::state>& receipt) noexcept;
    [[nodiscard]] HRESULT prepare_actor(const exclusive_request& request,
        const exclusive_lease::state& receipt,
        IDirectInput8W*& system,
        IDirectInputDevice8W*& device,
        bool& acquire_attempted);
    [[nodiscard]] HRESULT configure_actor_device(const exclusive_request& request,
        const exclusive_lease::state& receipt,
        IDirectInputDevice8W& device) const noexcept;
    void hold_actor_lease(const exclusive_request& request,
        exclusive_lease::state& receipt,
        IDirectInputDevice8W* device,
        HRESULT preparation) const noexcept;
    void hold_background_lease(const exclusive_request& request,
        exclusive_lease::state& receipt,
        IDirectInputDevice8W& device) const noexcept;
    void reap_completed() noexcept;
    [[nodiscard]] bool can_continue(const exclusive_request& request,
        const exclusive_lease::state& receipt) const noexcept;
    [[nodiscard]] bool exact_target_eligible(const exclusive_request& request) const noexcept;
    [[nodiscard]] HRESULT probe_authority(const exclusive_request& request,
        exclusive_lease::state& receipt,
        IDirectInputDevice8W& device,
        bool initial,
        bool requires_poll,
        steady_clock::time_point& next_probe) const noexcept;
    void monitor_authority(const exclusive_request& request,
        exclusive_lease::state& receipt,
        IDirectInputDevice8W& device,
        bool requires_poll,
        steady_clock::time_point next_probe) const noexcept;

    input_access& access;
    std::mutex gate;
    std::condition_variable changed;
    std::mutex reaper_gate;
    std::mutex drain_gate;
    std::array<slot, actor_capacity> slots{};
    std::atomic<bool> admission_closed;
    bool drained{};
};

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::implementation::reserve(
    exclusive_request request,
    const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected{DIERR_INPUTLOST};
    }
    if (native_control_reentrant()) {
        return std::unexpected{HRESULT_FROM_WIN32(ERROR_BUSY)};
    }
    HRESULT status = validate_request(request);
    if (FAILED(status)) {
        return std::unexpected{status};
    }

    reap_completed();
    std::size_t reserved_index = actor_capacity;
    status = reserve_slot(std::move(request), reserved_index);
    if (FAILED(status)) {
        return std::unexpected{status};
    }

    auto created = make_receipt(now, std::move(context));
    if (!created.has_value()) {
        dispose_slot(reserved_index);
        return std::unexpected{created.error()};
    }

    status = launch_actor(reserved_index, created->receipt);
    if (FAILED(status)) {
        dispose_slot(reserved_index);
        return std::unexpected{status};
    }
    return created->lease;
}

HRESULT exclusive_input::implementation::reserve_slot(exclusive_request&& request,
    std::size_t& reserved_index) noexcept {
    std::scoped_lock lock{gate};
    if (admission_closed.load(std::memory_order_acquire)) {
        return DIERR_INPUTLOST;
    }
    for (const auto& current : slots) {
        if (current.phase != slot_phase::free && same_identity(current.request, request)) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
    }
    for (std::size_t index = 0U; index < slots.size(); ++index) {
        if (slots[index].phase == slot_phase::free) {
            reserved_index = index;
            slots[index].phase = slot_phase::reserving;
            slots[index].request = std::move(request);
            return DI_OK;
        }
    }
    return DIERR_OUTOFMEMORY;
}

std::expected<exclusive_input::implementation::receipt_bundle, HRESULT>
exclusive_input::implementation::make_receipt(const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    try {
        auto receipt = std::make_shared<exclusive_lease::state>(now, std::move(context));
        receipt->start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->start_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->prepared_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->prepared_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->cancellation_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->cancellation_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->closed_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->closed_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        auto* raw_lease = new (std::nothrow) exclusive_lease{receipt};
        if (raw_lease == nullptr) {
            return std::unexpected{DIERR_OUTOFMEMORY};
        }
        return receipt_bundle{.receipt = std::move(receipt),
            .lease = std::shared_ptr<exclusive_lease>{raw_lease}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{DIERR_OUTOFMEMORY};
    } catch (...) {
        return std::unexpected{DIERR_GENERIC};
    }
}

HRESULT exclusive_input::implementation::launch_actor(const std::size_t reserved_index,
    const std::shared_ptr<exclusive_lease::state>& receipt) noexcept {
    if (admission_closed.load(std::memory_order_acquire)) {
        return DIERR_INPUTLOST;
    }
    try {
        std::scoped_lock lock{gate};
        auto& reserved = slots[reserved_index];
        if (admission_closed.load(std::memory_order_acquire) ||
            reserved.phase != slot_phase::reserving) {
            return DIERR_INPUTLOST;
        }
        reserved.receipt = receipt;
        reserved.worker =
            std::thread{[this, reserved_index, receipt] { run_actor(reserved_index, receipt); }};
        reserved.phase = slot_phase::joinable;
        return DI_OK;
    } catch (const std::bad_alloc&) {
        return DIERR_OUTOFMEMORY;
    } catch (const std::system_error& error) {
        const auto& code = error.code();
        if (code == std::errc::resource_unavailable_try_again ||
            code == std::errc::not_enough_memory) {
            return DIERR_OUTOFMEMORY;
        }
        if (code.category() == std::system_category() && code.value() > 0) {
            return HRESULT_FROM_WIN32(static_cast<DWORD>(code.value()));
        }
        return DIERR_GENERIC;
    } catch (...) {
        return DIERR_GENERIC;
    }
}

void exclusive_input::implementation::dispose_slot(const std::size_t index) noexcept {
    // A reservation or join claim keeps the identity occupied through provider disposal.
    auto& current = slots[index];
    std::shared_ptr<void> lifetime;
    {
        std::scoped_lock lock{gate};
        lifetime = std::move(current.request.factory.lifetime);
    }
    lifetime.reset();
    {
        std::scoped_lock lock{gate};
        current.request = {};
        current.receipt.reset();
        current.phase = slot_phase::free;
    }
    changed.notify_all();
}

bool exclusive_input::implementation::exact_target_eligible(
    const exclusive_request& request) const noexcept {
    const auto call = access.enter();
    return request_eligible(call, request);
}

bool exclusive_input::implementation::can_continue(const exclusive_request& request,
    const exclusive_lease::state& receipt) const noexcept {
    if (admission_closed.load(std::memory_order_acquire) || receipt.cancelled() ||
        (background(request) && !background_window_valid(request.background_window)) ||
        !exact_target_eligible(request)) {
        return false;
    }
    return !receipt.cancelled() && !admission_closed.load(std::memory_order_acquire) &&
           access.open();
}

HRESULT exclusive_input::implementation::probe_authority(const exclusive_request& request,
    exclusive_lease::state& receipt,
    IDirectInputDevice8W& device,
    const bool initial,
    const bool requires_poll,
    steady_clock::time_point& next_probe) const noexcept {
    if (!can_continue(request, receipt)) {
        receipt.request_cancel();
        return DIERR_INPUTLOST;
    }
    const auto started = receipt.now();
    next_probe = steady_clock::now() + probe_period;
    HRESULT result = DI_OK;
    if (requires_poll) {
        result = device.Poll();
        if (!can_continue(request, receipt)) {
            receipt.request_cancel();
            return DIERR_INPUTLOST;
        }
    }
    if (result == DI_OK || result == DI_NOEFFECT) {
        DIJOYSTATE scratch{};
        result = device.GetDeviceState(static_cast<DWORD>(sizeof(scratch)), &scratch);
    }
    if (!can_continue(request, receipt)) {
        receipt.request_cancel();
        return DIERR_INPUTLOST;
    }
    return receipt.record_probe(result, started, initial, admission_closed, access);
}

void exclusive_input::implementation::monitor_authority(const exclusive_request& request,
    exclusive_lease::state& receipt,
    IDirectInputDevice8W& device,
    const bool requires_poll,
    steady_clock::time_point next_probe) const noexcept {
    const std::array events{receipt.cancellation_event};
    while (!receipt.cancelled()) {
        const auto wake = (std::min)(next_probe, receipt.authority_deadline());
        if (actor_wait(events, wake) != WAIT_TIMEOUT) {
            break;
        }
        if (steady_clock::now() >= next_probe) {
            const HRESULT result =
                probe_authority(request, receipt, device, false, requires_poll, next_probe);
            if (result != DI_OK && result != E_PENDING) {
                break;
            }
        }
    }
    receipt.request_cancel();
}

void exclusive_input::implementation::hold_actor_lease(const exclusive_request& request,
    exclusive_lease::state& receipt,
    IDirectInputDevice8W* const device,
    const HRESULT preparation) const noexcept {
    if (preparation == DI_OK && background(request)) {
        if (device == nullptr) {
            receipt.publish_prepared(E_NOINTERFACE);
            return;
        }
        hold_background_lease(request, receipt, *device);
        return;
    }
    receipt.publish_prepared(preparation);
    const std::array cancellation_events{receipt.cancellation_event};
    if (receipt.authority() == DI_OK && actor_wait(cancellation_events) == WAIT_FAILED) {
        receipt.request_cancel();
    }
}

void exclusive_input::implementation::hold_background_lease(const exclusive_request& request,
    exclusive_lease::state& receipt,
    IDirectInputDevice8W& device) const noexcept {
    if (!can_continue(request, receipt)) {
        receipt.request_cancel();
        return;
    }
    DIDEVCAPS capabilities{};
    capabilities.dwSize = static_cast<DWORD>(sizeof(capabilities));
    const HRESULT inspected = device.GetCapabilities(&capabilities);
    if (!can_continue(request, receipt)) {
        receipt.request_cancel();
        return;
    }
    if (FAILED(inspected)) {
        receipt.publish_prepared(inspected);
        return;
    }
    const bool requires_poll = (capabilities.dwFlags & DIDC_POLLEDDATAFORMAT) != 0U;
    steady_clock::time_point next_probe{};
    const std::array events{receipt.cancellation_event};
    while (!receipt.cancelled()) {
        const HRESULT result =
            probe_authority(request, receipt, device, true, requires_poll, next_probe);
        if (result == DI_OK) {
            monitor_authority(request, receipt, device, requires_poll, next_probe);
            return;
        }
        if (result != E_PENDING || receipt.cancelled()) {
            return;
        }
        if (actor_wait(events, next_probe) != WAIT_TIMEOUT) {
            receipt.request_cancel();
            return;
        }
    }
}

void exclusive_input::implementation::run_actor(const std::size_t index,
    const std::shared_ptr<exclusive_lease::state>& receipt) noexcept {
    const auto& request = slots[index].request;
    HRESULT preparation = DIERR_GENERIC;
    HRESULT close_result = DI_OK;
    bool apartment_initialized{};
    bool acquire_attempted{};
    IDirectInput8W* system{};
    IDirectInputDevice8W* device{};

    const std::array start_events{receipt->cancellation_event, receipt->start_event};
    const DWORD started = actor_wait(start_events);
    if (started == WAIT_OBJECT_0 + 1U) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) {
            preparation = initialized;
        } else {
            apartment_initialized = true;
            try {
                preparation = prepare_actor(request, *receipt, system, device, acquire_attempted);
            } catch (const std::bad_alloc&) {
                preparation = DIERR_OUTOFMEMORY;
            } catch (...) {
                preparation = DIERR_GENERIC;
            }
        }
    } else if (started == WAIT_FAILED || started == WAIT_OBJECT_0) {
        preparation = DIERR_INPUTLOST;
    } else {
        preparation = E_UNEXPECTED;
    }

    hold_actor_lease(request, *receipt, device, preparation);
    if (device != nullptr) {
        if (acquire_attempted) {
            close_result = device->Unacquire();
        }
        device->Release();
        device = nullptr;
    }
    if (system != nullptr) {
        system->Release();
        system = nullptr;
    }
    if (apartment_initialized) {
        CoUninitialize();
    }
    receipt->publish_closed(FAILED(close_result) ? close_result : DI_OK);
}

HRESULT exclusive_input::implementation::prepare_actor(const exclusive_request& request,
    const exclusive_lease::state& receipt,
    IDirectInput8W*& system,
    IDirectInputDevice8W*& device,
    bool& acquire_attempted) {
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    void* root_value{};
    HRESULT status = request.factory.create(request.factory.context,
        request.factory.instance,
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        &root_value,
        nullptr);
    system = static_cast<IDirectInput8W*>(root_value);
    if (FAILED(status)) {
        return status;
    }
    if (system == nullptr) {
        return E_NOINTERFACE;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = qualify_exclusive_device(access,
        *system,
        request.target,
        request.native_instance,
        request.native_path,
        receipt.cancellation_event);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = system->CreateDevice(request.native_instance, &device, nullptr);
    if (FAILED(status)) {
        return status;
    }
    if (device == nullptr) {
        return E_NOINTERFACE;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = read_opened_path(*device, request.native_path);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = configure_actor_device(request, receipt, *device);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    acquire_attempted = true;
    status = device->Acquire();
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = read_opened_path(*device, request.native_path);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }

    status = qualify_exclusive_device(access,
        *system,
        request.target,
        request.native_instance,
        request.native_path,
        receipt.cancellation_event);
    if (FAILED(status)) {
        return status;
    }
    return can_continue(request, receipt) ? DI_OK : DIERR_INPUTLOST;
}

HRESULT exclusive_input::implementation::configure_actor_device(const exclusive_request& request,
    const exclusive_lease::state& receipt,
    IDirectInputDevice8W& device) const noexcept {
    const auto* const format = system_joystick_format();
    const bool is_background = background(request);
    if (format == nullptr ||
        (is_background && format->dwDataSize != static_cast<DWORD>(sizeof(DIJOYSTATE)))) {
        return DIERR_UNSUPPORTED;
    }
    const HRESULT status = device.SetDataFormat(format);
    if (FAILED(status)) {
        return status;
    }
    static_assert(sizeof(HWND) == sizeof(focus::window_id));
    auto* const window = is_background ? request.background_window.handle
                                       : std::bit_cast<HWND>(request.window.window);
    if ((!is_background && IsWindow(window) == FALSE) || !can_continue(request, receipt)) {
        return DIERR_INPUTLOST;
    }
    const auto scope = static_cast<DWORD>(is_background ? DISCL_BACKGROUND : DISCL_FOREGROUND);
    return device.SetCooperativeLevel(window, scope | static_cast<DWORD>(DISCL_EXCLUSIVE));
}

void exclusive_input::implementation::refresh_eligibility(const input_access::call& call) noexcept {
    std::array<std::shared_ptr<exclusive_lease::state>, actor_capacity> receipts{};
    std::array<connection_target, actor_capacity> targets{};
    std::array<focus::window_epoch, actor_capacity> windows{};
    std::size_t count{};
    {
        std::scoped_lock lock{gate};
        for (const auto& current : slots) {
            if (current.phase != slot_phase::free && current.receipt != nullptr) {
                receipts[count] = current.receipt;
                targets[count] = current.request.target;
                windows[count] = current.request.window;
                ++count;
            }
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (receipts[index]->cancelled() ||
            !request_eligible(call, targets[index], windows[index])) {
            receipts[index]->request_cancel();
        }
    }
}

void exclusive_input::implementation::close_admission() noexcept {
    admission_closed.store(true, std::memory_order_release);
    std::array<std::shared_ptr<exclusive_lease::state>, actor_capacity> receipts{};
    std::size_t count{};
    {
        std::scoped_lock lock{gate};
        for (const auto& current : slots) {
            if (current.phase != slot_phase::free && current.receipt != nullptr) {
                receipts[count++] = current.receipt;
            }
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        receipts[index]->request_cancel();
    }
    changed.notify_all();
}

void exclusive_input::implementation::reap_completed() noexcept {
    for (;;) {
        std::size_t index = actor_capacity;
        {
            std::scoped_lock reaper_lock{reaper_gate};
            {
                std::scoped_lock lock{gate};
                for (std::size_t candidate = 0U; candidate < slots.size(); ++candidate) {
                    auto& current = slots[candidate];
                    if (current.phase == slot_phase::joinable && current.receipt->is_closed()) {
                        index = candidate;
                        current.phase = slot_phase::joining;
                        break;
                    }
                }
            }
            if (index == actor_capacity) {
                return;
            }
            slots[index].worker.join();
        }
        dispose_slot(index);
    }
}

void exclusive_input::implementation::drain() noexcept {
    close_admission();
    std::scoped_lock drain_lock{drain_gate};
    if (drained) {
        return;
    }
    for (;;) {
        std::size_t index = actor_capacity;
        bool joining_elsewhere{};
        {
            std::scoped_lock reaper_lock{reaper_gate};
            {
                std::unique_lock lock{gate};
                changed.wait(lock, [this] {
                    return std::ranges::none_of(slots,
                        [](const slot& current) { return current.phase == slot_phase::reserving; });
                });
                joining_elsewhere = std::ranges::any_of(slots,
                    [](const slot& current) { return current.phase == slot_phase::joining; });
                if (!joining_elsewhere) {
                    for (std::size_t candidate = 0U; candidate < slots.size(); ++candidate) {
                        if (slots[candidate].phase != slot_phase::free) {
                            index = candidate;
                            slots[candidate].phase = slot_phase::joining;
                            break;
                        }
                    }
                }
            }
            if (!joining_elsewhere && index != actor_capacity) {
                join_worker_for_shutdown(slots[index].worker);
            }
        }

        if (joining_elsewhere) {
            std::unique_lock wait_lock{gate};
            changed.wait(wait_lock, [this] {
                return std::ranges::none_of(slots,
                    [](const slot& current) { return current.phase == slot_phase::joining; });
            });
            continue;
        }
        if (index == actor_capacity) {
            drained = true;
            return;
        }
        dispose_slot(index);
    }
}

exclusive_input::exclusive_input(input_access& access)
    : implementation_(std::make_unique<implementation>(access)) {
}

exclusive_input::~exclusive_input() {
    drain();
}

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::reserve(
    exclusive_request request) noexcept {
    return reserve(std::move(request), nullptr, nullptr);
}

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::reserve(
    exclusive_request request,
    const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    return implementation_->reserve(std::move(request), now, std::move(context));
}

void exclusive_input::refresh_eligibility(const input_access::call& call) noexcept {
    implementation_->refresh_eligibility(call);
}

void exclusive_input::close_admission() noexcept {
    implementation_->close_admission();
}

void exclusive_input::drain() noexcept {
    implementation_->drain();
}

} // namespace inputnext::dinput
