#include "identity.hpp"

#include "effects/effects.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <limits>

namespace inputnext::effects {
namespace {

[[nodiscard]] result<std::uint64_t> take_next(std::atomic<std::uint64_t>& counter) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    while (current != 0U) {
        const auto next = current == std::numeric_limits<std::uint64_t>::max() ? 0U : current + 1U;
        if (counter.compare_exchange_weak(current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return std::unexpected{status::capacity_exceeded};
}

} // namespace

result<std::uint64_t> identity_source::take_context() noexcept {
    return take_next(next_context_);
}

result<publication_revision> identity_source::take_publication() noexcept {
    const auto value = take_next(next_publication_);
    if (!value.has_value()) {
        return std::unexpected{value.error()};
    }
    return publication_revision{*value};
}

result<std::uint64_t> identity_source::take_capability() noexcept {
    return take_next(next_capability_);
}

} // namespace inputnext::effects
