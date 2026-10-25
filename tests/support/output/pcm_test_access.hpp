#pragma once

#include "output/pcm/body_pcm_internal.hpp"

#include <inputnext/output_body_pcm.hpp>

#include <memory>
#include <utility>

namespace inputnext::test::output {

using controlled_pcm_memory_port = inputnext::output::detail::pcm_memory_port;

[[nodiscard]] inline inputnext::output::body_pcm_preparer make_pcm_preparer(
    std::shared_ptr<controlled_pcm_memory_port> memory) {
    return inputnext::output::detail::body_pcm_preparer_test_access::make_preparer(
        std::move(memory));
}

} // namespace inputnext::test::output
