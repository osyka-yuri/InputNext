#pragma once

#include <inputnext/history.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::history_detail {

struct source_clock_sample final {
    std::uint64_t timestamp_us{};
    std::uint64_t system_time_ms{};
};

struct reader_boundary final {
    history_domain domain{};
    std::uint64_t sequence{};
    std::uint64_t checkpoint_qpc{};
    source_clock_sample clock{};
};

struct observed_read final {
    history_batch batch{};
    std::optional<reader_boundary> boundary{std::nullopt};
    bool fresh_checkpoint{};
};

} // namespace inputnext::history_detail
