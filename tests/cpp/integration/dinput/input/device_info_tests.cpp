#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/profile.hpp"

#include <Windows.h>
#include <dinput.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_test_support;

namespace {

[[nodiscard]] constexpr DWORD axis_x_id() noexcept {
    return DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U);
}

struct reentrant_context final {
    IDirectInputDevice8W* device{};
    DWORD visited{};
    HRESULT nested_query{E_UNEXPECTED};
};

BOOL CALLBACK reentrant_object_callback(const DIDEVICEOBJECTINSTANCEW* const object,
    void* const context) {
    auto* const state = static_cast<reentrant_context*>(context);
    if (state == nullptr || object == nullptr) {
        return DIENUM_STOP;
    }
    ++state->visited;
    DIDEVICEOBJECTINSTANCEW nested{};
    nested.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();
    state->nested_query = state->device->GetObjectInfo(&nested, axis_x_id(), DIPH_BYID);
    static_cast<void>(state->device->Unacquire());
    return DIENUM_STOP;
}

struct button_enumeration final {
    IDirectInputDevice8W* device{};
    DWORD count{};
    bool push_buttons{true};
    bool identities_match{true};
};

struct object_count final {
    DWORD count{};
};

template <typename Value>
struct trailing_canary final {
    Value value{};
    DWORD canary{0xC0FFEE11U};
};

BOOL CALLBACK count_object_callback(const DIDEVICEOBJECTINSTANCEW* const object,
    void* const context) {
    if (object != nullptr && context != nullptr) {
        ++static_cast<object_count*>(context)->count;
    }
    return DIENUM_CONTINUE;
}

BOOL CALLBACK inspect_button_callback(const DIDEVICEOBJECTINSTANCEW* const object,
    void* const context) {
    auto& result = *static_cast<button_enumeration*>(context);
    ++result.count;
    result.push_buttons = result.push_buttons && DIDFT_GETTYPE(object->dwType) == DIDFT_PSHBUTTON;
    DIDEVICEOBJECTINSTANCEW queried{};
    queried.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();
    const auto status = result.device->GetObjectInfo(&queried, object->dwType, DIPH_BYID);
    result.identities_match = result.identities_match && status == DI_OK &&
                              queried.dwType == object->dwType &&
                              queried.guidType == object->guidType;
    return DIENUM_CONTINUE;
}

template <typename Instance>
BOOL CALLBACK collect_object(const Instance* object, void* context) {
    static_cast<std::vector<Instance>*>(context)->push_back(*object);
    return DIENUM_CONTINUE;
}

} // namespace

TEST_CASE("dinput.device_views_share_com_identity_and_retain_the_parent",
    "[integration][dinput][input][com]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    std::atomic<std::uint32_t> parent_destructions{};
    auto* const raw_parent = new parent_unknown{&parent_destructions};
    com_owner<IUnknown> parent_owner{static_cast<IUnknown*>(raw_parent)};
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, *raw_parent);
    REQUIRE(raw_parent->references() == 2U);

    IDirectInputDevice8A* ansi_raw{};
    REQUIRE(device->QueryInterface(IID_IDirectInputDevice8A, reinterpret_cast<void**>(&ansi_raw)) ==
            S_OK);
    com_owner<IDirectInputDevice8A> ansi{ansi_raw};
    IUnknown* wide_identity{};
    IUnknown* ansi_identity{};
    REQUIRE(device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&wide_identity)) == S_OK);
    REQUIRE(ansi->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&ansi_identity)) == S_OK);
    CHECK(wide_identity == ansi_identity);
    wide_identity->Release();
    ansi_identity->Release();

    IDirectInputDevice7W* old_interface{};
    CHECK(device->QueryInterface(IID_IDirectInputDevice7W,
              reinterpret_cast<void**>(&old_interface)) == E_NOINTERFACE);
    CHECK(old_interface == nullptr);

    DIDEVICEINSTANCEW wide_info{};
    wide_info.dwSize = data_size<DIDEVICEINSTANCEW>();
    REQUIRE(device->GetDeviceInfo(&wide_info) == DI_OK);
    CHECK(std::wstring{wide_info.tszInstanceName} == L"InputNext integration gamepad");

    DIDEVICEINSTANCEA ansi_info{};
    ansi_info.dwSize = data_size<DIDEVICEINSTANCEA>();
    REQUIRE(ansi->GetDeviceInfo(&ansi_info) == DI_OK);
    CHECK(std::string{ansi_info.tszInstanceName} == "InputNext integration gamepad");
    CHECK(ansi_info.guidInstance == wide_info.guidInstance);

    std::array<DIOBJECTDATAFORMAT, 22U> descriptors{};
    std::array<GUID, 22U> guids{};
    const auto format = compact_format(descriptors, guids);
    REQUIRE(device->SetDataFormat(&format) == DI_OK);
    const hidden_window window;
    REQUIRE(
        device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    reentrant_context callback_state{.device = device.get()};
    CHECK(device->EnumObjects(reentrant_object_callback, &callback_state, DIDFT_ALL) == DI_OK);
    CHECK(callback_state.visited == 1U);
    CHECK(callback_state.nested_query == DI_OK);

    ansi.reset();
    CHECK(raw_parent->references() == 2U);
    parent_owner.reset();
    CHECK(parent_destructions.load(std::memory_order_relaxed) == 0U);
    device.reset();
    CHECK(parent_destructions.load(std::memory_order_relaxed) == 1U);
}

