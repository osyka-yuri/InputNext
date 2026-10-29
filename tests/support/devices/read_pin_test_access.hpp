#pragma once

#include "devices/connection_record.hpp"
#include <inputnext/input_binding.hpp>

#include <stdexcept>

namespace inputnext::detail {

struct read_pin_test_access final {
    [[nodiscard]] static connection_record& record(const read_pin& pin) {
        if (pin.record_ == nullptr) {
            throw std::invalid_argument{"A retained connection pin is required"};
        }
        return *pin.record_;
    }
};

} // namespace inputnext::detail
