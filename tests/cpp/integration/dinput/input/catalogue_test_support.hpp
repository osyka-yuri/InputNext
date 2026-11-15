#pragma once

#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/com/factory.hpp"
#include "dinput/input_access.hpp"
#include "dinput/property_abi.h"

#include <Windows.h>
#include <dinput.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace inputnext::dinput_catalogue_test {

using namespace std::chrono_literals;

struct system_device_record final {
    DIDEVICEINSTANCEW instance{};
    std::wstring path;
    DWORD vendor_product{0x0CE6054CU};
    bool force_feedback{};
    bool attached{true};
};

struct system_input_state final {
    mutable std::mutex gate;
    std::vector<system_device_record> devices;
};

struct host_context final {
    std::shared_ptr<dinput::input_access> access;
    std::shared_ptr<system_input_state> system;
    mutable std::atomic<std::uint32_t> session_requests;
};

[[nodiscard]] inline std::shared_ptr<dinput::input_access> session(void* value) noexcept {
    const auto& context = *static_cast<host_context*>(value);
    context.session_requests.fetch_add(1U, std::memory_order_relaxed);
    return context.access;
}

[[nodiscard]] HRESULT create_system(void* value,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept;

[[nodiscard]] inline DWORD read_vendor_product(IDirectInputDevice8W& device) {
    DIPROPDWORD value{};
    value.diph.dwSize = static_cast<DWORD>(sizeof(value));
    value.diph.dwHeaderSize = static_cast<DWORD>(sizeof(DIPROPHEADER));
    value.diph.dwHow = DIPH_DEVICE;
    const auto status = inputnext_dinput_get_property_w(&device, &DIPROP_VIDPID, &value.diph);
    return SUCCEEDED(status) ? value.dwData : 0U;
}

using root_owner = dinput_test_support::com_owner<IDirectInput8W>;

struct enum_capture final {
    std::vector<DIDEVICEINSTANCEW>* devices{};
};

[[nodiscard]] inline BOOL CALLBACK capture_instance(LPCDIDEVICEINSTANCEW instance,
    LPVOID value) noexcept {
    const auto& capture = *static_cast<enum_capture*>(value);
    if (instance == nullptr || capture.devices == nullptr) {
        return DIENUM_STOP;
    }
    try {
        capture.devices->push_back(*instance);
    } catch (...) {
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

[[nodiscard]] inline std::vector<DIDEVICEINSTANCEW> enumerate(IDirectInput8W& root,
    DWORD device_type = DI8DEVCLASS_GAMECTRL,
    DWORD flags = DIEDFL_ATTACHEDONLY) {
    std::vector<DIDEVICEINSTANCEW> result;
    enum_capture capture{.devices = &result};
    const auto status = root.EnumDevices(device_type, &capture_instance, &capture, flags);
    if (FAILED(status)) {
        return {};
    }
    return result;
}

[[nodiscard]] inline GUID guid(std::uint32_t seed) noexcept {
    return GUID{.Data1 = 0x5a000000U | seed,
        .Data2 = static_cast<WORD>(0x1200U | (seed & 0xffU)),
        .Data3 = 0x4a70U,
        .Data4 = {0x80U, 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U, static_cast<BYTE>(seed)}};
}

[[nodiscard]] inline DIDEVICEINSTANCEW make_system_instance(const GUID instance_guid,
    const wchar_t* name,
    DWORD type = DI8DEVTYPE_GAMEPAD) noexcept {
    DIDEVICEINSTANCEW instance{};
    instance.dwSize = static_cast<DWORD>(sizeof(instance));
    instance.guidInstance = instance_guid;
    instance.guidProduct = GUID{.Data1 = 0x12340000U,
        .Data2 = 0x56U,
        .Data3 = 0x78U,
        .Data4 = {0x90U, 0x12U, 0x34U, 0x56U, 0x78U, 0x9aU, 0xbcU, 0xdeU}};
    instance.dwDevType =
        DIDEVTYPE_HID | static_cast<DWORD>(MAKEWORD(type, DI8DEVTYPEGAMEPAD_STANDARD));
    instance.wUsagePage = 0x01U;
    instance.wUsage = 0x05U;
    wcsncpy_s(instance.tszInstanceName, name, _TRUNCATE);
    wcsncpy_s(instance.tszProductName, name, _TRUNCATE);
    return instance;
}

[[nodiscard]] inline metadata_field<opaque_device_id> available_gameinput_id(BYTE value) noexcept {
    opaque_device_id id{};
    id.bytes[0U] = static_cast<std::byte>(value);
    return metadata_field<opaque_device_id>{.availability = metadata_availability::available,
        .value = id};
}

struct environment final {
    std::unique_ptr<core_test_support::fixture> owners;
    host_context context{};

    environment()
        : owners(std::make_unique<core_test_support::fixture>()),
          system(std::make_shared<system_input_state>()) {
        context.access = std::make_shared<dinput::input_access>(*owners->core,
            *owners->gameinput.runtime,
            *owners->focus_observer);
        context.system = system;
        system_device_record keyboard{};
        keyboard.instance.dwSize = static_cast<DWORD>(sizeof(keyboard.instance));
        keyboard.instance.guidInstance = GUID_SysKeyboard;
        keyboard.instance.guidProduct = GUID_SysKeyboard;
        keyboard.instance.dwDevType =
            DIDEVTYPE_HID |
            static_cast<DWORD>(MAKEWORD(DI8DEVTYPE_KEYBOARD, DI8DEVTYPEKEYBOARD_PCENH));
        wcsncpy_s(keyboard.instance.tszInstanceName, L"System keyboard", _TRUNCATE);
        wcsncpy_s(keyboard.instance.tszProductName, L"System keyboard", _TRUNCATE);
        system->devices.push_back(keyboard);
    }

    [[nodiscard]] dinput::host root_host() noexcept {
        return dinput::host{.session = &session,
            .context = &context,
            .create_system = &create_system};
    }

    [[nodiscard]] root_owner create_root() {
        void* out{};
        const auto root_host_value = root_host();
        if (FAILED(dinput::create_root(root_host_value,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &out,
                nullptr))) {
            throw std::runtime_error{"DirectInput root creation failed"};
        }
        return root_owner{static_cast<IDirectInput8W*>(out)};
    }

    void add_system_gamepad(const GUID instance_guid,
        std::wstring path,
        const bool force_feedback = false,
        const wchar_t* name = L"Native gamepad") const {
        system_device_record record{.instance = make_system_instance(instance_guid, name),
            .path = std::move(path),
            .force_feedback = force_feedback};
        std::scoped_lock lock{system->gate};
        system->devices.push_back(std::move(record));
    }

    [[nodiscard]] connection_target connect(std::size_t index,
        std::optional<std::string> path = std::nullopt,
        const bool fingerprint = true,
        const bool publish_sample = true) const {
        std::u16string endpoint{u"test-endpoint"};
        endpoint.append(index, u'x');
        auto facts = gameinput_test_support::runtime_fixture::native_facts(std::move(endpoint));
        if (path.has_value()) {
            facts.pnp_path_utf8 =
                metadata_field<std::string>{.availability = metadata_availability::available,
                    .value = std::move(*path)};
        }
        if (fingerprint) {
            facts.gameinput_device_id = available_gameinput_id(static_cast<BYTE>(index + 1U));
            facts.gameinput_root_id = available_gameinput_id(static_cast<BYTE>(index + 81U));
        }
        owners->gameinput.native->update_facts(
            gameinput_test_support::handle(owners->gameinput.device_memory[index]),
            std::move(facts));
        return owners->connect(index, publish_sample);
    }

    [[nodiscard]] bool wait_for_catalogue(std::size_t count,
        std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            owners->gameinput.maintain();
            const auto current = owners->core->enumerate();
            if (current.has_value() && current->count == count) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        const auto current = owners->core->enumerate();
        return current.has_value() && current->count == count;
    }

    [[nodiscard]] bool disconnect(std::size_t index,
        connection_target target,
        std::chrono::milliseconds timeout = 2s) const {
        owners->gameinput.native->emit(
            gameinput_test_support::handle(owners->gameinput.device_memory[index]),
            gameinput_detail::native_device_event::disconnected);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            owners->gameinput.maintain();
            const auto latest = owners->core->read_latest(target);
            if (!latest.has_value() && latest.error() == core::status::stale_target) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void shutdown() {
        context.access->close_admission();
        context.access->drain();
        owners.reset();
    }

    std::shared_ptr<system_input_state> system;
};

} // namespace inputnext::dinput_catalogue_test
