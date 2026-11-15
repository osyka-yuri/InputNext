#include "catalogue_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/property_abi.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <dinput.h>
#include <inputnext/device_identity.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;

namespace {

[[nodiscard]] std::vector<GUID> instance_guids(const std::vector<DIDEVICEINSTANCEW>& devices) {
    std::vector<GUID> result;
    result.reserve(devices.size());
    for (const auto& device : devices) {
        result.push_back(device.guidInstance);
    }
    return result;
}

[[nodiscard]] bool contains_guid(const std::vector<DIDEVICEINSTANCEW>& devices,
    const GUID& expected) noexcept {
    return std::ranges::any_of(devices,
        [&expected](const auto& device) { return IsEqualGUID(device.guidInstance, expected); });
}

[[nodiscard]] IDirectInputDevice8W* create_device(IDirectInput8W& root, const GUID& guid) {
    IDirectInputDevice8W* result{};
    const auto status = root.CreateDevice(guid, &result, nullptr);
    if (FAILED(status)) {
        return nullptr;
    }
    return result;
}

[[nodiscard]] DIPROPGUIDANDPATH path_request() noexcept {
    DIPROPGUIDANDPATH value{};
    value.diph.dwSize = data_size<DIPROPGUIDANDPATH>();
    value.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    value.diph.dwHow = DIPH_DEVICE;
    return value;
}

BOOL CALLBACK capture_ansi_instance(LPCDIDEVICEINSTANCEA instance, LPVOID context) noexcept {
    *static_cast<DIDEVICEINSTANCEA*>(context) = *instance;
    return DIENUM_STOP;
}

} // namespace

TEST_CASE("dinput.catalogue.exposes_more_than_four_devices_and_stable_guids_across_roots",
    "[integration][dinput][input][catalogue]") {
    environment fixture;
    constexpr std::size_t device_count = 6U;
    for (std::size_t index = 0U; index < device_count; ++index) {
        const bool fingerprint_available = index < 3U;
        static_cast<void>(fixture.connect(index, std::nullopt, fingerprint_available));
    }
    REQUIRE(fixture.wait_for_catalogue(device_count));

    const auto first_root = fixture.create_root();
    const auto second_root = fixture.create_root();
    const auto first = enumerate(*first_root);
    const auto second = enumerate(*second_root);
    REQUIRE(first.size() == device_count);
    REQUIRE(second.size() == device_count);

    const auto first_ids = instance_guids(first);
    const auto second_ids = instance_guids(second);
    for (std::size_t index = 0U; index < first_ids.size(); ++index) {
        CHECK(IsEqualGUID(first_ids[index], second_ids[index]));
        CHECK((first_ids[index].Data3 & 0xf000U) == 0x8000U);
        CHECK((first_ids[index].Data4[0U] & 0xc0U) == 0x80U);
        CHECK(std::ranges::count_if(first_ids, [&first_ids, index](const GUID& candidate) {
            return IsEqualGUID(candidate, first_ids[index]);
        }) == 1);
    }
}

