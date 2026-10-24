#pragma once

#include "profiles/model.hpp"

#include <filesystem>

namespace inputnext::runtime_detail {

[[nodiscard]] std::filesystem::path runtime_library_path();
[[nodiscard]] profiles::profile load_process_profile();

} // namespace inputnext::runtime_detail
