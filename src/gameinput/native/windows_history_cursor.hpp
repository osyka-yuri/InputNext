#pragma once

#include "native_gameinput.hpp"
#include "native_history.hpp"
#include "native_history_api.hpp"

#include <cstdint>
#include <memory>

namespace GameInput::v4 {
struct IGameInput;
}

namespace inputnext::gameinput_detail {

[[nodiscard]] native_history_read_kind classify_history_failure(std::uint32_t result_code) noexcept;

[[nodiscard]] native_result<std::unique_ptr<native_history_cursor>>
create_windows_history_cursor(GameInput::v4::IGameInput* game_input, native_device_handle device);

} // namespace inputnext::gameinput_detail
