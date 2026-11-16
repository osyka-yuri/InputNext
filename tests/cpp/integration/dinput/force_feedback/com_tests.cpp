#include "cpp/integration/dinput/input/exclusive_native_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/com/device.hpp"
#include "dinput/device_info.hpp"
#include "dinput/force_feedback/service.hpp"
#include "dinput/profile.hpp"
#include "dinput/property_abi.h"
#include "dinput/system_input.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/device_identity.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput::force_feedback;
using namespace inputnext::dinput_native_test_support;
using namespace inputnext::dinput_test_support;

namespace {

using output::detail::composition;

struct test_motor_capability final {
    connection_target target{};
    motors supported{};
};

[[nodiscard]] motors query_test_motors(void* context, connection_target target) noexcept {
    const auto& capability = *static_cast<const test_motor_capability*>(context);
    return target == capability.target ? capability.supported : motors{};
}

class force_feedback_fixture final {
public:
    explicit force_feedback_fixture(motors supported = {.low = true, .high = true})
        : native{nullptr, nullptr},
          motor_capability{.target = native.target, .supported = supported},
          output_composition{nullptr},
          output_executor{nullptr},
          access{nullptr},
          fallback{nullptr} {
        const auto request = native.request();
        identity = identity_for(native.target, 0x71U);
        identity.instance.guidInstance = request.native_instance;
        identity.path = request.native_path;

        auto& owners = *native.catalogue.owners;
        native.catalogue.context.access->close_admission();
        native.catalogue.context.access->drain();

        fallback = std::make_shared<dinput::input_access>(*owners.core,
            *owners.gameinput.runtime,
            native.focus.observer());
        output_composition =
            std::make_unique<composition>(gameinput_test_support::test_runtime_id(),
                owners.gameinput.registry,
                native.focus.observer());
        auto created_executor = output::executor::create(output_composition->public_authority());
        if (!created_executor.has_value()) {
            throw std::runtime_error{"force feedback fixture could not create output executor"};
        }
        output_executor = std::move(*created_executor);
        const auto execution = output::detail::authority_test_access::attached_execution(
            output_composition->public_authority());
        if (!execution.has_value()) {
            throw std::runtime_error{"force feedback fixture has no attached executor"};
        }
        motor_capability.supported.backend = {.target = native.target,
            .incarnation = {.owner = *execution, .id = {1U}}};
        auto feedback = std::make_unique<service>(*output_composition,
            *output_executor,
            &motor_capability,
            &query_test_motors);
        access = std::make_shared<dinput::input_access>(*owners.core,
            *owners.gameinput.runtime,
            native.focus.observer(),
            std::move(feedback));
        native.catalogue.context.access = access;
    }

    ~force_feedback_fixture() {
        if (access != nullptr) {
            access->close_admission();
            access->drain();
            native.catalogue.context.access = fallback;
            access.reset();
        }
        if (output_executor != nullptr) {
            output_executor->shutdown();
            output_executor.reset();
        }
        output_composition.reset();
    }

    force_feedback_fixture(const force_feedback_fixture&) = delete;
    force_feedback_fixture& operator=(const force_feedback_fixture&) = delete;
    force_feedback_fixture(force_feedback_fixture&&) = delete;
    force_feedback_fixture& operator=(force_feedback_fixture&&) = delete;

    [[nodiscard]] com_owner<IDirectInputDevice8W> make_device(IUnknown& parent) const {
        void* created{}; // NOLINT(misc-const-correctness): DirectInput writes this out parameter.
        const auto status =
            dinput::make_device(identity, access, parent, true, &created, native.factory);
        if (FAILED(status)) {
            throw std::runtime_error{"force feedback fixture could not create DirectInput device"};
        }
        return com_owner<IDirectInputDevice8W>{static_cast<IDirectInputDevice8W*>(created)};
    }

