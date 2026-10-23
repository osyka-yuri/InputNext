#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include <memory>

namespace inputnext::focus::detail {

class observer_test_access final {
public:
    using clock_callback = observer::clock_callback;

    [[nodiscard]] static std::unique_ptr<observer> make_with_clock(runtime_id incarnation,
        std::unique_ptr<source> owned_source,
        clock_callback now,
        const void* clock_context);
    static void seed_counters(observer& target, const snapshot& seed) noexcept;
    [[nodiscard]] static snapshot stored_snapshot(const observer& target);
};

} // namespace inputnext::focus::detail
