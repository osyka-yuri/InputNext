#pragma once

#include "gameinput_probe/input/history/evidence.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <array>
#include <iosfwd>
#include <optional>

namespace inputnext::gameinput_detail {

class history_observer final {
public:
    explicit history_observer(gameinput_runtime& runtime) noexcept;

    void observe(const connection_inventory& connections,
        maintenance_time now,
        bool report,
        std::ostream& output);
    [[nodiscard]] bool close_all(std::ostream& output);
    [[nodiscard]] bool native_history_observed() const noexcept;

private:
    struct entry final {
        std::optional<history_reader_handle> reader;
        history_evidence evidence{};
        std::optional<button_mask> buttons;
        bool guide_known{};
    };

    void close(entry& observed, std::ostream& output);
    void read(entry& observed, maintenance_time now, bool report, std::ostream& output);

    gameinput_runtime& runtime_;
    std::array<entry, gameinput_live_connection_capacity> entries_{};
    bool observed_native_history_{};
    bool closed_cleanly_{true};
};

} // namespace inputnext::gameinput_detail
