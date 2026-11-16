#pragma once

#include "output/composition/window_acquisition.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <memory>
#include <optional>

namespace inputnext::output::detail {

struct internal_grant final {
    internal_context_options options;
    std::shared_ptr<const window_acquisition> window;
    std::optional<backend_handle> backend;
};

} // namespace inputnext::output::detail
