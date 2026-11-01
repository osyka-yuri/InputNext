#pragma once

#include <filesystem>

namespace inputnext::gameinput_detail {

[[nodiscard]] std::filesystem::path current_executable_path();

} // namespace inputnext::gameinput_detail