    native_service_fixture native;
    test_motor_capability motor_capability{};
    dinput::device_identity identity{};
    std::unique_ptr<composition> output_composition;
    std::unique_ptr<output::executor> output_executor;
    std::shared_ptr<dinput::input_access> access;
    std::shared_ptr<dinput::input_access> fallback;
};

struct object_capture final {
    std::array<DIDEVICEOBJECTINSTANCEW, 2U> values{};
    DWORD count{};
};

BOOL CALLBACK capture_object(const DIDEVICEOBJECTINSTANCEW* info, void* context) noexcept {
    auto& capture = *static_cast<object_capture*>(context);
    if (info == nullptr) {
        return DIENUM_STOP;
    }
    if (capture.count < static_cast<DWORD>(capture.values.size())) {
        capture.values[capture.count] = *info;
    }
    ++capture.count;
    return DIENUM_CONTINUE;
}

struct effect_capture final {
    std::array<DIEFFECTINFOW, 7U> values{};
    DWORD count{};
};

[[nodiscard]] DIPROPDWORD device_dword_property(DWORD data = 0U) noexcept {
    DIPROPDWORD value{};
    value.diph.dwSize = sizeof(value);
    value.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    value.diph.dwHow = DIPH_DEVICE;
    value.dwData = data;
    return value;
}

struct release_effect_context final {
    IDirectInputEffect* owned{};
    com_owner<IDirectInputDevice8W>* device{};
    parent_unknown* parent{};
    bool called{};
    bool guid_read{};
    bool device_released{};
    bool parent_alive{};
};

BOOL CALLBACK release_created_effect(IDirectInputEffect* enumerated, void* context) noexcept {
    auto& capture = *static_cast<release_effect_context*>(context);
    capture.called = enumerated != nullptr && capture.owned == enumerated;
    if (capture.called) {
        static_cast<void>(capture.owned->Release());
        capture.owned = nullptr;
        if (capture.device != nullptr) {
            capture.device->reset();
            capture.device = nullptr;
            capture.device_released = true;
            capture.parent_alive = capture.parent != nullptr && capture.parent->references() == 2U;
        }
        GUID id{};
        capture.guid_read = enumerated->GetEffectGuid(&id) == DI_OK && id == GUID_ConstantForce;
    }
    return DIENUM_STOP;
}

BOOL CALLBACK capture_effect(const DIEFFECTINFOW* info, void* context) noexcept {
    auto& capture = *static_cast<effect_capture*>(context);
    if (info == nullptr || capture.count >= static_cast<DWORD>(capture.values.size())) {
        return DIENUM_STOP;
    }
    capture.values[capture.count++] = *info;
    return DIENUM_CONTINUE;
}

[[nodiscard]] DIEFFECT
constant_effect(DWORD& axis, DICONSTANTFORCE& force, LONG& direction) noexcept {
    DIEFFECT value{};
    value.dwSize = sizeof(value);
    value.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    value.dwDuration = 1'000'000U;
    value.dwGain = DI_FFNOMINALMAX;
    value.dwTriggerButton = DIEB_NOTRIGGER;
    value.dwTriggerRepeatInterval = INFINITE;
    value.cAxes = 1U;
    value.rgdwAxes = &axis;
    value.rglDirection = &direction;
    value.cbTypeSpecificParams = sizeof(force);
    value.lpvTypeSpecificParams = &force;
    return value;
}

struct guarded_legacy_effect final {
    guarded_legacy_effect() {
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        region = static_cast<BYTE*>(VirtualAlloc(nullptr,
            static_cast<SIZE_T>(system_info.dwPageSize) * 2U,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE));
        if (region == nullptr) {
            throw std::runtime_error{"could not allocate the guarded DirectInput parameter"};
        }
        DWORD previous{};
        if (VirtualProtect(region + system_info.dwPageSize,
                system_info.dwPageSize,
                PAGE_NOACCESS,
                &previous) == FALSE) {
            VirtualFree(region, 0U, MEM_RELEASE);
            region = nullptr;
            throw std::runtime_error{"could not protect the legacy parameter boundary"};
        }
        value = reinterpret_cast<DIEFFECT_DX5*>(region + system_info.dwPageSize - sizeof(*value));
    }

