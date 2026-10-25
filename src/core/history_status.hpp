#pragma once

#include "core.hpp"
#include <inputnext/history.hpp>

namespace inputnext::core::detail {

[[nodiscard]] status map_history_failure(history_error error) noexcept;

[[nodiscard]] inline bool cleanup_completed(history_error error) noexcept {
    return error == history_error::closed || error == history_error::invalid_reader;
}

} // namespace inputnext::core::detail
