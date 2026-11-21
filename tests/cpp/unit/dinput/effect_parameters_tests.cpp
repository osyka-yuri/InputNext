#include "dinput/force_feedback/parameters.hpp"

#include "dinput/format.hpp"
#include "dinput/profile.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <dinput.h>

#include <array>
#include <cstddef>

using namespace inputnext::dinput;
using namespace inputnext::dinput::force_feedback;

namespace {

struct constant_parameters final {
    std::array<DWORD, 2U> axes{objects()[0U].id | DIDFT_FFACTUATOR,
        objects()[1U].id | DIDFT_FFACTUATOR};
    std::array<LONG, 2U> directions{100, -100};
    DICONSTANTFORCE force{10000};
    DIENVELOPE envelope{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 5000U,
        .dwAttackTime = 1000U,
        .dwFadeLevel = 0U,
        .dwFadeTime = 1000U};
    DIEFFECT effect{};

    explicit constant_parameters(DWORD size = static_cast<DWORD>(sizeof(DIEFFECT))) {
        effect.dwSize = size;
        effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
        effect.dwDuration = 500000U;
        effect.dwGain = 8000U;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = INFINITE;
        effect.cAxes = static_cast<DWORD>(axes.size());
        effect.rgdwAxes = axes.data();
        effect.rglDirection = directions.data();
        effect.lpEnvelope = &envelope;
        effect.cbTypeSpecificParams = static_cast<DWORD>(sizeof(force));
        effect.lpvTypeSpecificParams = &force;
        effect.dwStartDelay = 250U;
    }
};

} // namespace

TEST_CASE("dinput.effects.catalogue_exposes_only_supported_waveforms", "[unit][dinput]") {
    const auto catalogue = effects();
    REQUIRE(catalogue.size() == 7U);
    CHECK(find_effect(GUID_ConstantForce) == catalogue.data());
    CHECK(find_effect(GUID_RampForce) == &catalogue[1U]);
    CHECK(find_effect(GUID_Square) == &catalogue[2U]);
    CHECK(find_effect(GUID_Sine) == &catalogue[3U]);
    CHECK(find_effect(GUID_Triangle) == &catalogue[4U]);
    CHECK(find_effect(GUID_SawtoothUp) == &catalogue[5U]);
    CHECK(find_effect(GUID_SawtoothDown) == &catalogue[6U]);
    CHECK(actuator(0U));
    CHECK(actuator(1U));
    CHECK_FALSE(actuator(2U));
    CHECK((catalogue[0U].dynamic_params & DIEP_GAIN) != 0U);
    CHECK((catalogue[1U].type & DIEFT_FFATTACK) == 0U);
    CHECK((catalogue[1U].type & DIEFT_FFFADE) == 0U);
    CHECK(((catalogue[1U].static_params | catalogue[1U].dynamic_params) & DIEP_ENVELOPE) == 0U);
    CHECK((catalogue[1U].dynamic_params & DIEP_ENVELOPE) == 0U);
}

TEST_CASE("dinput.effects.full_parameters_are_copied_and_legacy_sizes_are_bounded",
    "[unit][dinput]") {
    auto parameters = effect_parameters{};
    const constant_parameters input{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);

    CHECK(set_parameters(parameters, *effect, &input.effect, DIEP_ALLPARAMS, nullptr) == DI_OK);
    CHECK(complete(parameters));
    CHECK(parameters.duration == input.effect.dwDuration);
    CHECK(parameters.gain == input.effect.dwGain);
    CHECK(parameters.start_delay == input.effect.dwStartDelay);
    CHECK(parameters.axes[0U] == 0U);
    CHECK(parameters.axes[1U] == 1U);
    CHECK(std::get<DICONSTANTFORCE>(parameters.force).lMagnitude == 10000);
    const auto envelope = parameters.envelope.value_or(DIENVELOPE{});
    REQUIRE(parameters.envelope.has_value());
    CHECK(envelope.dwAttackLevel == 5000U);

    auto legacy = DIEFFECT_DX5{};
    legacy.dwSize = static_cast<DWORD>(sizeof(legacy));
    legacy.dwDuration = 1000U;
    CHECK(set_parameters(parameters,
              *effect,
              reinterpret_cast<const DIEFFECT*>(&legacy),
              DIEP_STARTDELAY,
              nullptr) == DIERR_INVALIDPARAM);
    CHECK(parameters.start_delay == input.effect.dwStartDelay);
}

