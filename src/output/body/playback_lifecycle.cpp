#include "output/authority/state.hpp"

#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>

namespace inputnext::output {

void authority::implementation::retire_playback_locked(record& entry) noexcept {
    if (entry.playback.has_value()) {
        static_cast<void>(playback_bank.retire_lease(entry.playback->lease));
    }
    entry.playback.reset();
}

void authority::implementation::retire_playback_audio_locked(const audio_instance audio) noexcept {
    static_cast<void>(playback_bank.retire_audio(audio));
}

void authority::implementation::mark_started_playback_retired_locked(record& entry) noexcept {
    if (entry.playback.has_value()) {
        static_cast<void>(playback_bank.retire_started_lease(entry.playback->lease));
    }
}

} // namespace inputnext::output
