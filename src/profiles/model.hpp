#pragma once

#include "effects_profile.hpp"
#include "input/program.hpp"

namespace inputnext::profiles {

struct directinput_profile final {
    bool digital_triggers = false;
    double trigger_threshold = 0.5;

    friend bool operator==(const directinput_profile&, const directinput_profile&) = default;
};

struct profile final {
    input::program input{input::program::defaults()};
    effects_profile effects;
    directinput_profile directinput;

    friend bool operator==(const profile&, const profile&) = default;
};

} // namespace inputnext::profiles
