#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/latest_value.hpp>
#include <inputnext/source_lifetime.hpp>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

#include <cstdint>

namespace inputnext::detail {

enum class connection_phase : std::uint8_t {
    free,
    reserved,
    live,
    retired,
    draining,
};

struct connection_record final {
    std::mutex gate;
    std::condition_variable pins_changed;
    connection_target target{};
    std::size_t pin_count{};
    connection_phase phase{connection_phase::free};
    std::optional<latest_value> latest;
    std::unique_ptr<source_lifetime> source;
};

} // namespace inputnext::detail