TEST_CASE("dinput.effects.dx5_all_parameters_round_trip_without_reading_start_delay",
    "[unit][dinput]") {
    const auto input = constant_parameters{};
    DIEFFECT_DX5 legacy{};
    legacy.dwSize = static_cast<DWORD>(sizeof(legacy));
    legacy.dwFlags = input.effect.dwFlags;
    legacy.dwDuration = input.effect.dwDuration;
    legacy.dwSamplePeriod = input.effect.dwSamplePeriod;
    legacy.dwGain = input.effect.dwGain;
    legacy.dwTriggerButton = input.effect.dwTriggerButton;
    legacy.dwTriggerRepeatInterval = input.effect.dwTriggerRepeatInterval;
    legacy.cAxes = input.effect.cAxes;
    legacy.rgdwAxes = input.effect.rgdwAxes;
    legacy.rglDirection = input.effect.rglDirection;
    legacy.lpEnvelope = input.effect.lpEnvelope;
    legacy.cbTypeSpecificParams = input.effect.cbTypeSpecificParams;
    legacy.lpvTypeSpecificParams = input.effect.lpvTypeSpecificParams;

    auto parameters = effect_parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    CHECK(set_parameters(parameters,
              *effect,
              reinterpret_cast<const DIEFFECT*>(&legacy),
              DIEP_ALLPARAMS_DX5,
              nullptr) == DI_OK);
    CHECK(complete(parameters));
    CHECK(parameters.start_delay == 0U);

    std::array<DWORD, 2U> axes{};
    std::array<LONG, 2U> directions{};
    auto force = DICONSTANTFORCE{};
    auto envelope = DIENVELOPE{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 0U,
        .dwAttackTime = 0U,
        .dwFadeLevel = 0U,
        .dwFadeTime = 0U};
    DIEFFECT_DX5 output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    output.cAxes = static_cast<DWORD>(axes.size());
    output.rgdwAxes = axes.data();
    output.rglDirection = directions.data();
    output.lpEnvelope = &envelope;
    output.cbTypeSpecificParams = static_cast<DWORD>(sizeof(force));
    output.lpvTypeSpecificParams = &force;
    CHECK(get_parameters(parameters,
              *effect,
              reinterpret_cast<DIEFFECT*>(&output),
              DIEP_ALLPARAMS_DX5,
              nullptr) == DI_OK);
    CHECK(output.dwDuration == input.effect.dwDuration);
    CHECK(force.lMagnitude == input.force.lMagnitude);

    DIEFFECT current{};
    current.dwSize = static_cast<DWORD>(sizeof(current));
    current.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    current.cAxes = static_cast<DWORD>(axes.size());
    current.rgdwAxes = axes.data();
    current.rglDirection = directions.data();
    current.lpEnvelope = &envelope;
    current.cbTypeSpecificParams = static_cast<DWORD>(sizeof(force));
    current.lpvTypeSpecificParams = &force;
    CHECK(get_parameters(parameters, *effect, &current, DIEP_ALLPARAMS, nullptr) == DI_OK);
    CHECK(current.dwStartDelay == 0U);
}

