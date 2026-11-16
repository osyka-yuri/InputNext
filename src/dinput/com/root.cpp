#include "factory.hpp"

#include "device.hpp"
#include "dinput/catalogue/devices.hpp"
#include "dinput/device_info.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "enumeration.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace inputnext::dinput {
namespace {

[[nodiscard]] HRESULT validate_create_arguments(const HINSTANCE instance,
    const DWORD version) noexcept {
    if (instance == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    if (version < DIRECTINPUT_VERSION) {
        return DIERR_OLDDIRECTINPUTVERSION;
    }
    if (version > DIRECTINPUT_VERSION) {
        return DIERR_BETADIRECTINPUTVERSION;
    }
    return S_OK;
}

[[nodiscard]] bool is_zero_guid(const GUID& value) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_builtin_system_input_guid(const GUID& instance) noexcept {
    return instance == GUID_SysKeyboard || instance == GUID_SysMouse ||
           instance == GUID_SysKeyboardEm || instance == GUID_SysKeyboardEm2 ||
           instance == GUID_SysMouseEm || instance == GUID_SysMouseEm2;
}

[[nodiscard]] bool same_text(const wchar_t* left, const std::wstring& right) noexcept {
    std::size_t length{};
    while (length < MAX_PATH && left[length] != L'\0') {
        ++length;
    }
    if (length == 0U || length == MAX_PATH || right.empty() ||
        length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left,
               static_cast<int>(length),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::optional<std::wstring> ansi_to_wide(const char* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto required = MultiByteToWideChar(CP_ACP, 0U, value, -1, nullptr, 0);
    if (required <= 1) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0U, value, -1, result.data(), required) != required) {
        return std::nullopt;
    }
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

// Callers retain their COM reference through each call. Enumeration also retains
// this object across application callbacks that may release the caller's reference.
class direct_input_root final : public IDirectInput8A, public IDirectInput8W {
public:
    direct_input_root(const direct_input_root&) = delete;
    direct_input_root& operator=(const direct_input_root&) = delete;
    direct_input_root(direct_input_root&&) = delete;
    direct_input_root& operator=(direct_input_root&&) = delete;

    explicit direct_input_root(host supplied_host) noexcept : host_(std::move(supplied_host)) {
        object_created();
    }

    ~direct_input_root() {
        if (system_ != nullptr) {
            system_->Release();
            system_ = nullptr;
        }
        object_destroyed();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirectInput8A)) {
            *out = static_cast<IDirectInput8A*>(this);
        } else if (IsEqualIID(iid, IID_IDirectInput8W)) {
            *out = static_cast<IDirectInput8W*>(this);
        } else {
            return E_NOINTERFACE;
        }
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
        LPDIRECTINPUTDEVICE8A* out,
        LPUNKNOWN outer) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        void* device{};
        const auto status = create_device(instance, &device, outer, false);
        *out = static_cast<LPDIRECTINPUTDEVICE8A>(device);
        return status;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID instance,
        LPDIRECTINPUTDEVICE8W* out,
        LPUNKNOWN outer) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        void* device{};
        const auto status = create_device(instance, &device, outer, true);
        *out = static_cast<LPDIRECTINPUTDEVICE8W>(device);
        return status;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD device_type,
        LPDIENUMDEVICESCALLBACKA callback,
        LPVOID context,
        DWORD flags) noexcept override {
        return enum_devices(device_type, callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD device_type,
        LPDIENUMDEVICESCALLBACKW callback,
        LPVOID context,
        DWORD flags) noexcept override {
        return enum_devices(device_type, callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID instance) noexcept override {
        auto* system = native_reference();
        if (system == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        const auto native_status = system->GetDeviceStatus(instance);
        if (native_status == DI_OK || is_builtin_system_input_guid(instance)) {
            return native_status;
        }
        device_identity ignored{};
        if (find_normalized(*system, instance, ignored) == S_OK) {
            return DI_OK;
        }
        return native_status;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) noexcept override {
        auto* system = native_reference();
        if (system == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        return system->RunControlPanel(owner, flags);
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) noexcept override {
        if (initialization_.load(std::memory_order_acquire) != 0U) {
            return DIERR_ALREADYINITIALIZED;
        }
        const auto validation = validate_create_arguments(instance, version);
        if (FAILED(validation)) {
            return validation;
        }

        std::uint32_t expected{};
        if (!initialization_.compare_exchange_strong(expected,
                1U,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return DIERR_ALREADYINITIALIZED;
        }

        IDirectInput8W* system{};
        HRESULT status = E_NOTIMPL;
        if (host_.create_system != nullptr) {
            void* system_value{};
            status = host_.create_system(host_.context,
                instance,
                version,
                IID_IDirectInput8W,
                &system_value,
                nullptr);
            system = static_cast<IDirectInput8W*>(system_value);
        }
        if (FAILED(status) || system == nullptr) {
            if (system != nullptr) {
                system->Release();
            }
            initialization_.store(0U, std::memory_order_release);
            return FAILED(status) ? status : E_NOINTERFACE;
        }

        system_ = system;
        instance_ = instance;
        if (host_.is_runtime_provider != nullptr &&
            host_.is_runtime_provider(host_.context, instance)) {
            // The provider must finish its own enumeration before normalized discovery can succeed.
            host_.session = nullptr;
        }
        initialization_.store(2U, std::memory_order_release);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID device_class,
        LPCSTR name,
        LPGUID instance) noexcept override {
        if (instance == nullptr || name == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        *instance = {};
        try {
            auto wide_name = ansi_to_wide(name);
            if (!wide_name.has_value()) {
                return DIERR_INVALIDPARAM;
            }
            return find_device(device_class, *wide_name, instance);
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }
    }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID device_class,
        LPCWSTR name,
        LPGUID instance) noexcept override {
        if (instance == nullptr || name == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        *instance = {};
        try {
            return find_device(device_class, std::wstring{name}, instance);
        } catch (const std::bad_alloc&) {
            return DIERR_OUTOFMEMORY;
        } catch (...) {
            return DIERR_GENERIC;
        }
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR /*unused*/,
        LPDIACTIONFORMATA format,
        LPDIENUMDEVICESBYSEMANTICSCBA callback,
        LPVOID /*unused*/,
        DWORD flags) noexcept override {
        if (format == nullptr || callback == nullptr || format->dwSize != sizeof(*format) ||
            (flags & ~static_cast<DWORD>(DIEDBSFL_VALID)) != 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (initialization_.load(std::memory_order_acquire) != 2U) {
            return DIERR_NOTINITIALIZED;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR /*unused*/,
        LPDIACTIONFORMATW format,
        LPDIENUMDEVICESBYSEMANTICSCBW callback,
        LPVOID /*unused*/,
        DWORD flags) noexcept override {
        if (format == nullptr || callback == nullptr || format->dwSize != sizeof(*format) ||
            (flags & ~static_cast<DWORD>(DIEDBSFL_VALID)) != 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (initialization_.load(std::memory_order_acquire) != 2U) {
            return DIERR_NOTINITIALIZED;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK /*unused*/,
        LPDICONFIGUREDEVICESPARAMSA parameters,
        DWORD flags,
        LPVOID /*unused*/) noexcept override {
        if (parameters == nullptr || parameters->dwSize != sizeof(*parameters) ||
            (flags & ~static_cast<DWORD>(DICD_EDIT)) != 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (initialization_.load(std::memory_order_acquire) != 2U) {
            return DIERR_NOTINITIALIZED;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK /*unused*/,
        LPDICONFIGUREDEVICESPARAMSW parameters,
        DWORD flags,
        LPVOID /*unused*/) noexcept override {
        if (parameters == nullptr || parameters->dwSize != sizeof(*parameters) ||
            (flags & ~static_cast<DWORD>(DICD_EDIT)) != 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (initialization_.load(std::memory_order_acquire) != 2U) {
            return DIERR_NOTINITIALIZED;
        }
        return DIERR_UNSUPPORTED;
    }

private:
    template <typename Interface>
    class com_reference final {
    public:
        explicit com_reference(Interface* value) noexcept : value_(value) {
            if (value_ != nullptr) {
                value_->AddRef();
            }
        }

        ~com_reference() {
            if (value_ != nullptr) {
                value_->Release();
            }
        }

        com_reference(const com_reference&) = delete;
        com_reference& operator=(const com_reference&) = delete;
        com_reference(com_reference&&) = delete;
        com_reference& operator=(com_reference&&) = delete;

    private:
        Interface* value_{};
    };

    [[nodiscard]] IDirectInput8W* native_reference() noexcept {
        if (initialization_.load(std::memory_order_acquire) != 2U || system_ == nullptr) {
            return nullptr;
        }
        return system_;
    }

    [[nodiscard]] HRESULT
    create_device(REFGUID instance, void** out, IUnknown* outer, const bool unicode) noexcept {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const com_reference root_reference{this};
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* system = native_reference();
        if (system == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        com_reference const system_reference{system};

        void* native_device{};
        auto const status = create_native_device(*system, instance, unicode, &native_device);
        if (SUCCEEDED(status) && native_device != nullptr) {
            auto* device_unknown = static_cast<IUnknown*>(native_device);
            DIDEVCAPS capabilities{};
            capabilities.dwSize = static_cast<DWORD>(sizeof(capabilities));
            IDirectInputDevice8W* inspection{};
            void* inspection_value{};
            if (SUCCEEDED(
                    device_unknown->QueryInterface(IID_IDirectInputDevice8W, &inspection_value)) &&
                inspection_value != nullptr) {
                inspection = static_cast<IDirectInputDevice8W*>(inspection_value);
                const auto caps_status = inspection->GetCapabilities(&capabilities);
                inspection->Release();
                if (SUCCEEDED(caps_status) && is_game_controller_type(capabilities.dwDevType)) {
                    const auto normalized = try_create_normalized(*system, instance, out, unicode);
                    if (normalized != S_FALSE) {
                        device_unknown->Release();
                        return normalized;
                    }
                }
            }
            *out = native_device;
            return status;
        }

        if (native_device != nullptr) {
            static_cast<IUnknown*>(native_device)->Release();
        }
        if (!is_builtin_system_input_guid(instance)) {
            const auto normalized = try_create_normalized(*system, instance, out, unicode);
            if (normalized != S_FALSE) {
                return normalized;
            }
        }
        return status;
    }

    [[nodiscard]] static HRESULT create_native_device(IDirectInput8W& system,
        REFGUID instance,
        const bool unicode,
        void** out) noexcept {
        *out = nullptr;
        if (unicode) {
            IDirectInputDevice8W* device{};
            const auto status = system.CreateDevice(instance, &device, nullptr);
            *out = device;
            return status;
        }

        void* ansi_system_value{};
        const auto query_status = system.QueryInterface(IID_IDirectInput8A, &ansi_system_value);
        auto* ansi_system = static_cast<IDirectInput8A*>(ansi_system_value);
        if (FAILED(query_status) || ansi_system == nullptr) {
            return FAILED(query_status) ? query_status : E_NOINTERFACE;
        }
        IDirectInputDevice8A* device{};
        const auto status = ansi_system->CreateDevice(instance, &device, nullptr);
        ansi_system->Release();
        *out = device;
        return status;
    }

    [[nodiscard]] HRESULT try_create_normalized(IDirectInput8W& system,
        REFGUID instance,
        void** out,
        const bool unicode) noexcept {
        auto shared_access = host_.session == nullptr ? nullptr : host_.session(host_.context);
        if (shared_access == nullptr) {
            return S_FALSE;
        }
        device_identity identity{};
        const auto status = discover_projected_device(*shared_access, system, instance, identity);
        if (status != S_OK) {
            return S_FALSE;
        }
        auto& canonical_parent = static_cast<IUnknown&>(*static_cast<IDirectInput8A*>(this));
        native_input_factory factory{.create = host_.create_system,
            .context = host_.create_system == &create_system_input ? nullptr : host_.context,
            .instance = instance_,
            .lifetime = host_.system_lifetime};
        return make_device(identity,
            std::move(shared_access),
            canonical_parent,
            unicode,
            out,
            std::move(factory));
    }

    [[nodiscard]] HRESULT
    find_normalized(IDirectInput8W& system, REFGUID instance, device_identity& out) const noexcept {
        if (host_.session == nullptr) {
            return DIERR_INPUTLOST;
        }
        auto const shared_access = host_.session(host_.context);
        if (shared_access == nullptr) {
            return DIERR_INPUTLOST;
        }
        auto access = shared_access->enter();
        if (!access) {
            return DIERR_INPUTLOST;
        }
        return find_projected_device(std::move(access), system, instance, out);
    }

    template <typename Callback>
    [[nodiscard]] HRESULT
    enum_devices(DWORD device_type, Callback callback, LPVOID context, DWORD flags) noexcept {
        if (callback == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        const com_reference root_reference{this};
        auto* system = native_reference();
        if (system == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        const com_reference system_reference{system};
        return detail::enumerate_devices(host_,
            instance_,
            *system,
            device_type,
            callback,
            context,
            flags);
    }

    [[nodiscard]] HRESULT
    find_device(REFGUID device_class, const std::wstring& name, LPGUID instance) noexcept {
        auto* system = native_reference();
        if (system == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        com_reference const system_reference{system};

        GUID native_instance{};
        const auto native_status = system->FindDevice(device_class, name.c_str(), &native_instance);
        if (SUCCEEDED(native_status)) {
            *instance = native_instance;
            return native_status;
        }
        if (!is_zero_guid(device_class) && (IsEqualGUID(device_class, GUID_Joystick) == 0)) {
            return native_status;
        }
        if (host_.session == nullptr) {
            return native_status;
        }
        auto const shared_access = host_.session(host_.context);
        if (shared_access == nullptr) {
            return native_status;
        }
        catalogue_projection projection{};
        HRESULT project_status{};
        {
            auto access = shared_access->enter();
            if (!access) {
                return native_status;
            }
            project_status = project_catalogue(std::move(access), *system, projection);
        }
        if (FAILED(project_status)) {
            return native_status;
        }
        const auto found = std::ranges::find_if(projection.devices, [&name](const auto& device) {
            return same_text(device.instance.tszInstanceName, name) ||
                   same_text(device.instance.tszProductName, name);
        });
        if (found == projection.devices.end()) {
            return native_status;
        }
        *instance = found->instance.guidInstance;
        return S_OK;
    }

    std::atomic<ULONG> references_{1U};
    std::atomic<std::uint32_t> initialization_;
    host host_{};
    IDirectInput8W* system_{};
    HINSTANCE instance_{};
};

} // namespace

HRESULT create_root(const host& root_host,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* const outer) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }
    const auto validation = validate_create_arguments(instance, version);
    if (FAILED(validation)) {
        return validation;
    }

    auto* root = new (std::nothrow) direct_input_root{root_host};
    if (root == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto init_status = root->Initialize(instance, version);
    if (FAILED(init_status)) {
        root->Release();
        return init_status;
    }
    const auto query_status = root->QueryInterface(iid, out);
    root->Release();
    return query_status;
}

namespace detail {

HRESULT create_uninitialized_root(const host& root_host,
    IUnknown* const outer,
    REFIID iid,
    void** out) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }

    auto* root = new (std::nothrow) direct_input_root{root_host};
    if (root == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = root->QueryInterface(iid, out);
    root->Release();
    return status;
}

} // namespace detail

} // namespace inputnext::dinput