    ~guarded_legacy_effect() {
        if (region != nullptr) {
            VirtualFree(region, 0U, MEM_RELEASE);
        }
    }

    guarded_legacy_effect(const guarded_legacy_effect&) = delete;
    guarded_legacy_effect& operator=(const guarded_legacy_effect&) = delete;
    guarded_legacy_effect(guarded_legacy_effect&&) = delete;
    guarded_legacy_effect& operator=(guarded_legacy_effect&&) = delete;

    BYTE* region{};
    DIEFFECT_DX5* value{};
};

[[nodiscard]] HRESULT set_standard_format(IDirectInputDevice8W& device) noexcept {
    const auto* const format = dinput::system_joystick_format();
    return format == nullptr ? DIERR_INVALIDPARAM : device.SetDataFormat(format);
}

[[nodiscard]] DIDATAFORMAT
single_actuator_format(DIOBJECTDATAFORMAT& object, GUID& axis_guid, const DWORD axis_id) noexcept {
    axis_guid = dinput::objects()[0U].guid;
    object = DIOBJECTDATAFORMAT{.pguid = &axis_guid,
        .dwOfs = 0U,
        .dwType = axis_id,
        .dwFlags = DIDOI_ASPECTPOSITION};
    return DIDATAFORMAT{.dwSize = sizeof(DIDATAFORMAT),
        .dwObjSize = sizeof(DIOBJECTDATAFORMAT),
        .dwFlags = DIDF_ABSAXIS,
        .dwDataSize = sizeof(LONG),
        .dwNumObjs = 1U,
        .rgodf = &object};
}

TEST_CASE("dinput.force_feedback_advertises_supported_effects_and_x_y_actuators",
    "[integration][dinput][force_feedback][com]") {
    const force_feedback_fixture fixture;
    parent_unknown parent;
    auto device = fixture.make_device(parent);

    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);
    REQUIRE(device->GetCapabilities(&capabilities) == DI_OK);
    CHECK((capabilities.dwFlags & DIDC_FORCEFEEDBACK) != 0U);
    CHECK((capabilities.dwFlags & DIDC_FFATTACK) != 0U);
    CHECK((capabilities.dwFlags & DIDC_FFFADE) != 0U);
    CHECK((capabilities.dwFlags & DIDC_STARTDELAY) != 0U);
    CHECK(capabilities.dwFFSamplePeriod == 4000U);
    CHECK(capabilities.dwFFMinTimeResolution == 4000U);

    effect_capture all_effects{};
    REQUIRE(device->EnumEffects(&capture_effect, &all_effects, DIEFT_ALL) == DI_OK);
    REQUIRE(all_effects.count == all_effects.values.size());
    CHECK((all_effects.values[1U].dwStaticParams & DIEP_ENVELOPE) == 0U);
    CHECK((all_effects.values[1U].dwDynamicParams & DIEP_ENVELOPE) == 0U);
    CHECK((all_effects.values[1U].dwEffType & static_cast<DWORD>(DIEFT_FFATTACK | DIEFT_FFFADE)) ==
          0U);
    CHECK((all_effects.values[0U].dwDynamicParams & DIEP_ENVELOPE) != 0U);
    effect_capture periodic_effects{};
    REQUIRE(device->EnumEffects(&capture_effect, &periodic_effects, DIEFT_PERIODIC) == DI_OK);
    CHECK(periodic_effects.count == 5U);

