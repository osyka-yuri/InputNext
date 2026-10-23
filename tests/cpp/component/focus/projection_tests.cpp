#include <inputnext/focus.hpp>

#include "focus/observer_test_access.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

class source_release final {
public:
    explicit source_release(inputnext::test::focus::controlled_source& source) noexcept
        : source_(source) {}

    ~source_release() { source_.release(); }

    source_release(const source_release&) = delete;
    source_release& operator=(const source_release&) = delete;
    source_release(source_release&&) = delete;
    source_release& operator=(source_release&&) = delete;

private:
    inputnext::test::focus::controlled_source& source_;
};

} // namespace

TEST_CASE("focus.expiry_reads_preserve_stored_evidence", "[component][focus][freshness]") {
    using namespace std::chrono_literals;
    using namespace inputnext;

    test::focus::manual_clock clock;
    const focus::process_instance process{.pid = 83U, .creation_time_100ns = 1012U};
    runtime_id incarnation{};
    incarnation.bytes[0U] = std::byte{83U};
    auto owned_source = std::make_unique<test::focus::controlled_source>(process,
        std::vector{focus::reading{.process = process, .state = focus::evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value = focus::detail::observer_test_access::make_with_clock(incarnation,
        std::move(owned_source),
        &test::focus::manual_clock::read,
        &clock);
    const source_release release{*source};
    REQUIRE(source->wait_until_blocked());
    const auto before = focus::detail::observer_test_access::stored_snapshot(*value);
    REQUIRE(before.state == focus::evidence::foreground);
    REQUIRE(before.revision == 1U);

    clock.advance(250ms);
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        const auto projected = value->published();
        CHECK(projected.state == focus::evidence::unavailable);
        CHECK(projected.revision == before.revision + 1U);
        CHECK(projected.observation_started == std::chrono::steady_clock::time_point{});
    }
    {
        const auto guard = value->lock_presentation();
        CHECK(guard.published().state == focus::evidence::unavailable);
        CHECK(guard.published().revision == before.revision + 1U);
        CHECK(!guard.capture_foreground().has_value());
        CHECK(!guard.matches_foreground(before));
        CHECK(!guard.current_foreground_epoch().has_value());
    }

    const auto after = focus::detail::observer_test_access::stored_snapshot(*value);
    CHECK(after.observer_incarnation == before.observer_incarnation);
    CHECK(after.process == before.process);
    CHECK(after.revision == before.revision);
    CHECK(after.state == before.state);
    CHECK(after.observation_started == before.observation_started);
    CHECK(after.open == before.open);
    CHECK(after.exhausted == before.exhausted);
    CHECK(after.completed_observations == before.completed_observations);
    CHECK(after.native_code == before.native_code);
}