TEST_CASE("dinput.effects.all_seven_effects_accept_their_exact_payload_sizes", "[unit][dinput]") {
    std::array<DWORD, 2U> axes{objects()[0U].id | DIDFT_FFACTUATOR,
        objects()[1U].id | DIDFT_FFACTUATOR};
    std::array<LONG, 2U> directions{0, 1};
    DIENVELOPE envelope{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 0U,
        .dwAttackTime = 0U,
        .dwFadeLevel = 0U,
        .dwFadeTime = 0U};
    for (const auto& descriptor : effects()) {
        DIEFFECT input{};
        input.dwSize = static_cast<DWORD>(sizeof(input));
        input.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
        input.dwDuration = 100000U;
        input.dwGain = DI_FFNOMINALMAX;
        input.dwTriggerButton = DIEB_NOTRIGGER;
        input.dwTriggerRepeatInterval = INFINITE;
        input.cAxes = static_cast<DWORD>(axes.size());
        input.rgdwAxes = axes.data();
        input.rglDirection = directions.data();
        input.lpEnvelope = descriptor.wave == waveform::ramp ? nullptr : &envelope;

        DICONSTANTFORCE constant{.lMagnitude = 5000};
        DIRAMPFORCE ramp{.lStart = -5000, .lEnd = 5000};
        DIPERIODIC periodic{.dwMagnitude = 5000U, .lOffset = 0, .dwPhase = 0U, .dwPeriod = 10000U};
        switch (descriptor.wave) {
        case waveform::constant:
            input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(constant));
            input.lpvTypeSpecificParams = &constant;
            break;
        case waveform::ramp:
            input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(ramp));
            input.lpvTypeSpecificParams = &ramp;
            break;
        case waveform::square:
        case waveform::sine:
        case waveform::triangle:
        case waveform::sawtooth_up:
        case waveform::sawtooth_down:
            input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(periodic));
            input.lpvTypeSpecificParams = &periodic;
            break;
        }

        auto parameters = effect_parameters{};
        INFO(descriptor.name);
        const auto selected = descriptor.static_params | descriptor.dynamic_params;
        CHECK(set_parameters(parameters, descriptor, &input, selected, nullptr) == DI_OK);
        CHECK(complete(parameters));
    }
}

TEST_CASE("dinput.effects.partial_updates_are_transactional_and_preserve_other_fields",
    "[unit][dinput]") {
    auto parameters = effect_parameters{};
    DIEFFECT input{};
    input.dwSize = static_cast<DWORD>(sizeof(input));
    input.dwGain = 7500U;
    REQUIRE(
        set_parameters(parameters, *find_effect(GUID_ConstantForce), &input, DIEP_GAIN, nullptr) ==
        DI_OK);
    CHECK(parameters.gain == 7500U);
    CHECK(parameters.duration == 0U);
    CHECK_FALSE(complete(parameters));

    auto force = DICONSTANTFORCE{.lMagnitude = 10001};
    input.dwGain = 2500U;
    input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(force));
    input.lpvTypeSpecificParams = &force;
    CHECK(set_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &input,
              DIEP_GAIN | DIEP_TYPESPECIFICPARAMS,
              nullptr) == DIERR_INVALIDPARAM);
    CHECK(parameters.gain == 7500U);
    CHECK((parameters.valid & DIEP_TYPESPECIFICPARAMS) == 0U);
}

TEST_CASE("dinput.effects.axis_selectors_resolve_through_the_selected_format", "[unit][dinput]") {
    compiled_format format{};
    REQUIRE(compile_format(c_dfDIJoystick, format, device_model{true}) == DI_OK);
    effect_parameters parameters{};
    constant_parameters input{};
    input.effect.dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
    input.axes = {DIJOFS_X, DIJOFS_Y};
    input.effect.rgdwAxes = input.axes.data();

    REQUIRE(set_parameters(parameters,
                *find_effect(GUID_ConstantForce),
                &input.effect,
                DIEP_AXES | DIEP_DIRECTION,
                &format) == DI_OK);
    CHECK(parameters.axes[0U] == 0U);
    CHECK(parameters.axes[1U] == 1U);
    CHECK(parameters.coordinate_flags == DIEFF_CARTESIAN);

    std::array<DWORD, 2U> out_axes{0xccccccccU, 0xccccccccU};
    std::array<LONG, 2U> out_directions{12345, 12345};
    DIEFFECT output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    output.cAxes = 1U;
    output.rgdwAxes = out_axes.data();
    output.rglDirection = out_directions.data();
    CHECK(get_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &output,
              DIEP_AXES | DIEP_DIRECTION,
              &format) == DIERR_MOREDATA);
    CHECK(output.cAxes == 2U);
    CHECK(out_axes[0U] == 0xccccccccU);
    CHECK(out_directions[0U] == 12345);

    output.cAxes = 2U;
    CHECK(get_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &output,
              DIEP_AXES | DIEP_DIRECTION,
              &format) == DI_OK);
    CHECK(out_axes[0U] == (objects()[0U].id | DIDFT_FFACTUATOR));
    CHECK(out_axes[1U] == (objects()[1U].id | DIDFT_FFACTUATOR));
    CHECK(out_directions == input.directions);
}

