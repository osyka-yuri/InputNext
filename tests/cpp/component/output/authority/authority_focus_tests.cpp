#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"
#include "output/controlled_focus_source.hpp"
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

[[nodiscard]] bool wait_for_focus(const focus::observer& observer,
    focus::evidence expected,
    std::uint64_t revision_floor = 1U) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        const auto value = observer.published();
        if (value.open && value.state == expected && value.revision >= revision_floor) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class release_focus_source final {
public:
    explicit release_focus_source(inputnext::test::output::controlled_focus_source& source) noexcept
        : source_(source) {}

    ~release_focus_source() { source_.release(); }

    release_focus_source(const release_focus_source&) = delete;
    release_focus_source& operator=(const release_focus_source&) = delete;
    release_focus_source(release_focus_source&&) = delete;
    release_focus_source& operator=(release_focus_source&&) = delete;

private:
    inputnext::test::output::controlled_focus_source& source_;
};

TEST_CASE("output.authority_focus_loss_returns_unbound_owner_in_same_snapshot",
    "[component][output]") {
    const auto runtime = inputnext::test::output::runtime_identity(std::byte{0x62U});
    device_registry registry{runtime};
    auto candidate = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto admitted = registry.admit(candidate);
    REQUIRE(admitted.has_value());

    const focus::process_instance process{.pid = 62U, .creation_time_100ns = 789U};
    auto source = std::make_unique<inputnext::test::output::controlled_focus_source>(process,
        std::vector{
            focus::reading{.process = process, .state = focus::evidence::foreground},
            focus::reading{.process = process, .state = focus::evidence::background},
        });
    auto* source_view = source.get();
    source_view->block_call(2U);
    focus::observer observer{inputnext::test::output::runtime_identity(std::byte{0x63U}),
        std::move(source)};
    const release_focus_source release_on_exit{*source_view};
    REQUIRE(wait_for_focus(observer, focus::evidence::foreground));
    REQUIRE(source_view->wait_until_blocked());
    inputnext::output::detail::composition trusted{runtime, registry, observer};
    auto& owner = trusted.public_authority();
    const auto opened = owner.open(admitted->target);
    REQUIRE(opened.has_value());
    const internal_context_options options{
        .owner = internal_owner_key{91U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto background_producer = inputnext::test::output::make_internal_producer(trusted, options);
    const auto background = background_producer.open(admitted->target);
    REQUIRE(background.has_value());
    const auto background_submit = background_producer.submit(*background,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.2F, .high = 0.3F}));
    REQUIRE(background_submit.has_value());

    source_view->release();
    REQUIRE(wait_for_focus(observer, focus::evidence::background, 2U));
    const auto mixed = owner.read_snapshot(admitted->target);
    REQUIRE(mixed.has_value());
    CHECK(mixed->present_channels() == rumble_mask);
    CHECK(mixed->rumble.value_or(channel_snapshot<rumble_value>{}).value.low_frequency == 0.2F);
    REQUIRE(mixed->rumble.has_value());
    CHECK(mixed->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          background->identity);

    const auto revoked = owner.close(*opened, opened->identity.id.value);
    REQUIRE(revoked.has_value());
    CHECK(revoked->reason == close_reason::foreground_revocation);
    REQUIRE(owner.acknowledge(*revoked).has_value());
}

TEST_CASE("output.authority_regained_foreground_does_not_revive_old_epoch", "[component][output]") {
    const auto runtime = inputnext::test::output::runtime_identity(std::byte{0x64U});
    device_registry registry{runtime};
    auto candidate = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto admitted = registry.admit(candidate);
    REQUIRE(admitted.has_value());

    const focus::process_instance process{.pid = 64U, .creation_time_100ns = 790U};
    auto source = std::make_unique<inputnext::test::output::controlled_focus_source>(process,
        std::vector{
            focus::reading{.process = process, .state = focus::evidence::foreground},
            focus::reading{.process = process, .state = focus::evidence::background},
            focus::reading{.process = process, .state = focus::evidence::foreground},
        });
    auto* const source_view = source.get();
    source_view->block_call(2U);
    focus::observer observer{inputnext::test::output::runtime_identity(std::byte{0x65U}),
        std::move(source)};
    const release_focus_source release_on_exit{*source_view};
    REQUIRE(wait_for_focus(observer, focus::evidence::foreground));
    REQUIRE(source_view->wait_until_blocked());

    inputnext::output::detail::composition trusted{runtime, registry, observer};
    auto& owner = trusted.public_authority();
    const auto opened = owner.open(admitted->target);
    REQUIRE(opened.has_value());
    const internal_context_options options{
        .owner = internal_owner_key{92U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto background_producer = inputnext::test::output::make_internal_producer(trusted, options);
    const auto background = background_producer.open(admitted->target);
    REQUIRE(background.has_value());
    const auto accepted = background_producer.submit(*background,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.25F, .high = 0.5F}));
    REQUIRE(accepted.has_value());

    const auto opening_info = owner.inspect(*opened);
    REQUIRE(opening_info.has_value());
    REQUIRE(opening_info->foreground.has_value());
    const auto opening_epoch = opening_info->foreground.value_or(focus::foreground_epoch{});

    source_view->release();
    REQUIRE(wait_for_focus(observer, focus::evidence::foreground, opening_epoch.revision + 1U));
    CHECK(observer.published().completed_observations >= 3U);

    const auto revived_mutation = owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.75F, .high = 0.9F}));
    CHECK((!revived_mutation.has_value() && revived_mutation.error() == output_error::revoked));

    const auto mixed = owner.read_snapshot(admitted->target);
    REQUIRE(mixed.has_value());
    REQUIRE(mixed->rumble.has_value());
    CHECK(mixed->rumble.value_or(channel_snapshot<rumble_value>{}).value.low_frequency == 0.25F);
    CHECK(mixed->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          background->identity);

    const auto revoked = owner.close(*opened, opened->identity.id.value);
    REQUIRE(revoked.has_value());
    CHECK(revoked->reason == close_reason::foreground_revocation);
    REQUIRE(owner.acknowledge(*revoked).has_value());
}

