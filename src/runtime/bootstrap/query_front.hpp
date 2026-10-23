#pragma once

#include "runtime/abi/runtime_api.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace inputnext::runtime_detail {

class bootstrap_owner;

struct query_front_operations final {
    BOOL (*pin_module)(void* context) noexcept;
    bootstrap_owner* (*create_owner)(void* context);
    const input_next_runtime_api_v1* (*stable_api)(void* context, bootstrap_owner* owner) noexcept;
};

// Constant-initialized, process-retained front. Its callbacks and context are borrowed; test
// fixtures must keep them alive through all queries. The front never owns or destroys the owner.
struct query_front final {
    constexpr query_front(query_front_operations operations_value, void* borrowed_context) noexcept
        : owner{nullptr},
          module_pinned{false},
          operations{operations_value},
          context{borrowed_context} {}

    INIT_ONCE owner_once = INIT_ONCE_STATIC_INIT;
    std::atomic<bootstrap_owner*> owner;
    std::atomic<bool> module_pinned;
    const query_front_operations operations;
    void* const context;
};

[[nodiscard]] const input_next_runtime_api_v1* query_runtime_api(query_front& front,
    std::uint32_t requested_abi) noexcept;

static_assert(std::is_trivially_destructible_v<query_front>);
static_assert(std::is_trivially_destructible_v<query_front_operations>);

} // namespace inputnext::runtime_detail
