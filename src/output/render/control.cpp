#include "control.hpp"
#include "output/authority/admitted_body_render.hpp"
#include "output/execution/engine.hpp"
#include "output/executor/executor_state.hpp"

#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <expected>
#include <optional>

namespace inputnext::output::detail {

execution_result<std::optional<admitted_body_render>> body_render_control::admit_current(
    const backend_handle expected_backend,
    const audio_instance expected_audio,
    const std::size_t requested_frames) const noexcept {
    auto& state = *owner_.implementation_;
    const executor::implementation::operation active{state};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return state.engine->admit_current_body_render(expected_backend,
        expected_audio,
        requested_frames);
}

} // namespace inputnext::output::detail