TEST_CASE("output.foreground_revocation_preserves_background_at_logical_headroom",
    "[component][output]") {
    const auto runtime = inputnext::test::output::runtime_identity(std::byte{0x68U});
    device_registry registry{runtime};
    auto candidate = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto admitted = registry.admit(candidate);
    REQUIRE(admitted.has_value());

    const focus::process_instance process{.pid = 68U, .creation_time_100ns = 793U};
    auto source = std::make_unique<inputnext::test::output::controlled_focus_source>(process,
        std::vector{
            focus::reading{.process = process, .state = focus::evidence::foreground},
            focus::reading{.process = process, .state = focus::evidence::background},
            focus::reading{.process = process, .state = focus::evidence::foreground},
        });
    auto* const source_view = source.get();
    source_view->block_call(2U);
    focus::observer observer{inputnext::test::output::runtime_identity(std::byte{0x69U}),
        std::move(source)};
    const release_focus_source release_on_exit{*source_view};
    REQUIRE(wait_for_focus(observer, focus::evidence::foreground));
    REQUIRE(source_view->wait_until_blocked());

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    inputnext::output::detail::composition trusted{runtime,
        registry,
        observer,
        authority_seed{.next_context_id = 1U, .logical_version = maximum - 7U}};
    auto& owner = trusted.public_authority();
    const auto foreground = owner.open(admitted->target);
    REQUIRE(foreground.has_value());

    const internal_context_options background_options{
        .owner = internal_owner_key{93U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_or_indefinite,
        .foreground = foreground_policy::background,
    };
    auto background_producer =
        inputnext::test::output::make_internal_producer(trusted, background_options);
    const auto background = background_producer.open(admitted->target);
    REQUIRE(background.has_value());

    const auto foreground_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.75F, .high = 0.85F});
    const auto foreground_submit = owner.submit(*foreground, foreground_request);
    REQUIRE(foreground_submit.has_value());
    CHECK(foreground_submit->logical_version == maximum - 4U);

    const auto background_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 0ms, .low = 0.25F, .high = 0.35F});
    const auto background_submit = background_producer.submit(*background, background_request);
    REQUIRE(background_submit.has_value());
    CHECK(background_submit->logical_version == maximum - 3U);

    auto blocked_request = foreground_request;
    blocked_request.sequence = 2U;
    blocked_request.values.rumble = rumble_value{.low_frequency = 0.9F, .high_frequency = 0.95F};
    const auto blocked = owner.submit(*foreground, blocked_request);
    CHECK((!blocked.has_value() && blocked.error() == output_error::logical_version_exhausted));
    const auto unchanged = owner.inspect(*foreground);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->accepted_sequence == 1U);
    CHECK(unchanged->revisions[2U] == 1U);

    const auto opening_info = owner.inspect(*foreground);
    REQUIRE(opening_info.has_value());
    REQUIRE(opening_info->foreground.has_value());
    const auto opening_epoch = opening_info->foreground.value_or(focus::foreground_epoch{});
    source_view->release();
    REQUIRE(wait_for_focus(observer, focus::evidence::foreground, opening_epoch.revision + 1U));
    CHECK(observer.published().completed_observations >= 3U);

    const auto mixed = owner.read_snapshot(admitted->target);
    REQUIRE(mixed.has_value());
    CHECK(mixed->logical_version == maximum - 2U);
    REQUIRE(mixed->rumble.has_value());
    const auto background_winner = mixed->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(background_winner.value == background_request.values.rumble);
    CHECK(background_winner.provenance.identity == background->identity);
    CHECK(background_winner.provenance.accepted_sequence == 1U);
    CHECK(background_winner.provenance.revision == 1U);

    const auto revoked = owner.close(*foreground, foreground->identity.id.value);
    REQUIRE(revoked.has_value());
    CHECK(revoked->reason == close_reason::foreground_revocation);
    CHECK(revoked->logical_revocation_version == maximum - 2U);
    const auto retry = owner.close(*foreground, foreground->identity.id.value);
    REQUIRE(retry.has_value());
    CHECK(*retry == *revoked);
    REQUIRE(owner.acknowledge(*revoked).has_value());

    const auto after_ack = owner.read_snapshot(admitted->target);
    REQUIRE(after_ack.has_value());
    CHECK(after_ack->logical_version == maximum - 2U);
    CHECK(after_ack->rumble == mixed->rumble);

    const auto background_close =
        background_producer.close(*background, background->identity.id.value);
    REQUIRE(background_close.has_value());
    CHECK(background_close->reason == close_reason::explicit_close);
    CHECK(background_close->logical_revocation_version == maximum - 1U);
    REQUIRE(background_producer.acknowledge(*background_close).has_value());
}

