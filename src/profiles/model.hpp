#pragma once

#include "effects_profile.hpp"
#include "input/program.hpp"

namespace inputnext::profiles {

struct profile final {
    input::program input{input::program::defaults()};
    effects_profile effects;

    friend bool operator==(const profile&, const profile&) = default;
};

} // namespace inputnext::profiles