TEST_CASE("dinput.effects.axes_can_be_replaced_after_initialization", "[unit][dinput]") {
    auto input = constant_parameters{};
    input.effect.cAxes = 1U;
    effect_parameters parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);

    input.axes[0U] = objects()[1U].id | DIDFT_FFACTUATOR;
    REQUIRE(set_parameters(parameters, *effect, &input.effect, DIEP_AXES, nullptr) == DI_OK);
    CHECK(parameters.axis_count == 1U);
    CHECK(parameters.axes[0U] == 1U);
    CHECK(parameters.direction_count == 1U);
    CHECK(parameters.directions[0U] == 100);
}

TEST_CASE("dinput.effects_axis_count_changes_require_matching_axis_and_direction_updates",
    "[unit][dinput]") {
    auto input = constant_parameters{};
    effect_parameters parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);

    input.effect.cAxes = 1U;
    input.axes[0U] = objects()[1U].id | DIDFT_FFACTUATOR;
    input.directions[0U] = -321;
    CHECK(set_parameters(parameters, *effect, &input.effect, DIEP_AXES, nullptr) ==
          DIERR_INVALIDPARAM);
    CHECK(parameters.axis_count == 2U);
    CHECK(parameters.axes == std::array<std::size_t, 2U>{0U, 1U});
    CHECK(parameters.direction_count == 2U);
    CHECK(parameters.directions == std::array<LONG, 2U>{100, -100});

    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);
    CHECK(parameters.axis_count == 1U);
    CHECK(parameters.axes[0U] == 1U);
    CHECK(parameters.direction_count == 1U);
    CHECK(parameters.directions[0U] == -321);

    input.effect.cAxes = 2U;
    input.axes = {objects()[0U].id | DIDFT_FFACTUATOR, objects()[1U].id | DIDFT_FFACTUATOR};
    input.directions = {777, -888};
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);
    CHECK(parameters.axis_count == 2U);
    CHECK(parameters.axes == std::array<std::size_t, 2U>{0U, 1U});
    CHECK(parameters.direction_count == 2U);
    CHECK(parameters.directions == std::array<LONG, 2U>{777, -888});
}

TEST_CASE("dinput.effects.direction_readback_accepts_coordinate_masks_and_keeps_original_format",
    "[unit][dinput]") {
    auto input = constant_parameters{};
    auto parameters = effect_parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);

    std::array<LONG, 2U> directions{};
    DIEFFECT output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN | DIEFF_POLAR;
    output.cAxes = static_cast<DWORD>(directions.size());
    output.rglDirection = directions.data();
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_CARTESIAN));
    CHECK(directions == input.directions);

    input.effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_POLAR;
    input.directions = {9000, 0};
    REQUIRE(set_parameters(parameters, *effect, &input.effect, DIEP_DIRECTION, nullptr) == DI_OK);
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL;
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_POLAR));
    CHECK(directions == input.directions);

    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_SPHERICAL;
    CHECK(
        get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DIERR_UNSUPPORTED);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_SPHERICAL));
    CHECK(directions == input.directions);
}

