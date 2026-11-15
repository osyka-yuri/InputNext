#include "dinput/com/factory.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>

namespace {

using namespace inputnext::dinput;

struct fake_system_state final {
    std::uint32_t creates{};
    std::uint32_t device_creates{};
    std::uint32_t device_status_checks{};
    std::uint32_t enumerations{};
    std::uint32_t finds{};
    std::uint32_t initializes{};
    std::uint32_t control_panels{};
    std::uint32_t destroyed{};
    std::uint32_t session_requests{};
    DWORD last_enum_device_type{};
    std::uint32_t enum_native_visits{};
    std::uint32_t enum_items{1U};
    std::uint32_t create_failures{};
    std::uint32_t provider_classifications{};
    HINSTANCE classified_instance{};
    bool runtime_provider{};
    bool fail_enum_after_first{};
    GUID keyboard{.Data1 = 0x6F1D2B61U,
        .Data2 = 0xD5A0U,
        .Data3 = 0x11CFU,
        .Data4 = {0xBFU, 0xC7U, 0x44U, 0x45U, 0x53U, 0x54U, 0x00U, 0x00U}};
    GUID attached{.Data1 = 0x5A5A1234U,
        .Data2 = 0x1234U,
        .Data3 = 0x5678U,
        .Data4 = {0x90U, 0xABU, 0xCDU, 0xEFU, 0x12U, 0x34U, 0x56U, 0x78U}};
};

class fake_system_input final : public IDirectInput8W {
public:
    fake_system_input(const fake_system_input&) = delete;
    fake_system_input& operator=(const fake_system_input&) = delete;
    fake_system_input(fake_system_input&&) = delete;
    fake_system_input& operator=(fake_system_input&&) = delete;

