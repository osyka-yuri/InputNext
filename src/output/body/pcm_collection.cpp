#include "output/authority/state.hpp"

#include "output/playback/body_playback_bank.hpp"
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <mutex>

namespace inputnext::output {

body_pcm_preparer authority::pcm_preparer() const noexcept {
    return implementation_->pcm_preparer;
}

void authority::collect_body_pcm() noexcept {
    auto& state = *implementation_;
    detail::body_playback_retirement_batch completed;
    {
        std::scoped_lock lock{state.gate};
        static_cast<void>(state.playback_bank.collect_nonbusy(completed));
    }
}

} // namespace inputnext::output