    object_capture actuators{};
    REQUIRE(device->EnumObjects(&capture_object, &actuators, DIDFT_FFACTUATOR) == DI_OK);
    REQUIRE(actuators.count == 2U);
    CHECK(DIDFT_GETINSTANCE(actuators.values[0U].dwType) == 0U);
    CHECK(DIDFT_GETINSTANCE(actuators.values[1U].dwType) == 1U);
    CHECK((actuators.values[0U].dwFlags & DIDOI_FFACTUATOR) != 0U);
    CHECK((actuators.values[1U].dwFlags & DIDOI_FFACTUATOR) != 0U);

    DIOBJECTDATAFORMAT actuator_descriptor{};
    GUID actuator_guid{};
    const auto actuator_data_format =
        single_actuator_format(actuator_descriptor, actuator_guid, actuators.values[0U].dwType);
    REQUIRE(device->SetDataFormat(&actuator_data_format) == DI_OK);

    DWORD custom_axis_offset{};
    LONG custom_direction{};
    DICONSTANTFORCE custom_force{.lMagnitude = 2'000L};
    auto custom_parameters = constant_effect(custom_axis_offset, custom_force, custom_direction);
    custom_parameters.dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
    IDirectInputEffect* custom_raw{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, &custom_parameters, &custom_raw, nullptr) ==
            DI_OK);
    const com_owner<IDirectInputEffect> custom_effect{custom_raw};
    DWORD custom_output_offset{};
    DIEFFECT custom_readback{};
    custom_readback.dwSize = sizeof(custom_readback);
    custom_readback.dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
    custom_readback.cAxes = 1U;
    custom_readback.rgdwAxes = &custom_output_offset;
    REQUIRE(custom_effect->GetParameters(&custom_readback, DIEP_AXES) == DI_OK);
    CHECK(custom_output_offset == custom_axis_offset);

    REQUIRE(set_standard_format(*device) == DI_OK);
    DIDEVICEOBJECTINSTANCEW object_info{};
    object_info.dwSize = sizeof(object_info);
    REQUIRE(device->GetObjectInfo(&object_info, actuators.values[0U].dwType, DIPH_BYID) == DI_OK);
    CHECK((object_info.dwFlags & DIDOI_FFACTUATOR) != 0U);

    auto axis = actuators.values[0U].dwType;
    LONG direction{};
    DICONSTANTFORCE force{.lMagnitude = 4'000L};
    const auto parameters = constant_effect(axis, force, direction);
    IDirectInputEffect* created{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, &parameters, &created, nullptr) == DI_OK);
    com_owner<IDirectInputEffect> created_effect{created};

    std::array<DWORD, 2U> output_axes{};
    std::array<LONG, 2U> output_directions{};
    DICONSTANTFORCE output_force{};
    DIEFFECT returned{};
    returned.dwSize = sizeof(returned);
    returned.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    returned.cAxes = static_cast<DWORD>(output_axes.size());
    returned.rgdwAxes = output_axes.data();
    returned.rglDirection = output_directions.data();
    returned.cbTypeSpecificParams = sizeof(output_force);
    returned.lpvTypeSpecificParams = &output_force;
    REQUIRE(created_effect->GetParameters(&returned, DIEP_ALLPARAMS) == DI_OK);
    CHECK(returned.cAxes == 1U);
    CHECK(output_axes[0U] == axis);
    CHECK(output_force.lMagnitude == force.lMagnitude);
}

