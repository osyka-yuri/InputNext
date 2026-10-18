#pragma once

#include "metadata/provider.hpp"

#include <memory>

namespace inputnext::metadata_detail {

class windows_metadata_os;

[[nodiscard]] std::unique_ptr<metadata_provider> make_windows_metadata_provider();
// Internal component-test seam; production callers use the no-argument factory.
[[nodiscard]] std::unique_ptr<metadata_provider> make_windows_metadata_provider(
    windows_metadata_os& os);

} // namespace inputnext::metadata_detail