TEST_CASE("output.unstable_snapshot_uses_one_unbound_fallback_and_retires_exact_target",
    "[component][output]") {
    authority_fixture fixture{};
    const auto admit_source = [&fixture] {
        auto candidate = std::unique_ptr<source_lifetime>{
            std::make_unique<inputnext::test::output::registry_source>()};
        return fixture.registry.admit(candidate);
    };
    const auto fallback_connection = admit_source();
    const auto retired_connection = admit_source();
    const auto unrelated_connection = admit_source();
    REQUIRE(fallback_connection.has_value());
    REQUIRE(retired_connection.has_value());
    REQUIRE(unrelated_connection.has_value());

    const auto fallback_bound = fixture.owner.open(fallback_connection->target);
    const auto retired_bound = fixture.owner.open(retired_connection->target);
    const auto unrelated = fixture.owner.open(unrelated_connection->target);
    REQUIRE(fallback_bound.has_value());
    REQUIRE(retired_bound.has_value());
    REQUIRE(unrelated.has_value());

    const internal_context_options fallback_options{
        .owner = internal_owner_key{101U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    const internal_context_options retired_options{
        .owner = internal_owner_key{102U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto fallback_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, fallback_options);
    auto retired_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, retired_options);
    const auto fallback_background = fallback_producer.open(fallback_connection->target);
    const auto retired_background = retired_producer.open(retired_connection->target);
    REQUIRE(fallback_background.has_value());
    REQUIRE(retired_background.has_value());
    const auto fallback_submit = fallback_producer.submit(*fallback_background,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.3F, .high = 0.4F}));
    const auto retired_submit = retired_producer.submit(*retired_background,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.6F, .high = 0.7F}));
    REQUIRE(fallback_submit.has_value());
    REQUIRE(retired_submit.has_value());

    const auto fallback_info = fixture.owner.inspect(*fallback_bound);
    const auto retired_info = fixture.owner.inspect(*retired_bound);
    REQUIRE(fallback_info.has_value());
    REQUIRE(retired_info.has_value());
    REQUIRE(fallback_info->foreground.has_value());
    REQUIRE(retired_info->foreground.has_value());
    auto fallback_after = fallback_info->foreground.value_or(focus::foreground_epoch{});
    ++fallback_after.revision;
    auto retired_after = retired_info->foreground.value_or(focus::foreground_epoch{});
    ++retired_after.revision;
    const auto evaluation_time = std::chrono::steady_clock::now();

    auto gate = inputnext::output::detail::authority_test_access::lock_gate(fixture.owner);
    auto fallback_first_pin = fixture.registry.capture(fallback_connection->target);
    REQUIRE(fallback_first_pin.has_value());
    const auto fallback_snapshot =
        inputnext::output::detail::authority_test_access::finish_snapshot_locked(fixture.owner,
            fallback_connection->target,
            evaluation_time,
            fallback_info->foreground,
            fallback_after,
            std::move(*fallback_first_pin));
    REQUIRE(fallback_snapshot.has_value());
    CHECK(fallback_snapshot->present_channels() == rumble_mask);
    CHECK(
        fallback_snapshot->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
        fallback_background->identity);
    gate.unlock();

    auto retired_gate = inputnext::output::detail::authority_test_access::lock_gate(fixture.owner);
    auto retired_first_pin = fixture.registry.capture(retired_connection->target);
    REQUIRE(retired_first_pin.has_value());
    REQUIRE(fixture.registry.retire(retired_connection->target).has_value());
    const auto retired_snapshot =
        inputnext::output::detail::authority_test_access::finish_snapshot_locked(fixture.owner,
            retired_connection->target,
            evaluation_time,
            retired_info->foreground,
            retired_after,
            std::move(*retired_first_pin));
    CHECK(
        (!retired_snapshot.has_value() && retired_snapshot.error() == output_error::not_connected));
    retired_gate.unlock();

    const auto retired_inspection = fixture.owner.inspect(*retired_bound);
    CHECK((!retired_inspection.has_value() &&
           retired_inspection.error() == output_error::stale_context));
    CHECK(fixture.owner.inspect(*unrelated).has_value());
}

} // namespace