TEST_CASE("dinput.button_filters_and_object_ids_describe_push_buttons",
    "[integration][dinput][input][objects]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent);

    button_enumeration result{.device = device.get()};
    CHECK(device->EnumObjects(inspect_button_callback, &result, DIDFT_TGLBUTTON) == DI_OK);
    CHECK(result.count == 0U);
    REQUIRE(device->EnumObjects(inspect_button_callback, &result, DIDFT_PSHBUTTON) == DI_OK);
    CHECK(result.count == 15U);
    CHECK(result.push_buttons);
    CHECK(result.identities_match);

    result.count = 0U;
    REQUIRE(device->EnumObjects(inspect_button_callback, &result, DIDFT_BUTTON) == DI_OK);
    CHECK(result.count == 15U);
    CHECK(result.push_buttons);
    CHECK(result.identities_match);
}

TEST_CASE("dinput.object_filters_select_axes_and_ignore_output_collection_metadata",
    "[integration][dinput][input][objects]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent);

    const auto count_for = [&device](const DWORD filter) {
        object_count result{};
        CHECK(device->EnumObjects(&count_object_callback, &result, filter) == DI_OK);
        return result.count;
    };

    CHECK(count_for(DIDFT_AXIS | DIDFT_OUTPUT) == 6U);
    CHECK(count_for(DIDFT_OUTPUT) == 0U);
    CHECK(count_for(DIDFT_NODATA) == 0U);
    CHECK(count_for(DIDFT_COLLECTION | DIDFT_MAKEINSTANCE(1U)) == 0U);
    CHECK(count_for(DIDFT_NOCOLLECTION) == 22U);
    CHECK(count_for(DIDFT_FFACTUATOR | DIDFT_AXIS) == 0U);
}

