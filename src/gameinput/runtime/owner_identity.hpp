#pragma once

#include <cstddef>
#include <cstdint>

namespace inputnext::gameinput_detail {

struct runtime_owner_identity final {
    std::size_t slot_index{};
    std::uint64_t generation{};
};

} // namespace inputnext::gameinput_detail
