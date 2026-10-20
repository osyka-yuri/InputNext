#pragma once

#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace inputnext::focus {
struct snapshot;
}

namespace inputnext::xinput {
struct assignment;
}

namespace inputnext::gameinput_detail {

[[nodiscard]] std::string format_xinput_focus(const focus::snapshot& snapshot);
[[nodiscard]] std::string format_xinput_state(const xinput::assignment& assignment,
    std::optional<std::uint64_t> latest_revision,
    const focus::snapshot& focus,
    DWORD standard_result,
    const XINPUT_STATE& standard,
    DWORD extended_result,
    const XINPUT_STATE& extended);
[[nodiscard]] std::string format_xinput_capabilities(const xinput::assignment& assignment,
    DWORD result,
    const xinput::capabilities_ex& capabilities);

} // namespace inputnext::gameinput_detail