TEST_CASE("dinput.object_info_keeps_native_offsets_after_custom_format_changes",
    "[integration][dinput][input][objects]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const auto device = create_device(target, access, parent);
    void* ansi_raw{};
    REQUIRE(device->QueryInterface(IID_IDirectInputDevice8A, &ansi_raw) == DI_OK);
    const com_owner<IDirectInputDevice8A> ansi{static_cast<IDirectInputDevice8A*>(ansi_raw)};

    const auto profile = dinput::objects();
    std::array<DIOBJECTDATAFORMAT, 2U> descriptors{DIOBJECTDATAFORMAT{.pguid = &profile[0U].guid,
                                                       .dwOfs = 32U,
                                                       .dwType = profile[0U].id,
                                                       .dwFlags = DIDOI_ASPECTPOSITION},
        DIOBJECTDATAFORMAT{.pguid = &profile[6U].guid,
            .dwOfs = 1U,
            .dwType = profile[6U].id,
            .dwFlags = 0U}};
    const DIDATAFORMAT format{.dwSize = data_size<DIDATAFORMAT>(),
        .dwObjSize = data_size<DIOBJECTDATAFORMAT>(),
        .dwFlags = DIDF_ABSAXIS,
        .dwDataSize = 36U,
        .dwNumObjs = static_cast<DWORD>(descriptors.size()),
        .rgodf = descriptors.data()};

    for (const auto layout : {std::array<DWORD, 2U>{32U, 1U}, std::array<DWORD, 2U>{8U, 21U}}) {
        descriptors[0U].dwOfs = layout[0U];
        descriptors[1U].dwOfs = layout[1U];
        REQUIRE(device->SetDataFormat(&format) == DI_OK);
        for (const auto index : {0U, 6U}) {
            const auto& expected = profile[index];
            const auto offset = layout[index == 0U ? 0U : 1U];
            for (const auto [object, how] : {std::pair{expected.id, DWORD{DIPH_BYID}},
                     std::pair{DIMAKEUSAGEDWORD(expected.usage_page, expected.usage),
                         DWORD{DIPH_BYUSAGE}},
                     std::pair{offset, DWORD{DIPH_BYOFFSET}}}) {
                DIDEVICEOBJECTINSTANCEW wide{};
                wide.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();
                REQUIRE(device->GetObjectInfo(&wide, object, how) == DI_OK);
                CHECK(wide.dwOfs == expected.native_offset);
                CHECK(wide.dwType == expected.id);
                DIDEVICEOBJECTINSTANCEA narrow{};
                narrow.dwSize = data_size<DIDEVICEOBJECTINSTANCEA>();
                REQUIRE(ansi->GetObjectInfo(&narrow, object, how) == DI_OK);
                CHECK(narrow.dwOfs == wide.dwOfs);
                CHECK(narrow.dwType == wide.dwType);
            }
        }

        DIDEVICEOBJECTINSTANCEW omitted{};
        omitted.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();
        REQUIRE(device->GetObjectInfo(&omitted, profile[1U].id, DIPH_BYID) == DI_OK);
        CHECK(omitted.dwOfs == profile[1U].native_offset);
        CHECK(device->GetObjectInfo(&omitted, profile[0U].native_offset, DIPH_BYOFFSET) ==
              DIERR_OBJECTNOTFOUND);

        std::vector<DIDEVICEOBJECTINSTANCEW> wide_objects;
        std::vector<DIDEVICEOBJECTINSTANCEA> ansi_objects;
        REQUIRE(device->EnumObjects(&collect_object<DIDEVICEOBJECTINSTANCEW>,
                    &wide_objects,
                    DIDFT_ALL) == DI_OK);
        REQUIRE(
            ansi->EnumObjects(&collect_object<DIDEVICEOBJECTINSTANCEA>, &ansi_objects, DIDFT_ALL) ==
            DI_OK);
        REQUIRE(wide_objects.size() == profile.size());
        REQUIRE(ansi_objects.size() == wide_objects.size());
        for (std::size_t index = 0U; index < wide_objects.size(); ++index) {
            CHECK(wide_objects[index].dwOfs == profile[index].native_offset);
            CHECK(wide_objects[index].dwType == profile[index].id);
            CHECK(ansi_objects[index].dwOfs == wide_objects[index].dwOfs);
            CHECK(ansi_objects[index].dwType == wide_objects[index].dwType);
        }
    }
}

