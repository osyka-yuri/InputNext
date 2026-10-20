#pragma once

#include <inputnext/xinput.hpp>

#include <array>
#include <chrono>
#include <iosfwd>
#include <optional>

namespace inputnext {
class gameinput_xinput;
struct connection_inventory;
} // namespace inputnext

namespace inputnext::focus {
class observer;
}

namespace inputnext::gameinput_detail {

class xinput_observer final {
public:
    xinput_observer(gameinput_xinput& presentation, focus::observer& focus) noexcept;

    void observe(const connection_inventory& connections,
        std::chrono::steady_clock::time_point now,
        bool report,
        bool allow_metadata_query,
        std::ostream& output);

private:
    struct capability_schedule final {
        std::optional<xinput::assignment> assignment;
        std::chrono::steady_clock::time_point next_attempt{
            std::chrono::steady_clock::time_point::min()};
    };

    gameinput_xinput& presentation_;
    focus::observer& focus_;
    std::array<capability_schedule, 4U> capabilities_{};
};

} // namespace inputnext::gameinput_detail
