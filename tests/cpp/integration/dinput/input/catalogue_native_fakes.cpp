#include "catalogue_test_support.hpp"

#include <objbase.h>

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace inputnext::dinput_catalogue_test {

std::size_t native_control_script::index(const native_operation operation) noexcept {
    return static_cast<std::size_t>(operation);
}

void native_control_script::set_result(const native_operation operation,
    const HRESULT result) noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return;
    }
    std::scoped_lock lock{gate_};
    operations_[slot].result = result;
}

void native_control_script::block_next(const native_operation operation,
    const bool selected_device_only) noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return;
    }
    std::scoped_lock lock{gate_};
    auto& state = operations_[slot];
    state.block_next = true;
    state.selected_device_only = selected_device_only;
    state.blocked = false;
    state.released = false;
}

bool native_control_script::wait_for_entry(const native_operation operation,
    const std::size_t entry_count,
    const std::chrono::milliseconds timeout) const {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return false;
    }
    std::unique_lock lock{gate_};
    return changed_.wait_for(lock, timeout, [this, slot, entry_count] {
        return operations_[slot].entries >= entry_count;
    });
}

void native_control_script::release_block(const native_operation operation) noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return;
    }
    std::scoped_lock lock{gate_};
    operations_[slot].released = true;
    changed_.notify_all();
}

void native_control_script::send_message_on(const native_operation operation,
    const native_message message) noexcept {
    std::scoped_lock lock{gate_};
    send_operation_ = operation;
    send_window_ = message.window;
    send_message_ = message.message;
    send_wparam_ = message.wparam;
    send_lparam_ = message.lparam;
    send_once_ = true;
}

void native_control_script::clear_send_message() noexcept {
    std::scoped_lock lock{gate_};
    send_operation_ = native_operation::count;
    send_window_ = nullptr;
    send_message_ = 0U;
    send_wparam_ = 0U;
    send_lparam_ = 0L;
    send_once_ = false;
}

void native_control_script::set_opened_path_after_acquire(std::wstring path) {
    std::scoped_lock lock{gate_};
    opened_path_after_acquire_ = std::move(path);
}

bool native_control_script::copy_opened_path_after_acquire(const bool acquired,
    wchar_t* const destination,
    const std::size_t capacity) const noexcept {
    if (!acquired || destination == nullptr || capacity == 0U) {
        return false;
    }
    std::scoped_lock lock{gate_};
    if (opened_path_after_acquire_.empty() || opened_path_after_acquire_.size() >= capacity) {
        return false;
    }
    std::ranges::copy(opened_path_after_acquire_, destination);
    destination[opened_path_after_acquire_.size()] = L'\0';
    return true;
}

HRESULT native_control_script::observe(const native_operation operation,
    const DIDATAFORMAT* const format,
    const HWND window,
    const DWORD flags,
    const bool selected_device) noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return DIERR_INVALIDPARAM;
    }

    HRESULT result{};
    bool should_block{};
    bool should_send{};
    HWND send_window{};
    UINT send_message{};
    WPARAM send_wparam{};
    LPARAM send_lparam{};
    APTTYPE apartment = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    const bool single_threaded_apartment =
        SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
        (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    {
        std::scoped_lock lock{gate_};
        auto& state = operations_[slot];
        ++state.calls;
        state.last_thread = std::this_thread::get_id();
        result = state.result;
        try {
            observations_.push_back(native_observation{.operation = operation,
                .thread = state.last_thread,
                .result = result,
                .format = format,
                .window = window,
                .flags = flags,
                .selected_device = selected_device,
                .single_threaded_apartment = single_threaded_apartment});
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }

        should_block = state.block_next && (!state.selected_device_only || selected_device);
        if (should_block) {
            state.block_next = false;
            state.blocked = true;
            state.released = false;
            ++state.entries;
            changed_.notify_all();
        }
        should_send = send_once_ && send_operation_ == operation && send_window_ != nullptr;
        if (should_send) {
            send_window = send_window_;
            send_message = send_message_;
            send_wparam = send_wparam_;
            send_lparam = send_lparam_;
            send_once_ = false;
        }
    }

    if (should_send) {
        static_cast<void>(SendMessageW(send_window, send_message, send_wparam, send_lparam));
    }
    if (should_block) {
        std::unique_lock lock{gate_};
        changed_.wait(lock, [this, slot] { return operations_[slot].released; });
        operations_[slot].blocked = false;
        operations_[slot].released = false;
    }
    return result;
}

std::size_t native_control_script::count(const native_operation operation) const noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return 0U;
    }
    std::scoped_lock lock{gate_};
    return operations_[slot].calls;
}

std::thread::id native_control_script::last_thread(
    const native_operation operation) const noexcept {
    const auto slot = index(operation);
    if (slot >= operations_.size()) {
        return {};
    }
    std::scoped_lock lock{gate_};
    return operations_[slot].last_thread;
}