TEST_CASE("dinput.dx3_output_structures_preserve_size_and_adjacent_memory",
    "[integration][dinput][input][abi]") {
    static_assert(offsetof(trailing_canary<DIDEVCAPS_DX3>, canary) == sizeof(DIDEVCAPS_DX3));
    static_assert(
        offsetof(trailing_canary<DIDEVICEINSTANCE_DX3A>, canary) == sizeof(DIDEVICEINSTANCE_DX3A));
    static_assert(
        offsetof(trailing_canary<DIDEVICEINSTANCE_DX3W>, canary) == sizeof(DIDEVICEINSTANCE_DX3W));
    static_assert(offsetof(trailing_canary<DIDEVICEOBJECTINSTANCE_DX3A>, canary) ==
                  sizeof(DIDEVICEOBJECTINSTANCE_DX3A));
    static_assert(offsetof(trailing_canary<DIDEVICEOBJECTINSTANCE_DX3W>, canary) ==
                  sizeof(DIDEVICEOBJECTINSTANCE_DX3W));

    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent);

    IDirectInputDevice8A* ansi_raw{};
    REQUIRE(device->QueryInterface(IID_IDirectInputDevice8A, reinterpret_cast<void**>(&ansi_raw)) ==
            S_OK);
    com_owner<IDirectInputDevice8A> ansi{ansi_raw};

    DIDEVCAPS full_capabilities{};
    full_capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(device->GetCapabilities(&full_capabilities) == DI_OK);
    CHECK((full_capabilities.dwFlags & DIDC_ATTACHED) != 0U);
    CHECK(full_capabilities.dwAxes == 6U);
    CHECK(full_capabilities.dwButtons == 15U);
    CHECK(full_capabilities.dwPOVs == 1U);

    trailing_canary<DIDEVCAPS_DX3> capabilities{};
    capabilities.value.dwSize = data_size<DIDEVCAPS_DX3>();
    REQUIRE(device->GetCapabilities(reinterpret_cast<DIDEVCAPS*>(&capabilities.value)) == DI_OK);
    CHECK(capabilities.value.dwSize == data_size<DIDEVCAPS_DX3>());
    CHECK((capabilities.value.dwFlags & DIDC_ATTACHED) != 0U);
    CHECK(capabilities.value.dwAxes == 6U);
    CHECK(capabilities.value.dwButtons == 15U);
    CHECK(capabilities.value.dwPOVs == 1U);
    CHECK(capabilities.canary == 0xC0FFEE11U);

    DIDEVICEINSTANCEW full_wide_info{};
    full_wide_info.dwSize = data_size<DIDEVICEINSTANCEW>();
    REQUIRE(device->GetDeviceInfo(&full_wide_info) == DI_OK);
    trailing_canary<DIDEVICEINSTANCE_DX3W> wide_info{};
    wide_info.value.dwSize = data_size<DIDEVICEINSTANCE_DX3W>();
    REQUIRE(device->GetDeviceInfo(reinterpret_cast<DIDEVICEINSTANCEW*>(&wide_info.value)) == DI_OK);
    CHECK(wide_info.value.dwSize == data_size<DIDEVICEINSTANCE_DX3W>());
    CHECK(wide_info.value.guidInstance == full_wide_info.guidInstance);
    CHECK(wide_info.value.guidProduct == full_wide_info.guidProduct);
    CHECK(wide_info.value.dwDevType == full_wide_info.dwDevType);
    CHECK(std::wstring{wide_info.value.tszInstanceName} == L"InputNext integration gamepad");
    CHECK(std::wstring{wide_info.value.tszProductName} == L"InputNext test controller");
    CHECK(wide_info.canary == 0xC0FFEE11U);

    DIDEVICEINSTANCEA full_ansi_info{};
    full_ansi_info.dwSize = data_size<DIDEVICEINSTANCEA>();
    REQUIRE(ansi->GetDeviceInfo(&full_ansi_info) == DI_OK);
    trailing_canary<DIDEVICEINSTANCE_DX3A> ansi_info{};
    ansi_info.value.dwSize = data_size<DIDEVICEINSTANCE_DX3A>();
    REQUIRE(ansi->GetDeviceInfo(reinterpret_cast<DIDEVICEINSTANCEA*>(&ansi_info.value)) == DI_OK);
    CHECK(ansi_info.value.dwSize == data_size<DIDEVICEINSTANCE_DX3A>());
    CHECK(ansi_info.value.guidInstance == full_ansi_info.guidInstance);
    CHECK(ansi_info.value.guidProduct == full_ansi_info.guidProduct);
    CHECK(ansi_info.value.dwDevType == full_ansi_info.dwDevType);
    CHECK(std::string{ansi_info.value.tszInstanceName} == "InputNext integration gamepad");
    CHECK(std::string{ansi_info.value.tszProductName} == "InputNext test controller");
    CHECK(ansi_info.canary == 0xC0FFEE11U);

    const auto& x_axis = dinput::objects().front();
    trailing_canary<DIDEVICEOBJECTINSTANCE_DX3W> wide_object{};
    wide_object.value.dwSize = data_size<DIDEVICEOBJECTINSTANCE_DX3W>();
    REQUIRE(device->GetObjectInfo(reinterpret_cast<DIDEVICEOBJECTINSTANCEW*>(&wide_object.value),
                axis_x_id(),
                DIPH_BYID) == DI_OK);
    CHECK(wide_object.value.dwSize == data_size<DIDEVICEOBJECTINSTANCE_DX3W>());
    CHECK(wide_object.value.guidType == x_axis.guid);
    CHECK(wide_object.value.dwOfs == static_cast<DWORD>(offsetof(DIJOYSTATE, lX)));
    CHECK(wide_object.value.dwType == axis_x_id());
    CHECK((wide_object.value.dwFlags & DIDOI_ASPECTPOSITION) != 0U);
    CHECK(std::wstring{wide_object.value.tszName} == L"X Axis");
    CHECK(wide_object.canary == 0xC0FFEE11U);

    trailing_canary<DIDEVICEOBJECTINSTANCE_DX3A> ansi_object{};
    ansi_object.value.dwSize = data_size<DIDEVICEOBJECTINSTANCE_DX3A>();
    REQUIRE(ansi->GetObjectInfo(reinterpret_cast<DIDEVICEOBJECTINSTANCEA*>(&ansi_object.value),
                axis_x_id(),
                DIPH_BYID) == DI_OK);
    CHECK(ansi_object.value.dwSize == data_size<DIDEVICEOBJECTINSTANCE_DX3A>());
    CHECK(ansi_object.value.guidType == x_axis.guid);
    CHECK(ansi_object.value.dwOfs == static_cast<DWORD>(offsetof(DIJOYSTATE, lX)));
    CHECK(ansi_object.value.dwType == axis_x_id());
    CHECK((ansi_object.value.dwFlags & DIDOI_ASPECTPOSITION) != 0U);
    CHECK(std::string{ansi_object.value.tszName} == "X Axis");
    CHECK(ansi_object.canary == 0xC0FFEE11U);
}
