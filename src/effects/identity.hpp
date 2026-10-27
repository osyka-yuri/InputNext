#pragma once

#include "effects.hpp"

#include <atomic>
#include <cstdint>

namespace inputnext::effects {

struct identity_seeds final {
    std::uint64_t next_context{1U};
    std::uint64_t next_publication{1U};
    std::uint64_t next_capability{1U};
};

class identity_source final {
public:
    constexpr explicit identity_source(identity_seeds seeds = {}) noexcept
        : next_context_(seeds.next_context),
          next_publication_(seeds.next_publication),
          next_capability_(seeds.next_capability) {}

    identity_source(const identity_source&) = delete;
    identity_source& operator=(const identity_source&) = delete;
    identity_source(identity_source&&) = delete;
    identity_source& operator=(identity_source&&) = delete;
    ~identity_source() = default;

    [[nodiscard]] result<std::uint64_t> take_context() noexcept;
    [[nodiscard]] result<publication_revision> take_publication() noexcept;
    [[nodiscard]] result<std::uint64_t> take_capability() noexcept;

private:
    std::atomic<std::uint64_t> next_context_;
    std::atomic<std::uint64_t> next_publication_;
    std::atomic<std::uint64_t> next_capability_;
};

} // namespace inputnext::effects