std::vector<native_observation> native_control_script::observations() const {
    std::scoped_lock lock{gate_};
    return observations_;
}

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
            if (record_.controls != nullptr) {
                static_cast<void>(record_.controls->observe(native_operation::release_device,
                    nullptr,
                    nullptr,
                    0U,
                    selected_));
            }
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
        if (record_.controls != nullptr) {
            const HRESULT scripted = record_.controls->observe(native_operation::guid_and_path,
                nullptr,
                nullptr,
                0U,
                selected_);
            if (FAILED(scripted)) {
                return scripted;
            }
        }
        if (FAILED(record_.guid_path_result)) {
            return record_.guid_path_result;
        }
        std::array<wchar_t, MAX_PATH> opened_path{};
        const bool overridden = record_.controls != nullptr &&
                                record_.controls->copy_opened_path_after_acquire(acquired_,
                                    opened_path.data(),
                                    opened_path.size());
        const std::wstring_view path =
            overridden ? std::wstring_view{opened_path.data()} : std::wstring_view{record_.path};
        if (path.empty() || path.size() >= MAX_PATH) {
            return DIERR_UNSUPPORTED;
        }
        DIPROPGUIDANDPATH result{};
        result.diph = *header;
        result.guidClass = GUID_Joystick;
        std::ranges::copy(path, result.wszPath);
        result.wszPath[path.size()] = L'\0';
        std::memcpy(header, &result, sizeof(result));
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetProperty([[maybe_unused]] REFGUID property,
        [[maybe_unused]] LPCDIPROPHEADER header) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Acquire() noexcept override {
        const HRESULT status = record_.controls == nullptr
                                   ? DI_OK
                                   : record_.controls->observe(native_operation::acquire,
                                         nullptr,
                                         nullptr,
                                         0U,
                                         selected_);
        acquired_ = SUCCEEDED(status);
        return status;
    }

    HRESULT STDMETHODCALLTYPE Unacquire() noexcept override {
        const HRESULT status = record_.controls == nullptr
                                   ? DI_OK
                                   : record_.controls->observe(native_operation::unacquire,
                                         nullptr,
                                         nullptr,
                                         0U,
                                         selected_);
        if (SUCCEEDED(status)) {
            acquired_ = false;
        }
        return status;
    }

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

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) noexcept override {
        selected_ = true;
        return record_.controls == nullptr
                   ? DI_OK
                   : record_.controls->observe(native_operation::set_data_format,
                         format,
                         nullptr,
                         0U,
                         selected_);
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(
        [[maybe_unused]] HANDLE event) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) noexcept override {
        return record_.controls == nullptr
                   ? DI_OK
                   : record_.controls->observe(native_operation::set_cooperative_level,
                         nullptr,
                         window,
                         flags,
                         selected_);
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
    bool selected_{};
    bool acquired_{};
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
            if (state_->controls != nullptr) {
                static_cast<void>(state_->controls->observe(native_operation::release_root));
            }
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
        if (state_->controls != nullptr) {
            const HRESULT scripted = state_->controls->observe(native_operation::create_device);
            if (FAILED(scripted)) {
                return scripted;
            }
        }
        try {
            auto record = find(instance);
            if (!record.has_value()) {
                return DIERR_NOTFOUND;
            }
            if (FAILED(record->create_device_result)) {
                return record->create_device_result;
            }
            record->controls = state_->controls;
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
        if (state_->controls != nullptr) {
            const HRESULT scripted = state_->controls->observe(native_operation::enum_devices);
            if (FAILED(scripted)) {
                return scripted;
            }
        }
        try {
            std::vector<system_device_record> snapshot;
            HRESULT enumeration_status{};
            {
                std::scoped_lock lock{state_->gate};
                enumeration_status = state_->enum_devices_result;
                snapshot = state_->devices;
            }
            if (FAILED(enumeration_status)) {
                return enumeration_status;
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
    if (context.system == nullptr) {
        return E_INVALIDARG;
    }
    if (context.system->controls != nullptr) {
        const HRESULT scripted =
            context.system->controls->observe(native_operation::factory_create);
        if (FAILED(scripted)) {
            return scripted;
        }
    }
    auto* created = new (std::nothrow) system_input{context.system};
    if (created == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = created->QueryInterface(iid, out);
    created->Release();
    return status;
}

HRESULT create_native_system(void* value,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept {
    if (value == nullptr) {
        if (out != nullptr) {
            *out = nullptr;
        }
        return E_INVALIDARG;
    }
    const auto& context = *static_cast<native_factory_context*>(value);
    host_context adapter{};
    adapter.system = context.system;
    return create_system(&adapter, instance, version, iid, out, outer);
}

} // namespace inputnext::dinput_catalogue_test
