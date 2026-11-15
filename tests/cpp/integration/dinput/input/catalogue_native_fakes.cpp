#include "catalogue_test_support.hpp"

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace inputnext::dinput_catalogue_test {
namespace {

class system_device final : public IDirectInputDevice8W {
public:
    explicit system_device(system_device_record record) : record_(std::move(record)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirectInputDevice8W)) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IDirectInputDevice8W*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS capabilities) noexcept override {
        if (capabilities == nullptr || capabilities->dwSize != sizeof(*capabilities)) {
            return DIERR_INVALIDPARAM;
        }
        capabilities->dwFlags = record_.force_feedback ? DIDC_FORCEFEEDBACK : 0U;
        capabilities->dwDevType = record_.instance.dwDevType;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects([[maybe_unused]] LPDIENUMDEVICEOBJECTSCALLBACKW callback,
        [[maybe_unused]] LPVOID context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property,
        LPDIPROPHEADER header) noexcept override {
        if (header == nullptr || header->dwHeaderSize != sizeof(DIPROPHEADER) ||
            header->dwHow != DIPH_DEVICE) {
            return DIERR_UNSUPPORTED;
        }
        if (reinterpret_cast<std::uintptr_t>(&property) == 24U) {
            if (header->dwSize != static_cast<DWORD>(sizeof(DIPROPDWORD))) {
                return DIERR_INVALIDPARAM;
            }
            DIPROPDWORD value{};
            std::memcpy(&value, header, sizeof(value));
            value.dwData = record_.vendor_product;
            std::memcpy(header, &value, sizeof(value));
            return DI_OK;
        }
        if (reinterpret_cast<std::uintptr_t>(&property) != 12U ||
            header->dwSize != static_cast<DWORD>(sizeof(DIPROPGUIDANDPATH))) {
            return DIERR_UNSUPPORTED;
        }
        if (record_.path.empty() || record_.path.size() >= MAX_PATH) {
            return DIERR_UNSUPPORTED;
        }
        DIPROPGUIDANDPATH result{};
        result.diph = *header;
        result.guidClass = GUID_Joystick;
        std::ranges::copy(record_.path, result.wszPath);
        result.wszPath[record_.path.size()] = L'\0';
        std::memcpy(header, &result, sizeof(result));
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetProperty([[maybe_unused]] REFGUID property,
        [[maybe_unused]] LPCDIPROPHEADER header) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Acquire() noexcept override { return DIERR_UNSUPPORTED; }

    HRESULT STDMETHODCALLTYPE Unacquire() noexcept override { return DIERR_UNSUPPORTED; }

    HRESULT STDMETHODCALLTYPE GetDeviceState([[maybe_unused]] DWORD size,
        [[maybe_unused]] LPVOID state) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData([[maybe_unused]] DWORD object_size,
        [[maybe_unused]] LPDIDEVICEOBJECTDATA data,
        [[maybe_unused]] LPDWORD count,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(
        [[maybe_unused]] LPCDIDATAFORMAT format) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(
        [[maybe_unused]] HANDLE event) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel([[maybe_unused]] HWND window,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo([[maybe_unused]] LPDIDEVICEOBJECTINSTANCEW info,
        DWORD /*object*/,
        DWORD /*how*/) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW info) noexcept override {
        if (info == nullptr || info->dwSize != sizeof(*info)) {
            return DIERR_INVALIDPARAM;
        }
        *info = record_.instance;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel([[maybe_unused]] HWND window,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Initialize([[maybe_unused]] HINSTANCE instance,
        [[maybe_unused]] DWORD version,
        [[maybe_unused]] REFGUID guid) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE CreateEffect([[maybe_unused]] REFGUID effect,
        [[maybe_unused]] LPCDIEFFECT parameters,
        LPDIRECTINPUTEFFECT* out,
        [[maybe_unused]] LPUNKNOWN outer) noexcept override {
        if (out != nullptr) {
            *out = nullptr;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects([[maybe_unused]] LPDIENUMEFFECTSCALLBACKW callback,
        [[maybe_unused]] LPVOID context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo([[maybe_unused]] LPDIEFFECTINFOW info,
        [[maybe_unused]] REFGUID effect) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(
        [[maybe_unused]] LPDWORD state) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(
        [[maybe_unused]] DWORD command) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(
        [[maybe_unused]] LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
        [[maybe_unused]] LPVOID context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Escape([[maybe_unused]] LPDIEFFESCAPE escape) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Poll() noexcept override { return DIERR_UNSUPPORTED; }

    HRESULT STDMETHODCALLTYPE SendDeviceData([[maybe_unused]] DWORD object_size,
        [[maybe_unused]] LPCDIDEVICEOBJECTDATA data,
        [[maybe_unused]] LPDWORD count,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile([[maybe_unused]] LPCWSTR file,
        [[maybe_unused]] LPDIENUMEFFECTSINFILECALLBACK callback,
        [[maybe_unused]] LPVOID context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile([[maybe_unused]] LPCWSTR file,
        [[maybe_unused]] DWORD count,
        [[maybe_unused]] LPDIFILEEFFECT effects,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap([[maybe_unused]] LPDIACTIONFORMATW format,
        [[maybe_unused]] LPCWSTR user_name,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetActionMap([[maybe_unused]] LPDIACTIONFORMATW format,
        [[maybe_unused]] LPCWSTR user_name,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(
        [[maybe_unused]] LPDIDEVICEIMAGEINFOHEADERW info) noexcept override {
        return DIERR_UNSUPPORTED;
    }

private:
    std::atomic<ULONG> references_{1U};
    const system_device_record record_;
};

class system_input final : public IDirectInput8W {
public:
    explicit system_input(std::shared_ptr<system_input_state> state) noexcept
        : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirectInput8W)) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IDirectInput8W*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID instance,
        LPDIRECTINPUTDEVICE8W* out,
        LPUNKNOWN outer) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        try {
            const auto record = find(instance);
            if (!record.has_value()) {
                return DIERR_NOTFOUND;
            }
            auto* created = new (std::nothrow) system_device{*record};
            if (created == nullptr) {
                return DIERR_OUTOFMEMORY;
            }
            *out = created;
            return DI_OK;
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD device_type,
        LPDIENUMDEVICESCALLBACKW callback,
        LPVOID context,
        DWORD flags) noexcept override {
        if (callback == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        try {
            std::vector<system_device_record> snapshot;
            {
                std::scoped_lock lock{state_->gate};
                snapshot = state_->devices;
            }
            for (const auto& record : snapshot) {
                if (!matches_type(record.instance.dwDevType, device_type) ||
                    ((flags & DIEDFL_ATTACHEDONLY) != 0U && !record.attached) ||
                    ((flags & DIEDFL_FORCEFEEDBACK) != 0U && !record.force_feedback)) {
                    continue;
                }
                if (callback(&record.instance, context) == DIENUM_STOP) {
                    break;
                }
            }
            return DI_OK;
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID instance) noexcept override {
        try {
            return find(instance).has_value() ? DI_OK : DIERR_NOTFOUND;
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel([[maybe_unused]] HWND window,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize([[maybe_unused]] HINSTANCE instance,
        [[maybe_unused]] DWORD version) noexcept override {
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE FindDevice([[maybe_unused]] REFGUID device_class,
        [[maybe_unused]] LPCWSTR name,
        LPGUID out) noexcept override {
        if (out == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        *out = {};
        return DIERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics([[maybe_unused]] LPCWSTR user_name,
        [[maybe_unused]] LPDIACTIONFORMATW format,
        [[maybe_unused]] LPDIENUMDEVICESBYSEMANTICSCBW callback,
        [[maybe_unused]] LPVOID context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(
        [[maybe_unused]] LPDICONFIGUREDEVICESCALLBACK callback,
        [[maybe_unused]] LPDICONFIGUREDEVICESPARAMSW parameters,
        [[maybe_unused]] DWORD flags,
        [[maybe_unused]] LPVOID context) noexcept override {
        return DIERR_UNSUPPORTED;
    }

private:
    [[nodiscard]] static bool matches_type(DWORD actual, DWORD requested) noexcept {
        if (requested == DI8DEVCLASS_ALL) {
            return true;
        }
        if (requested == DI8DEVCLASS_GAMECTRL) {
            const auto type = static_cast<DWORD>(GET_DIDEVICE_TYPE(actual));
            return type != DI8DEVTYPE_KEYBOARD && type != DI8DEVTYPE_MOUSE;
        }
        if (requested == DI8DEVCLASS_KEYBOARD) {
            return GET_DIDEVICE_TYPE(actual) == DI8DEVTYPE_KEYBOARD;
        }
        if (requested == DI8DEVCLASS_POINTER) {
            return GET_DIDEVICE_TYPE(actual) == DI8DEVTYPE_MOUSE;
        }
        return GET_DIDEVICE_TYPE(actual) == (requested & 0xffU) &&
               ((requested >> 8U) == 0U || GET_DIDEVICE_SUBTYPE(actual) == (requested >> 8U));
    }

    [[nodiscard]] std::optional<system_device_record> find(REFGUID instance) const {
        std::scoped_lock lock{state_->gate};
        const auto found = std::ranges::find_if(state_->devices, [&instance](const auto& value) {
            return IsEqualGUID(value.instance.guidInstance, instance);
        });
        return found == state_->devices.end() ? std::nullopt
                                              : std::optional<system_device_record>{*found};
    }

    std::atomic<ULONG> references_{1U};
    std::shared_ptr<system_input_state> state_;
};

} // namespace

HRESULT create_system(void* value,
    [[maybe_unused]] HINSTANCE instance,
    [[maybe_unused]] DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }
    const auto& context = *static_cast<host_context*>(value);
    auto* created = new (std::nothrow) system_input{context.system};
    if (created == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = created->QueryInterface(iid, out);
    created->Release();
    return status;
}

} // namespace inputnext::dinput_catalogue_test
