#pragma once

#include <cstdint>
#include <iosfwd>

namespace inputnext::gameinput_detail {

struct probe_options final {
    std::uint32_t duration_ms{10'000U};
    bool require_device{};
    bool require_reading{};
    bool history{};
    bool require_history{};
    bool xinput{};
};

[[nodiscard]] int run_probe_worker(const probe_options& options, std::ostream& output);

} // namespace inputnext::gameinput_detail
