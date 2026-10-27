#include "effects_api.hpp"

#include "effects/effects.hpp"
#include "effects_api_support.hpp"
#include "effects_projection.hpp"
#include "runtime/abi/session_access.hpp"
#include "runtime/session/native_session.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/device_identity.hpp>

namespace inputnext::runtime_detail {

using effects_api_support::commit_output;
using effects_api_support::guarded;
using effects_projection::to_c_status;

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_query_progress(
    const input_next_effects_progress_query_v1* const query,
    input_next_effects_progress_v1* const out_progress) noexcept {
    return guarded([&] {
        if (query == nullptr || out_progress == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (query->struct_size != sizeof(*query)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (!effects_projection::valid_output_header(*out_progress)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto query_input = *query;
        connection_target target{};
        effects::publication_revision publication{};
        if (!effects_projection::parse_progress_query(query_input, target, publication)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto progress = session->effects().query_progress(target, publication);
        if (!progress.has_value()) {
            return to_c_status(progress.error());
        }
        input_next_effects_progress_v1 staged{};
        if (!effects_projection::write_progress(*progress, staged)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_progress, staged);
        return INPUT_NEXT_STATUS_OK;
    });
}

} // namespace inputnext::runtime_detail