TEST_CASE("dinput.force_feedback_is_not_advertised_without_managed_motor_support",
    "[integration][dinput][force_feedback][capabilities]") {
    const force_feedback_fixture fixture{motors{}};
    parent_unknown parent;
    auto device = fixture.make_device(parent);

    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);
    REQUIRE(device->GetCapabilities(&capabilities) == DI_OK);
    CHECK((capabilities.dwFlags & DIDC_FORCEFEEDBACK) == 0U);
    CHECK((capabilities.dwFlags &
              static_cast<DWORD>(DIDC_FFATTACK | DIDC_FFFADE | DIDC_STARTDELAY)) == 0U);

    effect_capture effects{};
    REQUIRE(device->EnumEffects(&capture_effect, &effects, DIEFT_ALL) == DI_OK);
    CHECK(effects.count == 0U);

    object_capture actuators{};
    REQUIRE(device->EnumObjects(&capture_object, &actuators, DIDFT_FFACTUATOR) == DI_OK);
    CHECK(actuators.count == 0U);

    auto gain = device_dword_property(7'500U);
    CHECK(inputnext_dinput_get_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) == DI_OK);
    CHECK(gain.dwData == DI_FFNOMINALMAX);
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) == DI_OK);

    IDirectInputEffect* effect{};
    CHECK(device->CreateEffect(GUID_ConstantForce, nullptr, &effect, nullptr) == DIERR_UNSUPPORTED);
    CHECK(effect == nullptr);
}

TEST_CASE("dinput.force_feedback_reset_invalidates_effects_and_turns_actuators_off",
    "[integration][dinput][force_feedback][commands]") {
    const force_feedback_fixture fixture;
    parent_unknown parent;
    auto device = fixture.make_device(parent);
    REQUIRE(set_standard_format(*device) == DI_OK);
    REQUIRE(device->SetCooperativeLevel(fixture.native.window.get(),
                DISCL_FOREGROUND | DISCL_EXCLUSIVE) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    IDirectInputEffect* raw_effect{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, nullptr, &raw_effect, nullptr) == DI_OK);
    com_owner<IDirectInputEffect> effect{raw_effect};
    REQUIRE(device->SendForceFeedbackCommand(DISFFC_RESET) == DI_OK);

    DWORD state{};
    REQUIRE(device->GetForceFeedbackState(&state) == DI_OK);
    CHECK((state & DIGFFS_ACTUATORSOFF) != 0U);
    CHECK((state & DIGFFS_EMPTY) != 0U);

    DWORD effect_status{};
    CHECK(effect->GetEffectStatus(&effect_status) == DIERR_NOTINITIALIZED);
    CHECK(effect->Start(1U, 0U) == DIERR_NOTINITIALIZED);

    REQUIRE(device->SendForceFeedbackCommand(DISFFC_SETACTUATORSON) == DI_OK);
    REQUIRE(device->GetForceFeedbackState(&state) == DI_OK);
    CHECK((state & DIGFFS_ACTUATORSON) != 0U);
    CHECK((state & DIGFFS_EMPTY) != 0U);

    IDirectInputEffect* recreated{};
    CHECK(device->CreateEffect(GUID_ConstantForce, nullptr, &recreated, nullptr) == DI_OK);
    CHECK(recreated != nullptr);
    if (recreated != nullptr) {
        recreated->Release();
    }
}

TEST_CASE("dinput.force_feedback_create_copies_current_and_bounded_dx5_parameters_offline",
    "[integration][dinput][force_feedback][com][parameters]") {
    const force_feedback_fixture fixture;
    parent_unknown parent;
    auto device = fixture.make_device(parent);
    REQUIRE(set_standard_format(*device) == DI_OK);

    object_capture actuators{};
    REQUIRE(device->EnumObjects(&capture_object, &actuators, DIDFT_FFACTUATOR) == DI_OK);
    REQUIRE(actuators.count == 2U);
    auto axis = actuators.values[1U].dwType;
    LONG direction = 45;
    DICONSTANTFORCE force{.lMagnitude = 7'500L};
    const auto current = constant_effect(axis, force, direction);

    IDirectInputEffect* current_raw{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, &current, &current_raw, nullptr) == DI_OK);
    const com_owner<IDirectInputEffect> current_effect{current_raw};

    const guarded_legacy_effect legacy_storage;
    auto& legacy = *legacy_storage.value;
    legacy.dwSize = sizeof(legacy);
    legacy.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    legacy.dwDuration = 1'000'000U;
    legacy.dwGain = DI_FFNOMINALMAX;
    legacy.dwTriggerButton = DIEB_NOTRIGGER;
    legacy.dwTriggerRepeatInterval = INFINITE;
    legacy.cAxes = 1U;
    legacy.rgdwAxes = &axis;
    legacy.rglDirection = &direction;
    legacy.cbTypeSpecificParams = sizeof(force);
    legacy.lpvTypeSpecificParams = &force;

    IDirectInputEffect* legacy_raw{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce,
                reinterpret_cast<const DIEFFECT*>(&legacy),
                &legacy_raw,
                nullptr) == DI_OK);
    const com_owner<IDirectInputEffect> legacy_effect{legacy_raw};
}

