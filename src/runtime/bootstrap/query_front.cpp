#include "query_front.hpp"

#include "runtime/abi/runtime_api.hpp"

#include <atomic>
#include <cstdint>

namespace inputnext::runtime_detail {
namespace {

constinit thread_local query_front* active_front{};

[[nodiscard]] bool valid_operations(const query_front_operations& operations) noexcept {
    return operations.pin_module != nullptr && operations.create_owner != nullptr &&
           operations.stable_api != nullptr;
}

BOOL CALLBACK create_owner_once([[maybe_unused]] PINIT_ONCE once,
    PVOID parameter,
    [[maybe_unused]] PVOID* callback_context) noexcept {
    auto& front = *static_cast<query_front*>(parameter);
    if (!front.module_pinned.load(std::memory_order_acquire)) {
        if (front.operations.pin_module(front.context) == FALSE) {
            return FALSE;
        }
        front.module_pinned.store(true, std::memory_order_release);
    }

    try {
        auto* const owner = front.operations.create_owner(front.context);
        if (owner == nullptr) {
            return FALSE;
        }
        front.owner.store(owner, std::memory_order_release);
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

class front_recursion_scope final {
public:
    explicit front_recursion_scope(query_front& front) noexcept : previous_{active_front} {
        active_front = &front;
    }

    ~front_recursion_scope() { active_front = previous_; }

    front_recursion_scope(const front_recursion_scope&) = delete;
    front_recursion_scope& operator=(const front_recursion_scope&) = delete;
    front_recursion_scope(front_recursion_scope&&) = delete;
    front_recursion_scope& operator=(front_recursion_scope&&) = delete;

private:
    query_front* const previous_;
};

} // namespace

const input_next_runtime_api_v1* query_runtime_api(query_front& front,
    const std::uint32_t requested_abi) noexcept {
    if (requested_abi != 1U || !valid_operations(front.operations)) {
        return nullptr;
    }

    if (active_front == &front) {
        return nullptr;
    }
    const front_recursion_scope recursion{front};

    auto* owner = front.owner.load(std::memory_order_acquire);
    if (owner == nullptr) {
        if (InitOnceExecuteOnce(&front.owner_once, &create_owner_once, &front, nullptr) == FALSE) {
            return nullptr;
        }
        owner = front.owner.load(std::memory_order_acquire);
    }
    if (owner == nullptr) {
        return nullptr;
    }

    return front.operations.stable_api(front.context, owner);
}

} // namespace inputnext::runtime_detail
