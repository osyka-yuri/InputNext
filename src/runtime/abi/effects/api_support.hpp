#pragma once

#include <input_next_core.h>
#include <input_next_effects.h>

#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace inputnext::runtime_detail::effects_api_support {

template <typename Value>
void commit_output(Value* const destination, const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memcpy(destination, &value, sizeof(value));
}

template <typename Operation>
[[nodiscard]] input_next_status guarded(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return INPUT_NEXT_STATUS_CAPACITY_EXCEEDED;
    } catch (...) {
        return INPUT_NEXT_STATUS_IO_FAILED;
    }
}

} // namespace inputnext::runtime_detail::effects_api_support