TEST_CASE("dinput.force_feedback_effect_retains_its_parent_and_enumeration_allows_release",
    "[integration][dinput][force_feedback][com][lifetime]") {
    const force_feedback_fixture fixture;
    std::atomic<std::uint32_t> parent_destroyed{};
    auto* const parent = new parent_unknown{&parent_destroyed};
    auto device = fixture.make_device(*parent);
    REQUIRE(set_standard_format(*device) == DI_OK);

    IDirectInputEffect* effect{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, nullptr, &effect, nullptr) == DI_OK);
    void* identity{};
    REQUIRE(effect->QueryInterface(IID_IUnknown, &identity) == S_OK);
    auto* const unknown = static_cast<IUnknown*>(identity);
    void* same_effect{};
    REQUIRE(unknown->QueryInterface(IID_IDirectInputEffect, &same_effect) == S_OK);
    CHECK(same_effect == effect);
    static_cast<IDirectInputEffect*>(same_effect)->Release();
    unknown->Release();

    release_effect_context context{.owned = effect, .device = &device, .parent = parent};
    REQUIRE(device->EnumCreatedEffectObjects(&release_created_effect, &context, 0U) == DI_OK);
    CHECK(context.called);
    CHECK(context.guid_read);
    CHECK(context.device_released);
    CHECK(context.parent_alive);
    CHECK_FALSE(device);
    CHECK(context.owned == nullptr);
    CHECK(parent->references() == 1U);
    CHECK(parent_destroyed.load(std::memory_order_acquire) == 0U);
    static_cast<void>(parent->Release());
    CHECK(parent_destroyed.load(std::memory_order_acquire) == 1U);
}

TEST_CASE("dinput.force_feedback_gain_is_writable_while_acquired_and_shutdown_detaches_effects",
    "[integration][dinput][force_feedback][com][lifecycle]") {
    const force_feedback_fixture fixture;
    parent_unknown parent;
    auto device = fixture.make_device(parent);
    REQUIRE(set_standard_format(*device) == DI_OK);
    REQUIRE(device->SetCooperativeLevel(fixture.native.window.get(),
                DISCL_FOREGROUND | DISCL_EXCLUSIVE) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    DIPROPDWORD gain{};
    gain.diph.dwSize = sizeof(gain);
    gain.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    gain.diph.dwHow = DIPH_DEVICE;
    gain.dwData = 7'500U;
    REQUIRE(inputnext_dinput_set_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) == DI_OK);
    gain.dwData = 0U;
    REQUIRE(inputnext_dinput_get_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) == DI_OK);
    CHECK(gain.dwData == 7'500U);

    IDirectInputEffect* effect{};
    REQUIRE(device->CreateEffect(GUID_ConstantForce, nullptr, &effect, nullptr) == DI_OK);
    fixture.access->close_admission();
    fixture.access->drain();

    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    CHECK(effect->GetParameters(&parameters, DIEP_GAIN) == DIERR_INPUTLOST);
    GUID effect_id{};
    CHECK(effect->GetEffectGuid(&effect_id) == DI_OK);
    CHECK(effect_id == GUID_ConstantForce);
    effect->Release();
    device.reset();
}

} // namespace
