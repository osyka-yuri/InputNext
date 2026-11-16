#include "actor.hpp"

#include "dinput/catalogue/devices.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/property_abi.h"
#include "dinput/system_input.hpp"
#include "lease_state.hpp"
#include "ownership.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <inputnext/focus.hpp>
#include <new>
#include <optional>
#include <span>

namespace inputnext::dinput {
namespace {

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
    if (!detail::valid_exclusive_path(actual) ||
        IsEqualGUID(actual.guidClass, expected.guidClass) == 0 ||
        !detail::same_exclusive_path(actual, expected)) {
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

} // namespace

bool exclusive_input::actor::exact_target_eligible() const noexcept {
    const auto call = access.enter();
    return detail::exclusive_target_eligible(call, request.target, request.window);
}

bool exclusive_input::actor::can_continue() const noexcept {
    if (admission_closed.load(std::memory_order_acquire) || receipt.cancelled() ||
        (background(request) && !background_window_valid(request.background_window)) ||
        !exact_target_eligible()) {
        return false;
    }
    return !receipt.cancelled() && !admission_closed.load(std::memory_order_acquire) &&
           access.open();
}

HRESULT exclusive_input::actor::probe_authority(IDirectInputDevice8W& device,
    const bool initial,
    const bool requires_poll,
    steady_clock::time_point& next_probe) const noexcept {
    if (!can_continue()) {
        receipt.request_cancel();
        return DIERR_INPUTLOST;
    }
    const auto started = receipt.now();
    next_probe = steady_clock::now() + probe_period;
    HRESULT result = DI_OK;
    if (requires_poll) {
        result = device.Poll();
        if (!can_continue()) {
            receipt.request_cancel();
            return DIERR_INPUTLOST;
        }
    }
    if (result == DI_OK || result == DI_NOEFFECT) {
        DIJOYSTATE scratch{};
        result = device.GetDeviceState(static_cast<DWORD>(sizeof(scratch)), &scratch);
    }
    if (!can_continue()) {
        receipt.request_cancel();
        return DIERR_INPUTLOST;
    }
    return receipt.record_probe(result, started, initial, admission_closed, access);
}

void exclusive_input::actor::monitor_authority(IDirectInputDevice8W& device,
    const bool requires_poll,
    steady_clock::time_point next_probe) const noexcept {
    const std::array events{receipt.cancellation_event};
    while (!receipt.cancelled()) {
        const auto wake = (std::min)(next_probe, receipt.authority_deadline());
        if (actor_wait(events, wake) != WAIT_TIMEOUT) {
            break;
        }
        if (steady_clock::now() >= next_probe) {
            const HRESULT result = probe_authority(device, false, requires_poll, next_probe);
            if (result != DI_OK && result != E_PENDING) {
                break;
            }
        }
    }
    receipt.request_cancel();
}

void exclusive_input::actor::hold_lease(IDirectInputDevice8W* const device,
    const HRESULT preparation) const noexcept {
    if (preparation == DI_OK && background(request)) {
        if (device == nullptr) {
            receipt.publish_prepared(E_NOINTERFACE);
            return;
        }
        hold_background_lease(*device);
        return;
    }
    receipt.publish_prepared(preparation);
    const std::array cancellation_events{receipt.cancellation_event};
    if (receipt.authority() == DI_OK && actor_wait(cancellation_events) == WAIT_FAILED) {
        receipt.request_cancel();
    }
}

void exclusive_input::actor::hold_background_lease(IDirectInputDevice8W& device) const noexcept {
    if (!can_continue()) {
        receipt.request_cancel();
        return;
    }
    DIDEVCAPS capabilities{};
    capabilities.dwSize = static_cast<DWORD>(sizeof(capabilities));
    const HRESULT inspected = device.GetCapabilities(&capabilities);
    if (!can_continue()) {
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
        const HRESULT result = probe_authority(device, true, requires_poll, next_probe);
        if (result == DI_OK) {
            monitor_authority(device, requires_poll, next_probe);
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

void exclusive_input::actor::run() const noexcept {
    HRESULT preparation = DIERR_GENERIC;
    HRESULT close_result = DI_OK;
    bool apartment_initialized{};
    bool acquire_attempted{};
    IDirectInput8W* system{};
    IDirectInputDevice8W* device{};

    const std::array start_events{receipt.cancellation_event, receipt.start_event};
    const DWORD started = actor_wait(start_events);
    if (started == WAIT_OBJECT_0 + 1U) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) {
            preparation = initialized;
        } else {
            apartment_initialized = true;
            try {
                preparation = prepare(system, device, acquire_attempted);
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

    hold_lease(device, preparation);
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
    receipt.publish_closed(FAILED(close_result) ? close_result : DI_OK);
}

HRESULT exclusive_input::actor::prepare(IDirectInput8W*& system,
    IDirectInputDevice8W*& device,
    bool& acquire_attempted) const {
    if (!can_continue()) {
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
    if (!can_continue()) {
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
    if (!can_continue()) {
        return DIERR_INPUTLOST;
    }

    status = system->CreateDevice(request.native_instance, &device, nullptr);
    if (FAILED(status)) {
        return status;
    }
    if (device == nullptr) {
        return E_NOINTERFACE;
    }
    if (!can_continue()) {
        return DIERR_INPUTLOST;
    }

    status = read_opened_path(*device, request.native_path);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue()) {
        return DIERR_INPUTLOST;
    }

    status = configure_device(*device);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue()) {
        return DIERR_INPUTLOST;
    }

    acquire_attempted = true;
    status = device->Acquire();
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue()) {
        return DIERR_INPUTLOST;
    }

    status = read_opened_path(*device, request.native_path);
    if (FAILED(status)) {
        return status;
    }
    if (!can_continue()) {
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
    return can_continue() ? DI_OK : DIERR_INPUTLOST;
}

HRESULT exclusive_input::actor::configure_device(IDirectInputDevice8W& device) const noexcept {
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
    if ((!is_background && IsWindow(window) == FALSE) || !can_continue()) {
        return DIERR_INPUTLOST;
    }
    const auto scope = static_cast<DWORD>(is_background ? DISCL_BACKGROUND : DISCL_FOREGROUND);
    return device.SetCooperativeLevel(window, scope | static_cast<DWORD>(DISCL_EXCLUSIVE));
}

} // namespace inputnext::dinput
