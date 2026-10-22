#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

namespace inputnext::gameinput_detail {

class native_rumble_endpoint {
public:
    virtual ~native_rumble_endpoint() noexcept = default;

    native_rumble_endpoint(const native_rumble_endpoint&) = delete;
    native_rumble_endpoint& operator=(const native_rumble_endpoint&) = delete;
    native_rumble_endpoint(native_rumble_endpoint&&) = delete;
    native_rumble_endpoint& operator=(native_rumble_endpoint&&) = delete;

    [[nodiscard]] virtual output::native_result set(output::rumble_value value) noexcept = 0;

protected:
    native_rumble_endpoint() = default;
};

} // namespace inputnext::gameinput_detail
