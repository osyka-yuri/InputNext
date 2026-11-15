#pragma once

#include <cstdint>
#include <memory>

namespace inputnext::focus::detail {

class window_events final {
public:
    struct sample final {
        std::uint64_t transition{};
        bool available{};
    };

    window_events() noexcept;
    ~window_events();

    window_events(const window_events&) = delete;
    window_events& operator=(const window_events&) = delete;
    window_events(window_events&&) = delete;
    window_events& operator=(window_events&&) = delete;

    [[nodiscard]] sample read() const noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::focus::detail