TEST_CASE("dinput.effects.direction_readback_converts_cartesian_and_polar_conventions",
    "[unit][dinput]") {
    auto input = constant_parameters{};
    auto parameters = effect_parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    input.directions = {1, 0};
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);

    std::array<LONG, 2U> directions{};
    DIEFFECT output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_POLAR;
    output.cAxes = static_cast<DWORD>(directions.size());
    output.rglDirection = directions.data();
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_POLAR));
    CHECK(directions == std::array<LONG, 2U>{9000, 0});

    input.effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_POLAR;
    input.directions = {0, 0};
    REQUIRE(set_parameters(parameters, *effect, &input.effect, DIEP_DIRECTION, nullptr) == DI_OK);
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_CARTESIAN));
    CHECK(directions == std::array<LONG, 2U>{0, -DI_FFNOMINALMAX});

    input.directions = {9000, 0};
    REQUIRE(set_parameters(parameters, *effect, &input.effect, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(directions == std::array<LONG, 2U>{DI_FFNOMINALMAX, 0});

    input.effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    input.directions = {0, 0};
    REQUIRE(set_parameters(parameters, *effect, &input.effect, DIEP_DIRECTION, nullptr) == DI_OK);
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_POLAR;
    directions = {77, 88};
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) ==
          DIERR_INVALIDPARAM);
    CHECK(output.dwFlags == (DIEFF_OBJECTIDS | DIEFF_POLAR));
    CHECK(directions == std::array<LONG, 2U>{77, 88});
}

TEST_CASE("dinput.effects.direction_readback_reports_exact_axis_count_with_extra_capacity",
    "[unit][dinput]") {
    const constant_parameters input{};
    effect_parameters parameters{};
    const auto* effect = find_effect(GUID_ConstantForce);
    REQUIRE(effect != nullptr);
    REQUIRE(
        set_parameters(parameters, *effect, &input.effect, DIEP_AXES | DIEP_DIRECTION, nullptr) ==
        DI_OK);

    std::array<DWORD, 3U> axes{0xccccccccU, 0xccccccccU, 0xccccccccU};
    DIEFFECT output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS;
    output.cAxes = static_cast<DWORD>(axes.size());
    output.rgdwAxes = axes.data();
    CHECK(get_parameters(parameters, *effect, &output, DIEP_AXES, nullptr) == DI_OK);
    CHECK(output.cAxes == 2U);
    CHECK(axes[2U] == 0xccccccccU);

    std::array<LONG, 3U> directions{12345, 12345, 12345};
    output.dwFlags = DIEFF_CARTESIAN;
    output.cAxes = static_cast<DWORD>(directions.size());
    output.rglDirection = directions.data();
    CHECK(get_parameters(parameters, *effect, &output, DIEP_DIRECTION, nullptr) == DI_OK);
    CHECK(output.cAxes == 2U);
    CHECK(directions[2U] == 12345);
}

TEST_CASE("dinput.effects.polar_direction_requires_a_zero_reserved_component", "[unit][dinput]") {
    auto input = constant_parameters{};
    input.effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_POLAR;
    input.directions = {9000, 1};
    effect_parameters parameters{};
    const auto saved = parameters;

    CHECK(set_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &input.effect,
              DIEP_DIRECTION,
              nullptr) == DIERR_INVALIDPARAM);
    CHECK(parameters.direction_count == saved.direction_count);
    CHECK(parameters.directions == saved.directions);
}

TEST_CASE("dinput.effects.readback_checks_all_capacities_before_writing", "[unit][dinput]") {
    auto parameters = effect_parameters{};
    const constant_parameters input{};
    REQUIRE(set_parameters(parameters,
                *find_effect(GUID_ConstantForce),
                &input.effect,
                DIEP_ALLPARAMS,
                nullptr) == DI_OK);

    std::array<DWORD, 2U> axes{0xccccccccU, 0xccccccccU};
    std::array<LONG, 2U> directions{12345, 12345};
    DICONSTANTFORCE force{.lMagnitude = 321};
    DIEFFECT output{};
    output.dwSize = static_cast<DWORD>(sizeof(output));
    output.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    output.cAxes = 2U;
    output.rgdwAxes = axes.data();
    output.rglDirection = directions.data();
    output.cbTypeSpecificParams = 0U;
    output.lpvTypeSpecificParams = &force;
    output.dwGain = 123U;
    CHECK(get_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &output,
              DIEP_GAIN | DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS,
              nullptr) == DIERR_MOREDATA);
    CHECK(output.cbTypeSpecificParams == sizeof(DICONSTANTFORCE));
    CHECK(output.dwGain == 123U);
    CHECK(axes[0U] == 0xccccccccU);
    CHECK(directions[0U] == 12345);
    CHECK(force.lMagnitude == 321);
}