    explicit fake_system_input(fake_system_state& state) noexcept : state_(state) {}

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

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID,
        LPDIRECTINPUTDEVICE8W* out,
        LPUNKNOWN /*unused*/) noexcept override {
        ++state_.device_creates;
        if (out != nullptr) {
            *out = nullptr;
        }
        return DIERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD device_type,
        LPDIENUMDEVICESCALLBACKW callback,
        LPVOID context,
        DWORD /*unused*/) noexcept override {
        ++state_.enumerations;
        state_.last_enum_device_type = device_type;
        if (callback == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        DIDEVICEINSTANCEW instance{};
        instance.dwSize = static_cast<DWORD>(sizeof(instance));
        const bool game_controller = device_type == DI8DEVCLASS_GAMECTRL;
        instance.guidInstance = game_controller ? state_.attached : state_.keyboard;
        instance.guidProduct = instance.guidInstance;
        instance.dwDevType =
            game_controller
                ? static_cast<DWORD>(MAKEWORD(DI8DEVTYPE_GAMEPAD, DI8DEVTYPEGAMEPAD_STANDARD))
                : MAKEWORD(DI8DEVTYPE_KEYBOARD, DI8DEVTYPEKEYBOARD_PCENH);
        constexpr std::wstring_view keyboard_name = L"Fixture keyboard";
        constexpr std::wstring_view game_controller_name = L"Fixture game controller";
        const auto name = game_controller ? game_controller_name : keyboard_name;
        std::ranges::copy(name, std::begin(instance.tszInstanceName));
        std::ranges::copy(name, std::begin(instance.tszProductName));
        const auto item_count = std::max(state_.enum_items, 1U);
        for (std::uint32_t index = 0U; index < item_count; ++index) {
            if (index != 0U) {
                ++instance.guidInstance.Data1;
                instance.guidProduct = instance.guidInstance;
            }
            ++state_.enum_native_visits;
            if (callback(&instance, context) == DIENUM_STOP) {
                return DI_OK;
            }
            if (index == 0U && state_.fail_enum_after_first) {
                return E_FAIL;
            }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID instance) noexcept override {
        ++state_.device_status_checks;
        if (instance == state_.keyboard || instance == state_.attached) {
            return DI_OK;
        }
        return DIERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND /*unused*/, DWORD /*unused*/) noexcept override {
        ++state_.control_panels;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE /*unused*/, DWORD /*unused*/) noexcept override {
        ++state_.initializes;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID,
        LPCWSTR /*unused*/,
        LPGUID instance) noexcept override {
        ++state_.finds;
        if (instance != nullptr) {
            *instance = {};
        }
        return DIERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR /*unused*/,
        LPDIACTIONFORMATW /*unused*/,
        LPDIENUMDEVICESBYSEMANTICSCBW /*unused*/,
        LPVOID /*unused*/,
        DWORD /*unused*/) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK /*unused*/,
        LPDICONFIGUREDEVICESPARAMSW /*unused*/,
        DWORD /*unused*/,
        LPVOID /*unused*/) noexcept override {
        return DIERR_UNSUPPORTED;
    }

private:
    ~fake_system_input() { ++state_.destroyed; }

    std::atomic<ULONG> references_{1U};
    fake_system_state& state_;
};

HRESULT create_fake_system(void* context,
    HINSTANCE /*unused*/,
    DWORD /*unused*/,
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
    auto& state = *static_cast<fake_system_state*>(context);
    ++state.creates;
    if (state.create_failures != 0U) {
        --state.create_failures;
        return E_FAIL;
    }
    auto* system = new (std::nothrow) fake_system_input{state};
    if (system == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = system->QueryInterface(iid, out);
    system->Release();
    return status;
}

std::shared_ptr<input_access> request_fake_session(void* context) noexcept {
    ++static_cast<fake_system_state*>(context)->session_requests;
    return {};
}

bool classify_runtime_provider(void* context, HINSTANCE instance) noexcept {
    auto& state = *static_cast<fake_system_state*>(context);
    ++state.provider_classifications;
    state.classified_instance = instance;
    return state.runtime_provider;
}

struct callback_state final {
    IDirectInput8A* root{};
    std::uint32_t calls{};
    GUID seen{};
};

struct provider_enum_state final {
    fake_system_state* system{};
    IDirectInput8W* root{};
    std::uint32_t calls{};
    std::uint32_t native_visits_at_callback{};
    std::uint32_t root_release_result{};
    BOOL result{DIENUM_CONTINUE};
    bool release_root{};
};

BOOL CALLBACK provider_enum_callback(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    auto& state = *static_cast<provider_enum_state*>(context);
    if (instance == nullptr) {
        return DIENUM_STOP;
    }
    ++state.calls;
    state.native_visits_at_callback = state.system->enum_native_visits;
    if (state.release_root) {
        auto* root = state.root;
        state.root = nullptr;
        state.root_release_result = root->Release();
    }
    return state.result;
}

BOOL CALLBACK throwing_enum_callback(LPCDIDEVICEINSTANCEW /*unused*/, LPVOID context) {
    ++*static_cast<std::uint32_t*>(context);
    throw std::runtime_error{"Fixture enumeration failure"};
}

struct provider_ansi_enum_state final {
    std::uint32_t calls{};
    BOOL result{DIENUM_CONTINUE};
};

BOOL CALLBACK provider_ansi_enum_callback(LPCDIDEVICEINSTANCEA instance, LPVOID context) {
    auto& state = *static_cast<provider_ansi_enum_state*>(context);
    if (instance == nullptr) {
        return DIENUM_STOP;
    }
    ++state.calls;
    return state.result;
}

BOOL CALLBACK reentrant_enum_callback(LPCDIDEVICEINSTANCEA instance, LPVOID context) {
    auto& state = *static_cast<callback_state*>(context);
    ++state.calls;
    state.seen = instance->guidInstance;
    void* unicode{};
    const auto status = state.root->QueryInterface(IID_IDirectInput8W, &unicode);
    if (FAILED(status) || unicode == nullptr) {
        return DIENUM_STOP;
    }
    static_cast<IDirectInput8W*>(unicode)->Release();
    return DIENUM_STOP;
}

[[nodiscard]] host make_host(fake_system_state& state,
    const bool classify_provider = false) noexcept {
    return host{.session = &request_fake_session,
        .context = &state,
        .create_system = &create_fake_system,
        .is_runtime_provider = classify_provider ? &classify_runtime_provider : nullptr};
}

[[nodiscard]] HINSTANCE make_fake_instance(const std::uintptr_t value) noexcept {
    return std::bit_cast<HINSTANCE>(value);
}

BOOL CALLBACK count_enum_callback(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    if (instance == nullptr || context == nullptr) {
        return DIENUM_STOP;
    }
    ++*static_cast<std::uint32_t*>(context);
    return DIENUM_CONTINUE;
}

TEST_CASE("dinput.root.canonical_a_w_identity", "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state);
    void* ansi_value{};
    REQUIRE(create_root(root_host,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                &ansi_value,
                nullptr) == S_OK);
    auto* ansi = static_cast<IDirectInput8A*>(ansi_value);

    void* wide_value{};
    REQUIRE(ansi->QueryInterface(IID_IDirectInput8W, &wide_value) == S_OK);
    auto* wide = static_cast<IDirectInput8W*>(wide_value);
    void* ansi_unknown{};
    void* wide_unknown{};
    REQUIRE(ansi->QueryInterface(IID_IUnknown, &ansi_unknown) == S_OK);
    REQUIRE(wide->QueryInterface(IID_IUnknown, &wide_unknown) == S_OK);
    CHECK(ansi_unknown == wide_unknown);
    CHECK(state.creates == 1U);
    CHECK(ansi->Initialize(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION) ==
          DIERR_ALREADYINITIALIZED);

    static_cast<IUnknown*>(wide_unknown)->Release();
    static_cast<IUnknown*>(ansi_unknown)->Release();
    wide->Release();
    ansi->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.class_factory_and_server_locks", "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state);
    void* factory_value{};
    REQUIRE(
        get_class_object(root_host, CLSID_DirectInput8, IID_IClassFactory, &factory_value) == S_OK);
    auto* factory = static_cast<IClassFactory*>(factory_value);
    CHECK(can_unload() == S_FALSE);

    CHECK(factory->LockServer(TRUE) == S_OK);
    CHECK(can_unload() == S_FALSE);
    CHECK(factory->LockServer(FALSE) == S_OK);
    CHECK(factory->LockServer(FALSE) == E_UNEXPECTED);

    auto* const outer = static_cast<IUnknown*>(factory);
    void* rejected{};
    CHECK(factory->CreateInstance(outer, IID_IDirectInput8A, &rejected) == CLASS_E_NOAGGREGATION);
    CHECK(rejected == nullptr);

    void* root_value{};
    REQUIRE(factory->CreateInstance(nullptr, IID_IDirectInput8A, &root_value) == S_OK);
    auto* root = static_cast<IDirectInput8A*>(root_value);
    CHECK(state.creates == 0U);
    CHECK(root->Initialize(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION) == S_OK);
    CHECK(state.creates == 1U);
    root->Release();
    factory->Release();
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.reentrant_enumeration_and_system_passthrough", "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state);
    void* root_value{};
    REQUIRE(create_root(root_host,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                &root_value,
                nullptr) == S_OK);
    auto* root = static_cast<IDirectInput8A*>(root_value);
    callback_state callback{.root = root};
    CHECK(root->EnumDevices(DI8DEVCLASS_KEYBOARD,
              &reentrant_enum_callback,
              &callback,
              DIEDFL_ALLDEVICES) == DI_OK);
    CHECK(callback.calls == 1U);
    CHECK(IsEqualGUID(callback.seen, state.keyboard));
    CHECK(state.session_requests == 0U);
    void* wide_value{};
    REQUIRE(root->QueryInterface(IID_IDirectInput8W, &wide_value) == S_OK);
    auto* wide_root = static_cast<IDirectInput8W*>(wide_value);
    LPDIRECTINPUTDEVICE8W keyboard{};
    CHECK(wide_root->CreateDevice(state.keyboard, &keyboard, nullptr) == DIERR_NOTFOUND);
    CHECK(keyboard == nullptr);
    CHECK(state.session_requests == 0U);
    CHECK(root->EnumDevices(DI8DEVCLASS_ALL,
              &reentrant_enum_callback,
              &callback,
              DIEDFL_ALLDEVICES) == DI_OK);
    CHECK(state.session_requests == 1U);
    CHECK(callback.calls == 2U);
    wide_root->Release();
    root->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.device_status_avoids_opening_native_devices", "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state);
    void* root_value{};
    REQUIRE(create_root(root_host,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &root_value,
                nullptr) == S_OK);
    auto* root = static_cast<IDirectInput8W*>(root_value);

    CHECK(root->GetDeviceStatus(state.attached) == DI_OK);
    CHECK(root->GetDeviceStatus(GUID_SysKeyboard) == DI_OK);
    const std::array builtins{GUID_SysMouse,
        GUID_SysKeyboardEm,
        GUID_SysKeyboardEm2,
        GUID_SysMouseEm,
        GUID_SysMouseEm2};
    for (const auto& instance : builtins) {
        CHECK(root->GetDeviceStatus(instance) == DIERR_NOTFOUND);
    }
    CHECK(state.device_creates == 0U);
    CHECK(state.session_requests == 0U);

    const GUID unknown{.Data1 = 0x76543210U,
        .Data2 = 0x2345U,
        .Data3 = 0x6789U,
        .Data4 = {0xA0U, 0xB1U, 0xC2U, 0xD3U, 0xE4U, 0xF5U, 0x06U, 0x17U}};
    CHECK(root->GetDeviceStatus(unknown) == DIERR_NOTFOUND);
    CHECK(state.session_requests == 1U);
    CHECK(state.device_creates == 0U);

    root->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.create_validates_before_system_loading", "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state);
    void* output = &state;
    CHECK(create_root(root_host,
              nullptr,
              DIRECTINPUT_VERSION,
              IID_IDirectInput8A,
              &output,
              nullptr) == DIERR_INVALIDPARAM);
    CHECK(output == nullptr);
    CHECK(state.creates == 0U);
    CHECK(state.session_requests == 0U);

    CHECK(create_root(root_host,
              GetModuleHandleW(nullptr),
              DIRECTINPUT_VERSION - 1U,
              IID_IDirectInput8A,
              &output,
              nullptr) == DIERR_OLDDIRECTINPUTVERSION);
    CHECK(create_root(root_host,
              GetModuleHandleW(nullptr),
              DIRECTINPUT_VERSION + 1U,
              IID_IDirectInput8A,
              &output,
              nullptr) == DIERR_BETADIRECTINPUTVERSION);
    CHECK(state.creates == 0U);
}