TEST_CASE("dinput.catalogue.exact_path_is_case_insensitive_and_suppresses_one_system_duplicate",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    constexpr std::string_view pnp_path{R"(\\?\HID#VID_054C&PID_0CE6#Controller-A)"};
    const auto native_guid = guid(1U);
    const auto lowercase_path = [&] {
        std::string value{pnp_path};
        std::ranges::transform(value, value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }();
    fixture.add_system_gamepad(native_guid,
        std::wstring{lowercase_path.begin(), lowercase_path.end()});
    const auto target = fixture.connect(0U, std::string{pnp_path});
    REQUIRE(target.valid());
    REQUIRE(fixture.wait_for_catalogue(1U));

    const auto root = fixture.create_root();
    const auto devices = enumerate(*root);
    REQUIRE(devices.size() == 1U);
    CHECK(IsEqualGUID(devices.front().guidInstance, native_guid));
    CHECK(IsEqualGUID(devices.front().guidProduct,
        make_system_instance(native_guid, L"Native gamepad").guidProduct));
    const com_owner<IDirectInputDevice8W> projected{create_device(*root, native_guid)};
    REQUIRE(projected.get() != nullptr);
    auto property = path_request();
    REQUIRE(inputnext_dinput_get_property_w(projected.get(), &DIPROP_GUIDANDPATH, &property.diph) ==
            DI_OK);
    CHECK(IsEqualGUID(property.guidClass, GUID_Joystick));
    const std::wstring expected_path{lowercase_path.begin(), lowercase_path.end()};
    CHECK(std::wstring_view{property.wszPath} == expected_path);

    const auto forced =
        enumerate(*root, DI8DEVCLASS_GAMECTRL, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    CHECK(forced.empty());
}

TEST_CASE("dinput.catalogue.matched_joystick_preserves_native_type_and_subtype",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#Joystick)"};
    const auto native_guid = guid(2U);
    auto native = make_system_instance(native_guid, L"Native joystick", DI8DEVTYPE_JOYSTICK);
    constexpr DWORD device_type =
        DIDEVTYPE_HID | MAKEWORD(DI8DEVTYPE_JOYSTICK, DI8DEVTYPEJOYSTICK_STANDARD);
    native.dwDevType = device_type;
    {
        std::scoped_lock lock{fixture.system->gate};
        fixture.system->devices.push_back(system_device_record{.instance = native,
            .path = std::wstring{path.begin(), path.end()}});
    }
    static_cast<void>(fixture.connect(0U, std::string{path}));
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto root = fixture.create_root();

    for (const auto filter : {DWORD{DI8DEVCLASS_ALL},
             DWORD{DI8DEVCLASS_GAMECTRL},
             DWORD{DI8DEVTYPE_JOYSTICK},
             DWORD{MAKEWORD(DI8DEVTYPE_JOYSTICK, DI8DEVTYPEJOYSTICK_STANDARD)}}) {
        const auto devices = enumerate(*root, filter);
        REQUIRE(std::ranges::count_if(devices, [&native_guid](const auto& device) {
            return device.guidInstance == native_guid;
        }) == 1);
        const auto& projected = *std::ranges::find_if(devices,
            [&native_guid](const auto& device) { return device.guidInstance == native_guid; });
        CHECK(projected.dwDevType == device_type);
    }
    CHECK(enumerate(*root, DI8DEVTYPE_GAMEPAD).empty());
    CHECK(enumerate(*root, MAKEWORD(DI8DEVTYPE_JOYSTICK, DI8DEVTYPEJOYSTICK_LIMITED)).empty());

    void* ansi_raw{};
    REQUIRE(root->QueryInterface(IID_IDirectInput8A, &ansi_raw) == DI_OK);
    const com_owner<IDirectInput8A> ansi{static_cast<IDirectInput8A*>(ansi_raw)};
    DIDEVICEINSTANCEA enumerated{};
    REQUIRE(ansi->EnumDevices(DI8DEVTYPE_JOYSTICK,
                &capture_ansi_instance,
                &enumerated,
                DIEDFL_ATTACHEDONLY) == DI_OK);
    CHECK(enumerated.guidInstance == native_guid);
    CHECK(enumerated.dwDevType == device_type);

    const com_owner<IDirectInputDevice8W> device{create_device(*root, native_guid)};
    REQUIRE(device.get() != nullptr);
    DIDEVICEINSTANCEW info{};
    info.dwSize = data_size<DIDEVICEINSTANCEW>();
    REQUIRE(device->GetDeviceInfo(&info) == DI_OK);
    CHECK(info.guidInstance == native_guid);
    CHECK(info.dwDevType == device_type);
    DIDEVCAPS capabilities{};
    capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(device->GetCapabilities(&capabilities) == DI_OK);
    CHECK(capabilities.dwDevType == device_type);
    CHECK(capabilities.dwAxes == 6U);
}

TEST_CASE("dinput.catalogue.matched_controller_types_share_the_normalized_projection",
    "[integration][dinput][input][catalogue][merge]") {
    for (const auto type : {DWORD{DI8DEVTYPE_JOYSTICK},
             DWORD{DI8DEVTYPE_GAMEPAD},
             DWORD{DI8DEVTYPE_DRIVING},
             DWORD{DI8DEVTYPE_FLIGHT},
             DWORD{DI8DEVTYPE_1STPERSON},
             DWORD{DI8DEVTYPE_REMOTE},
             DWORD{DI8DEVTYPE_SUPPLEMENTAL}}) {
        CAPTURE(type);
        environment fixture;
        constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#ClassifiedController)"};
        const auto native_guid = guid(3U);
        auto native = make_system_instance(native_guid, L"Native controller", type);
        native.guidFFDriver = guid(4U);
        native.wUsage = 0x04U;
        {
            std::scoped_lock lock{fixture.system->gate};
            fixture.system->devices.push_back(system_device_record{.instance = native,
                .path = std::wstring{path.begin(), path.end()},
                .force_feedback = true});
        }
        static_cast<void>(fixture.connect(0U, std::string{path}));
        REQUIRE(fixture.wait_for_catalogue(1U));
        const auto root = fixture.create_root();
        const auto devices = enumerate(*root);
        REQUIRE(devices.size() == 1U);
        const auto& projected = devices.front();
        REQUIRE(projected.guidInstance == native_guid);
        CHECK(projected.dwDevType == native.dwDevType);
        CHECK(projected.guidFFDriver == GUID_NULL);
        CHECK(projected.wUsage == 0x05U);
        for (const auto filter : {static_cast<DWORD>(type), native.dwDevType & 0xffffU}) {
            const auto filtered = enumerate(*root, filter);
            REQUIRE(filtered.size() == 1U);
            CHECK(filtered.front().guidInstance == projected.guidInstance);
            CHECK(filtered.front().dwDevType == projected.dwDevType);
            CHECK(filtered.front().guidFFDriver == projected.guidFFDriver);
            CHECK(filtered.front().wUsage == projected.wUsage);
            CHECK(enumerate(*root, filter, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK).empty());
        }
        const com_owner<IDirectInputDevice8W> device{create_device(*root, native_guid)};
        REQUIRE(device.get() != nullptr);
        DIDEVICEINSTANCEW info{};
        info.dwSize = data_size<DIDEVICEINSTANCEW>();
        REQUIRE(device->GetDeviceInfo(&info) == DI_OK);
        CHECK(info.guidInstance == projected.guidInstance);
        CHECK(info.dwDevType == projected.dwDevType);
        CHECK(info.guidFFDriver == projected.guidFFDriver);
        CHECK(info.wUsage == projected.wUsage);
        DIDEVCAPS capabilities{};
        capabilities.dwSize = data_size<DIDEVCAPS>();
        REQUIRE(device->GetCapabilities(&capabilities) == DI_OK);
        CHECK(capabilities.dwDevType == projected.dwDevType);
        CHECK((capabilities.dwFlags & DIDC_FORCEFEEDBACK) == 0U);
    }
}

TEST_CASE("dinput.catalogue.enumeration_and_device_info_share_the_ansi_name",
    "[integration][dinput][input][catalogue]") {
    environment fixture;
    constexpr std::wstring_view glyph{L"\U0001F3AE"};
    constexpr auto glyph_count = (MAX_PATH - 1U) / glyph.size();
    std::wstring name;
    for (std::size_t index = 0U; index < glyph_count; ++index) {
        name.append(glyph);
    }
    constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#LongName)"};
    const auto native_guid = guid(0x10U);
    fixture.add_system_gamepad(native_guid,
        std::wstring{path.begin(), path.end()},
        false,
        name.c_str());
    static_cast<void>(fixture.connect(0U, std::string{path}));
    REQUIRE(fixture.wait_for_catalogue(1U));

    const auto root = fixture.create_root();
    void* ansi_root{};
    REQUIRE(root->QueryInterface(IID_IDirectInput8A, &ansi_root) == DI_OK);
    const com_owner<IDirectInput8A> ansi{static_cast<IDirectInput8A*>(ansi_root)};
    DIDEVICEINSTANCEA enumerated{};
    REQUIRE(ansi->EnumDevices(DI8DEVCLASS_GAMECTRL,
                &capture_ansi_instance,
                &enumerated,
                DIEDFL_ATTACHEDONLY) == DI_OK);
    CHECK(enumerated.guidInstance == native_guid);

    const com_owner<IDirectInputDevice8W> projected{create_device(*root, native_guid)};
    REQUIRE(projected.get() != nullptr);
    void* ansi_device{};
    REQUIRE(projected->QueryInterface(IID_IDirectInputDevice8A, &ansi_device) == DI_OK);
    const com_owner<IDirectInputDevice8A> device{static_cast<IDirectInputDevice8A*>(ansi_device)};
    DIDEVICEINSTANCEA info{};
    info.dwSize = data_size<DIDEVICEINSTANCEA>();
    REQUIRE(device->GetDeviceInfo(&info) == DI_OK);
    CHECK(info.guidInstance == native_guid);

    std::array<char, 8U> encoded{};
    const auto width = WideCharToMultiByte(CP_ACP,
        0U,
        glyph.data(),
        static_cast<int>(glyph.size()),
        encoded.data(),
        static_cast<int>(encoded.size()),
        nullptr,
        nullptr);
    REQUIRE(width > 0);
    const auto glyph_width = static_cast<std::size_t>(width);
    const auto fits = std::min(glyph_count, (MAX_PATH - 1U) / glyph_width);
    std::string expected;
    for (std::size_t index = 0U; index < fits; ++index) {
        expected.append(encoded.data(), glyph_width);
    }
    CHECK(std::string_view{enumerated.tszInstanceName} == expected);
    CHECK(std::string_view{info.tszInstanceName} == expected);
    CHECK(std::string_view{info.tszProductName} == expected);
}

TEST_CASE("dinput.catalogue.same_vid_pid_with_different_paths_keeps_both_devices",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    const auto native_guid = guid(2U);
    fixture.add_system_gamepad(native_guid, LR"(\\?\HID#VID_054C&PID_0CE6#Native)");
    static_cast<void>(fixture.connect(0U, std::string{R"(\\?\HID#VID_054C&PID_0CE6#GameInput)"}));
    REQUIRE(fixture.wait_for_catalogue(1U));

    const auto root = fixture.create_root();
    const auto devices = enumerate(*root);
    REQUIRE(devices.size() == 2U);
    CHECK(contains_guid(devices, native_guid));
    const auto projected = std::ranges::find_if(devices, [&native_guid](const auto& device) {
        return !IsEqualGUID(device.guidInstance, native_guid);
    });
    REQUIRE(projected != devices.end());
    IDirectInputDevice8W* raw_native = create_device(*root, native_guid);
    IDirectInputDevice8W* normalized = create_device(*root, projected->guidInstance);
    REQUIRE(raw_native != nullptr);
    REQUIRE(normalized != nullptr);
    const com_owner<IDirectInputDevice8W> raw_native_owner{raw_native};
    const com_owner<IDirectInputDevice8W> normalized_owner{normalized};
    CHECK(read_vendor_product(*raw_native) == 0x0CE6054CU);
    CHECK(read_vendor_product(*normalized) == 0x0CE6054CU);
    auto property = path_request();
    property.guidClass = GUID_Joystick;
    CHECK(inputnext_dinput_get_property_w(normalized, &DIPROP_GUIDANDPATH, &property.diph) ==
          DIERR_UNSUPPORTED);
    CHECK(IsEqualGUID(property.guidClass, GUID_Joystick));
    CHECK(property.wszPath[0U] == L'\0');
}

TEST_CASE("dinput.catalogue.duplicate_input_path_does_not_suppress_native_device",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    const auto native_guid = guid(3U);
    constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#Shared)"};
    fixture.add_system_gamepad(native_guid, std::wstring{path.begin(), path.end()});
    static_cast<void>(fixture.connect(0U, std::string{path}));
    static_cast<void>(fixture.connect(1U, std::string{path}));
    REQUIRE(fixture.wait_for_catalogue(2U));

    const auto root = fixture.create_root();
    const auto devices = enumerate(*root);
    REQUIRE(devices.size() == 3U);
    CHECK(contains_guid(devices, native_guid));
}

TEST_CASE("dinput.catalogue.duplicate_system_path_does_not_suppress_native_devices",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    const auto first_native = guid(4U);
    const auto second_native = guid(5U);
    constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#Duplicate)"};
    fixture.add_system_gamepad(first_native, std::wstring{path.begin(), path.end()});
    fixture.add_system_gamepad(second_native, std::wstring{path.begin(), path.end()});
    static_cast<void>(fixture.connect(0U, std::string{path}));
    REQUIRE(fixture.wait_for_catalogue(1U));

    const auto root = fixture.create_root();
    const auto devices = enumerate(*root);
    REQUIRE(devices.size() == 3U);
    CHECK(contains_guid(devices, first_native));
    CHECK(contains_guid(devices, second_native));
}

TEST_CASE("dinput.catalogue.a_former_match_does_not_claim_the_native_guid_when_ambiguous",
    "[integration][dinput][input][catalogue][merge]") {
    environment fixture;
    const auto first_native = guid(6U);
    const auto second_native = guid(7U);
    constexpr std::string_view path{R"(\\?\HID#VID_054C&PID_0CE6#Changed)"};
    const std::wstring native_path{path.begin(), path.end()};
    fixture.add_system_gamepad(first_native, native_path);
    static_cast<void>(fixture.connect(0U, std::string{path}));
    REQUIRE(fixture.wait_for_catalogue(1U));

    const auto root = fixture.create_root();
    const auto matched = enumerate(*root);
    REQUIRE(matched.size() == 1U);
    CHECK(IsEqualGUID(matched.front().guidInstance, first_native));
    const com_owner<IDirectInputDevice8W> existing{create_device(*root, first_native)};
    REQUIRE(existing.get() != nullptr);
    const hidden_window window;
    configure_compact_device(*existing.get(), window.get());
    REQUIRE(existing->Acquire() == DI_OK);

    fixture.add_system_gamepad(second_native, native_path);
    const auto ambiguous = enumerate(*root);
    REQUIRE(ambiguous.size() == 3U);
    CHECK(std::ranges::count_if(ambiguous, [&first_native](const auto& device) {
        return IsEqualGUID(device.guidInstance, first_native);
    }) == 1);
    CHECK(contains_guid(ambiguous, second_native));

    const com_owner<IDirectInputDevice8W> native{create_device(*root, first_native)};
    REQUIRE(native.get() != nullptr);
    DIDEVCAPS capabilities{};
    capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(native->GetCapabilities(&capabilities) == DI_OK);
    CHECK((capabilities.dwFlags & DIDC_EMULATED) == 0U);
    compact_state state{};
    CHECK(existing->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.catalogue.saved_virtual_guid_resolves_from_a_fresh_root_and_old_child_stays_lost",
    "[integration][dinput][input][catalogue][reconnect]") {
    environment fixture;
    const auto old_target = fixture.connect(0U, std::nullopt, false);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto first_root = fixture.create_root();
    const auto first_devices = enumerate(*first_root);
    REQUIRE(first_devices.size() == 1U);
    const auto saved_guid = first_devices.front().guidInstance;
    CHECK(first_root->GetDeviceStatus(saved_guid) == DI_OK);

    IDirectInputDevice8W* raw_old_device = create_device(*first_root, saved_guid);
    REQUIRE(raw_old_device != nullptr);
    const com_owner<IDirectInputDevice8W> old_device{raw_old_device};
    std::array<DIOBJECTDATAFORMAT, 22U> descriptors{};
    std::array<GUID, 22U> object_guids{};
    const auto format = compact_format(descriptors, object_guids);
    REQUIRE(old_device->SetDataFormat(&format) == DI_OK);
    const hidden_window window;
    REQUIRE(old_device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) ==
            DI_OK);
    REQUIRE(old_device->Acquire() == DI_OK);
    compact_state current{};
    REQUIRE(old_device->GetDeviceState(data_size<compact_state>(), &current) == DI_OK);

    REQUIRE(fixture.disconnect(0U, old_target));
    CHECK(old_device->GetDeviceState(data_size<compact_state>(), &current) == DIERR_INPUTLOST);

    const auto new_target = fixture.connect(0U, std::nullopt, false);
    REQUIRE(new_target.valid());
    CHECK(new_target != old_target);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto second_root = fixture.create_root();
    const auto second_devices = enumerate(*second_root);
    REQUIRE(second_devices.size() == 1U);
    const auto current_guid = second_devices.front().guidInstance;
    CHECK_FALSE(IsEqualGUID(current_guid, saved_guid));
    CHECK(second_root->GetDeviceStatus(current_guid) == DI_OK);
    CHECK(second_root->GetDeviceStatus(saved_guid) != DI_OK);

    IDirectInputDevice8W* stale_by_guid{};
    CHECK(FAILED(second_root->CreateDevice(saved_guid, &stale_by_guid, nullptr)));
    CHECK(stale_by_guid == nullptr);

    IDirectInputDevice8W* raw_new_device = create_device(*second_root, current_guid);
    REQUIRE(raw_new_device != nullptr);
    const com_owner<IDirectInputDevice8W> new_device{raw_new_device};
    REQUIRE(new_device->SetDataFormat(&format) == DI_OK);
    REQUIRE(new_device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) ==
            DI_OK);
    REQUIRE(new_device->Acquire() == DI_OK);
    CHECK(old_device->GetDeviceState(data_size<compact_state>(), &current) == DIERR_INPUTLOST);
    CHECK(new_device->GetDeviceState(data_size<compact_state>(), &current) == DI_OK);
}

TEST_CASE("dinput.catalogue.system_keyboard_remains_available_after_input_shutdown",
    "[integration][dinput][input][catalogue][shutdown]") {
    environment fixture;
    const auto root = fixture.create_root();
    const auto before = fixture.context.session_requests.load(std::memory_order_relaxed);
    fixture.shutdown();

    const auto keyboards = enumerate(*root, DI8DEVCLASS_KEYBOARD, DIEDFL_ATTACHEDONLY);
    REQUIRE(keyboards.size() == 1U);
    CHECK(IsEqualGUID(keyboards.front().guidInstance, GUID_SysKeyboard));
    CHECK(fixture.context.session_requests.load(std::memory_order_relaxed) == before);

    IDirectInputDevice8W* keyboard{};
    REQUIRE(root->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr) == DI_OK);
    CHECK(keyboard != nullptr);
    if (keyboard != nullptr) {
        keyboard->Release();
    }
    CHECK(fixture.context.session_requests.load(std::memory_order_relaxed) == before);
}