TEST_CASE("dinput.effects.ramp_ignores_absent_envelope_but_rejects_nonnull_envelope",
    "[unit][dinput]") {
    auto parameters = effect_parameters{};
    auto axes = std::array<DWORD, 2U>{objects()[0U].id | DIDFT_FFACTUATOR,
        objects()[1U].id | DIDFT_FFACTUATOR};
    auto directions = std::array<LONG, 2U>{0, 1};
    auto force = DIRAMPFORCE{.lStart = -5000, .lEnd = 9000};
    DIEFFECT input{};
    input.dwSize = static_cast<DWORD>(sizeof(input));
    input.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    input.dwDuration = 100000U;
    input.dwGain = DI_FFNOMINALMAX;
    input.dwTriggerButton = DIEB_NOTRIGGER;
    input.dwTriggerRepeatInterval = INFINITE;
    input.cAxes = static_cast<DWORD>(axes.size());
    input.rgdwAxes = axes.data();
    input.rglDirection = directions.data();
    input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(force));
    input.lpvTypeSpecificParams = &force;

    const auto* effect = find_effect(GUID_RampForce);
    REQUIRE(effect != nullptr);
    CHECK((effect->static_params & DIEP_ENVELOPE) == 0U);
    CHECK((effect->dynamic_params & DIEP_ENVELOPE) == 0U);
    CHECK(set_parameters(parameters, *effect, &input, DIEP_ALLPARAMS, nullptr) == DI_OK);
    CHECK(complete(parameters));
    CHECK_FALSE(parameters.envelope.has_value());
    CHECK(std::get<DIRAMPFORCE>(parameters.force).lEnd == 9000);

    auto envelope = DIENVELOPE{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 0U,
        .dwAttackTime = 0U,
        .dwFadeLevel = 0U,
        .dwFadeTime = 0U};
    input.lpEnvelope = &envelope;
    CHECK(set_parameters(parameters, *effect, &input, DIEP_ENVELOPE, nullptr) == DIERR_UNSUPPORTED);
    CHECK_FALSE(parameters.envelope.has_value());
}

TEST_CASE("dinput.effects.rejects_unsupported_waveforms_and_malformed_periodic_values",
    "[unit][dinput]") {
    auto parameters = effect_parameters{};
    auto periodic =
        DIPERIODIC{.dwMagnitude = 5000U, .lOffset = 0, .dwPhase = 0U, .dwPeriod = 10000U};
    DIEFFECT input{};
    input.dwSize = static_cast<DWORD>(sizeof(input));
    input.cbTypeSpecificParams = static_cast<DWORD>(sizeof(periodic));
    input.lpvTypeSpecificParams = &periodic;
    const auto* effect = find_effect(GUID_Sine);
    REQUIRE(effect != nullptr);
    CHECK(set_parameters(parameters, *effect, &input, DIEP_TYPESPECIFICPARAMS, nullptr) == DI_OK);
    const auto saved = parameters;

    periodic.dwPhase = 36000U;
    CHECK(set_parameters(parameters, *effect, &input, DIEP_TYPESPECIFICPARAMS, nullptr) ==
          DIERR_INVALIDPARAM);
    CHECK(std::get<DIPERIODIC>(parameters.force).dwPhase ==
          std::get<DIPERIODIC>(saved.force).dwPhase);
    CHECK(find_effect(GUID_Spring) == nullptr);
    CHECK(set_parameters(parameters,
              *find_effect(GUID_ConstantForce),
              &input,
              DIEP_TYPESPECIFICPARAMS,
              nullptr) == DIERR_INVALIDPARAM);
}