TEST_CASE("dinput.root.provider_classification_keeps_factory_root_native_only",
    "[component][dinput][com]") {
    fake_system_state state{.create_failures = 1U, .runtime_provider = true};
    const auto root_host = make_host(state, true);
    void* factory_value{};
    REQUIRE(
        get_class_object(root_host, CLSID_DirectInput8, IID_IClassFactory, &factory_value) == S_OK);
    auto* factory = static_cast<IClassFactory*>(factory_value);

    void* root_value{};
    REQUIRE(factory->CreateInstance(nullptr, IID_IDirectInput8W, &root_value) == S_OK);
    auto* root = static_cast<IDirectInput8W*>(root_value);
    CHECK(state.creates == 0U);

    HINSTANCE const failed_instance = make_fake_instance(0x1234U);
    HINSTANCE const provider_instance = make_fake_instance(0x5678U);
    CHECK(root->Initialize(failed_instance, DIRECTINPUT_VERSION) == E_FAIL);
    CHECK(state.provider_classifications == 0U);
    CHECK(root->Initialize(provider_instance, DIRECTINPUT_VERSION) == S_OK);
    CHECK(state.provider_classifications == 1U);
    CHECK(state.classified_instance == provider_instance);
    CHECK(root->Initialize(make_fake_instance(0x9ABCU), DIRECTINPUT_VERSION) ==
          DIERR_ALREADYINITIALIZED);
    CHECK(state.provider_classifications == 1U);
    CHECK(state.classified_instance == provider_instance);

    std::uint32_t enumerated{};
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &count_enum_callback,
              &enumerated,
              DIEDFL_ALLDEVICES) == DI_OK);
    LPDIRECTINPUTDEVICE8W device{};
    CHECK(root->CreateDevice(state.attached, &device, nullptr) == DIERR_NOTFOUND);
    CHECK(device == nullptr);
    const GUID unknown{.Data1 = 0x76543210U,
        .Data2 = 0x2345U,
        .Data3 = 0x6789U,
        .Data4 = {0xA0U, 0xB1U, 0xC2U, 0xD3U, 0xE4U, 0xF5U, 0x06U, 0x17U}};
    CHECK(root->GetDeviceStatus(unknown) == DIERR_NOTFOUND);
    GUID found{};
    CHECK(root->FindDevice(GUID_Joystick, L"Missing controller", &found) == DIERR_NOTFOUND);
    CHECK(IsEqualGUID(found, GUID_NULL));

    CHECK(enumerated == 1U);
    CHECK(state.enumerations == 1U);
    CHECK(state.last_enum_device_type == DI8DEVCLASS_GAMECTRL);
    CHECK(state.device_creates == 1U);
    CHECK(state.device_status_checks == 1U);
    CHECK(state.finds == 1U);
    CHECK(state.session_requests == 0U);

    root->Release();
    factory->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.nonprovider_classification_preserves_normalized_fallback",
    "[component][dinput][com]") {
    fake_system_state state{};
    const auto root_host = make_host(state, true);
    HINSTANCE const instance = make_fake_instance(0x2468U);
    void* root_value{};
    REQUIRE(create_root(root_host,
                instance,
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &root_value,
                nullptr) == S_OK);
    auto* root = static_cast<IDirectInput8W*>(root_value);
    CHECK(state.provider_classifications == 1U);
    CHECK(state.classified_instance == instance);

    std::uint32_t enumerated{};
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &count_enum_callback,
              &enumerated,
              DIEDFL_ALLDEVICES) == DI_OK);
    LPDIRECTINPUTDEVICE8W device{};
    CHECK(root->CreateDevice(state.attached, &device, nullptr) == DIERR_NOTFOUND);
    CHECK(device == nullptr);
    const GUID unknown{.Data1 = 0x76543210U,
        .Data2 = 0x2345U,
        .Data3 = 0x6789U,
        .Data4 = {0xA0U, 0xB1U, 0xC2U, 0xD3U, 0xE4U, 0xF5U, 0x06U, 0x17U}};
    CHECK(root->GetDeviceStatus(unknown) == DIERR_NOTFOUND);
    GUID found{};
    CHECK(root->FindDevice(GUID_Joystick, L"Missing controller", &found) == DIERR_NOTFOUND);
    CHECK(IsEqualGUID(found, GUID_NULL));

    CHECK(enumerated == 1U);
    CHECK(state.enumerations == 1U);
    CHECK(state.last_enum_device_type == DI8DEVCLASS_GAMECTRL);
    CHECK(state.device_creates == 1U);
    CHECK(state.device_status_checks == 1U);
    CHECK(state.finds == 1U);
    CHECK(state.session_requests == 4U);

    root->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.provider_enumeration_forwards_callbacks_and_stop",
    "[component][dinput][com]") {
    fake_system_state state{.enum_items = 2U,
        .runtime_provider = true,
        .fail_enum_after_first = true};
    const auto root_host = make_host(state, true);
    HINSTANCE const instance = make_fake_instance(0x3456U);
    void* root_value{};
    REQUIRE(create_root(root_host,
                instance,
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &root_value,
                nullptr) == S_OK);
    auto* root = static_cast<IDirectInput8W*>(root_value);

    provider_enum_state stop_state{.system = &state, .root = root, .result = DIENUM_STOP};
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &provider_enum_callback,
              &stop_state,
              DIEDFL_ALLDEVICES) == DI_OK);
    CHECK(stop_state.calls == 1U);
    CHECK(stop_state.native_visits_at_callback == 1U);
    CHECK(state.enum_native_visits == 1U);

    state.enum_native_visits = 0U;
    provider_enum_state continue_state{.system = &state, .root = root, .result = DIENUM_CONTINUE};
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &provider_enum_callback,
              &continue_state,
              DIEDFL_ALLDEVICES) == E_FAIL);
    CHECK(continue_state.calls == 1U);
    CHECK(continue_state.native_visits_at_callback == 1U);
    CHECK(state.enum_native_visits == 1U);
    CHECK(state.session_requests == 0U);

    provider_ansi_enum_state ansi_state{.result = DIENUM_STOP};
    void* ansi_value{};
    REQUIRE(root->QueryInterface(IID_IDirectInput8A, &ansi_value) == S_OK);
    auto* ansi = static_cast<IDirectInput8A*>(ansi_value);
    state.enum_native_visits = 0U;
    CHECK(ansi->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &provider_ansi_enum_callback,
              &ansi_state,
              DIEDFL_ALLDEVICES) == DI_OK);
    CHECK(ansi_state.calls == 1U);
    CHECK(state.enum_native_visits == 1U);
    ansi->Release();

    provider_enum_state release_state{.system = &state,
        .root = root,
        .result = DIENUM_STOP,
        .release_root = true};
    state.fail_enum_after_first = false;
    state.enum_native_visits = 0U;
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &provider_enum_callback,
              &release_state,
              DIEDFL_ALLDEVICES) == DI_OK);
    CHECK(release_state.calls == 1U);
    CHECK(release_state.root_release_result == 1U);
    CHECK(state.enum_native_visits == 1U);
    CHECK(state.session_requests == 0U);
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

TEST_CASE("dinput.root.provider_enumeration_translates_callback_exceptions",
    "[component][dinput][com]") {
    fake_system_state state{.runtime_provider = true};
    const auto root_host = make_host(state, true);
    HINSTANCE const instance = make_fake_instance(0x4567U);
    void* root_value{};
    REQUIRE(create_root(root_host,
                instance,
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &root_value,
                nullptr) == S_OK);
    auto* root = static_cast<IDirectInput8W*>(root_value);
    std::uint32_t callbacks{};
    CHECK(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
              &throwing_enum_callback,
              &callbacks,
              DIEDFL_ALLDEVICES) == DIERR_GENERIC);
    CHECK(callbacks == 1U);
    CHECK(state.enum_native_visits == 1U);
    CHECK(state.session_requests == 0U);
    root->Release();
    CHECK(state.destroyed == 1U);
    CHECK(can_unload() == S_OK);
}

} // namespace
