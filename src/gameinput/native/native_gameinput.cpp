#include "native_gameinput.hpp"

#include "gameinput/input_profile.hpp"
#include "input/touch_resolver.hpp"
#include "native_history.hpp"

#include <expected>
#include <memory>

namespace inputnext::gameinput_detail {

native_result<std::unique_ptr<native_history_cursor>> native_gameinput::create_history_cursor(
    native_device_handle device,
    const native_input_profile& profile,
    const input::touch_snapshot& touch_context) {
    static_cast<void>(touch_context);
    if (profile.history == input_history_path::composite) {
        return std::unexpected(native_failure{
            .cause = native_failure_cause::api_unavailable,
            .input_reason = input_fallback_reason::incoherent_reading,
        });
    }
    return create_history_cursor(device);
}

} // namespace inputnext::gameinput_detail
