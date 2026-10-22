#include "output/composition/composition.hpp"

#include "focus/observer_test_access.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/xinput/rumble.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace inputnext;
using namespace inputnext::xinput_test_support;

class output_fixture final {
public:
    output_fixture()
        : focus_observer(focus_state.observer()),
          connection(devices.add()),
          composition(connection.connection.target.runtime, devices.registry(), focus_observer) {}

    explicit output_fixture(focus::observer& authority_focus_value)
        : focus_observer(authority_focus_value),
          connection(devices.add()),
          composition(connection.connection.target.runtime, devices.registry(), focus_observer) {}

    [[nodiscard]] std::unique_ptr<output::detail::xinput_rumble> make_bridge() {
        auto created = composition.make_xinput_rumble();
        if (!created.has_value()) {
            throw std::runtime_error{"could not create XInput output test bridge"};
        }
        return std::move(created.value());
    }

    [[nodiscard]] std::unique_ptr<xinput::service> make_service(
        std::unique_ptr<output::detail::xinput_rumble> bridge) {
        return std::make_unique<xinput::service>(focus_observer,
            metadata,
            std::move(bridge),
            xinput::presentation_seed{});
    }

    [[nodiscard]] xinput::assignment assign(xinput::service& service) const {
        auto assigned = service.assign(connection.connection.target, connection.connection.reader);
        if (!assigned.has_value()) {
            throw std::runtime_error{"could not assign XInput output test connection"};
        }
        return assigned.value();
    }

    [[nodiscard]] output::logical_snapshot snapshot() {
        auto observed = composition.public_authority().read_snapshot(connection.connection.target);
        if (!observed.has_value()) {
            throw std::runtime_error{"could not read XInput output test snapshot"};
        }
        return observed.value();
    }

    registry_fixture devices;
    focus_fixture focus_state;
    focus::observer& focus_observer;
    metadata_source metadata;
    test_connection connection;
    output::detail::composition composition;
};

[[nodiscard]] DWORD
set_state(xinput::service& service, const DWORD position, const XINPUT_VIBRATION& vibration) {
    return service.set_state(xinput::flavor::v1_4, position, &vibration);
}

[[nodiscard]] output::rumble_value normalize(const XINPUT_VIBRATION& vibration) noexcept {
    return output::rumble_value{.low_frequency =
                                    static_cast<float>(vibration.wLeftMotorSpeed) / 65535.0F,
        .high_frequency = static_cast<float>(vibration.wRightMotorSpeed) / 65535.0F};
}

class renewal_clock final {
public:
    using clock = std::chrono::steady_clock;
    using duration = clock::duration;
    using time_point = clock::time_point;
    using rep = duration::rep;

    [[nodiscard]] static time_point read(const void* context) noexcept {
        return static_cast<const renewal_clock*>(context)->now();
    }

    [[nodiscard]] time_point now() const noexcept {
        static thread_local int thread_token{};
        const void* const current_thread = &thread_token;
        const void* expected_thread{};
        static_cast<void>(worker_thread_.compare_exchange_strong(expected_thread,
            current_thread,
            std::memory_order_acq_rel,
            std::memory_order_acquire));

        if (worker_thread_.load(std::memory_order_acquire) == current_thread) {
            const auto worker_call = worker_calls_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            if (worker_call == 3U) {
                renewal_waiting_.store(true, std::memory_order_release);
                renewal_waiting_.notify_all();
                renewal_released_.wait(false, std::memory_order_acquire);
            }
        }

        return time_point{duration{ticks_.load(std::memory_order_relaxed)}};
    }

    void advance(const duration amount) noexcept {
        ticks_.fetch_add(amount.count(), std::memory_order_relaxed);
    }

    [[nodiscard]] bool wait_until_renewal_waits(
        const std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) const noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (renewal_waiting_.load(std::memory_order_acquire)) {
                return true;
            }
            std::this_thread::yield();
        }
        return renewal_waiting_.load(std::memory_order_acquire);
    }

    void release_renewal() noexcept {
        renewal_released_.store(true, std::memory_order_release);
        renewal_released_.notify_all();
    }

private:
    mutable std::atomic<const void*> worker_thread_;
    mutable std::atomic<std::uint64_t> worker_calls_;
    mutable std::atomic<bool> renewal_waiting_;
    mutable std::atomic<bool> renewal_released_;
    std::atomic<rep> ticks_;
};

class release_renewal_on_exit final {
public:
    explicit release_renewal_on_exit(renewal_clock& clock) noexcept : clock_(clock) {}

    ~release_renewal_on_exit() { clock_.release_renewal(); }

    release_renewal_on_exit(const release_renewal_on_exit&) = delete;
    release_renewal_on_exit& operator=(const release_renewal_on_exit&) = delete;
    release_renewal_on_exit(release_renewal_on_exit&&) = delete;
    release_renewal_on_exit& operator=(release_renewal_on_exit&&) = delete;

private:
    renewal_clock& clock_;
};

class release_source_on_exit final {
public:
    explicit release_source_on_exit(test::focus::controlled_source& source) noexcept
        : source_(source) {}

    ~release_source_on_exit() { source_.release(); }

    release_source_on_exit(const release_source_on_exit&) = delete;
    release_source_on_exit& operator=(const release_source_on_exit&) = delete;
    release_source_on_exit(release_source_on_exit&&) = delete;
    release_source_on_exit& operator=(release_source_on_exit&&) = delete;

private:
    test::focus::controlled_source& source_;
};

[[nodiscard]] focus::snapshot wait_for_observations(focus::observer const& observer,
    const std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto observed = observer.published();
        if (observed.completed_observations >= expected) {
            return observed;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error{"focus observer did not complete the requested observation"};
}

struct internal_capacity final {
    std::vector<output::detail::internal_producer> producers;
    std::vector<output::internal_context> contexts;
};

[[nodiscard]] internal_capacity fill_internal_capacity(output_fixture& fixture) {
    internal_capacity filled;
    filled.producers.reserve(32U);
    filled.contexts.reserve(32U);
    for (std::uint64_t index = 1U; index <= 32U; ++index) {
        const output::internal_context_options options{
            .owner = output::internal_owner_key{1000U + index},
            .priority = output::internal_priority::generic_profile,
            .ttl = output::ttl_permission::finite_or_indefinite,
            .foreground = output::foreground_policy::background,
        };
        auto created = fixture.composition.make_producer(options);
        if (!created.has_value()) {
            throw std::runtime_error{"could not fill XInput output authority capacity"};
        }
        filled.producers.push_back(std::move(created.value()));
        auto context = filled.producers.back().open(fixture.connection.connection.target);
        if (!context.has_value()) {
            throw std::runtime_error{"could not open XInput output capacity filler"};
        }
        filled.contexts.push_back(context.value());
    }
    return filled;
}

[[nodiscard]] bool release_one(internal_capacity& filled) {
    const auto receipt = filled.producers.front().close(filled.contexts.front(),
        filled.contexts.front().identity.id.value);
    return receipt.has_value() && filled.producers.front().acknowledge(receipt.value()).has_value();
}

TEST_CASE("xinput.output_service_recreation_keeps_late_cleanup_isolated",
    "[component][xinput][output][lifecycle]") {
    output_fixture fixture{};

    auto first_bridge = fixture.make_bridge();
    const auto first_identity = first_bridge->service_id();
    auto first = fixture.make_service(std::move(first_bridge));
    const auto first_assignment = fixture.assign(*first);
    const XINPUT_VIBRATION first_vibration{.wLeftMotorSpeed = 11000U, .wRightMotorSpeed = 17000U};
    CHECK(set_state(*first, first_assignment.user_index, first_vibration) == ERROR_SUCCESS);
    const auto first_snapshot = fixture.snapshot();
    REQUIRE(first_snapshot.rumble.has_value());
    const auto first_rumble =
        first_snapshot.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    const auto first_context = first_rumble.provenance.identity;

    auto second_bridge = fixture.make_bridge();
    const auto second_identity = second_bridge->service_id();
    CHECK(first_identity.authority == second_identity.authority);
    CHECK(first_identity.id != second_identity.id);

    const auto second = fixture.make_service(std::move(second_bridge));
    const auto second_assignment = fixture.assign(*second);
    CHECK(second_assignment.target == first_assignment.target);
    const XINPUT_VIBRATION second_vibration{.wLeftMotorSpeed = 47000U, .wRightMotorSpeed = 53000U};
    CHECK(set_state(*second, second_assignment.user_index, second_vibration) == ERROR_SUCCESS);
    const auto second_snapshot = fixture.snapshot();
    REQUIRE(second_snapshot.rumble.has_value());
    const auto second_rumble =
        second_snapshot.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    const auto second_context = second_rumble.provenance.identity;
    CHECK(second_context.internal_owner != first_context.internal_owner);
    CHECK(second_context.id != first_context.id);
    CHECK(second_rumble.value == normalize(second_vibration));

    first.reset();

    const auto after_old_service_cleanup = fixture.snapshot();
    REQUIRE(after_old_service_cleanup.rumble.has_value());
    const auto surviving_rumble =
        after_old_service_cleanup.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(surviving_rumble.provenance.identity == second_context);
    CHECK(surviving_rumble.value == normalize(second_vibration));
    const auto second_status = second->get_rumble_output_status(second_assignment.user_index);
    REQUIRE(second_status.has_value());
    const auto surviving_status = second_status.value_or(xinput::rumble_output_status{});
    CHECK(surviving_status.desired_state_cached);
    CHECK(surviving_status.logical_version == surviving_rumble.provenance.accepted_commit_order);
}

TEST_CASE("xinput.output_prepared_request_survives_fresh_renewal_in_the_same_epoch",
    "[component][xinput][output][concurrency]") {
    renewal_clock clock{};
    const focus::process_instance process{.pid = 468U, .creation_time_100ns = 992211U};
    const std::vector<focus::reading> readings{
        {.process = process, .state = focus::evidence::foreground},
        {.process = process, .state = focus::evidence::foreground},
    };
    auto controlled = std::make_unique<test::focus::controlled_source>(process, readings, 1U);
    auto* const source = controlled.get();
    const auto focus_owner =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x63U),
            std::move(controlled),
            &renewal_clock::read,
            &clock);
    const release_renewal_on_exit release_clock{clock};
    const release_source_on_exit release_source{*source};
    REQUIRE(source->wait_until_blocked());

    source->release_initial_observation();
    const auto initial_publication = wait_for_observations(*focus_owner, 1U);
    REQUIRE(initial_publication.state == focus::evidence::foreground);
    REQUIRE(initial_publication.revision != 0U);

    output_fixture fixture{*focus_owner};
    auto bridge = fixture.composition.make_xinput_rumble();
    REQUIRE(bridge.has_value());
    xinput::service service{*focus_owner,
        fixture.metadata,
        std::move(bridge.value()),
        xinput::presentation_seed{}};
    const auto occupant = fixture.assign(service);
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 12345U, .wRightMotorSpeed = 54321U};
    xinput::detail::service_test_access::prepared_rumble prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare_rumble(service,
                occupant.user_index,
                &vibration,
                prepared) == ERROR_SUCCESS);
    REQUIRE(clock.wait_until_renewal_waits());

    const auto initial_epoch = prepared.epoch;
    CHECK(initial_epoch.revision == initial_publication.revision);
    clock.advance(std::chrono::milliseconds{249});
    clock.release_renewal();

    const auto renewed_publication = wait_for_observations(*focus_owner, 2U);
    REQUIRE(renewed_publication.state == focus::evidence::foreground);
    CHECK(renewed_publication.completed_observations > initial_publication.completed_observations);
    CHECK(renewed_publication.revision == initial_publication.revision);
    CHECK(renewed_publication.observation_started > initial_publication.observation_started);
    {
        const auto current_epoch_guard = focus_owner->lock_presentation();
        const auto renewed_epoch = current_epoch_guard.current_foreground_epoch();
        REQUIRE(renewed_epoch.has_value());
        const auto current_epoch = renewed_epoch.value_or(focus::foreground_epoch{});
        CHECK(current_epoch == initial_epoch);
        CHECK(prepared.epoch == current_epoch);
    }

    CHECK(xinput::detail::service_test_access::commit_rumble(service,
              occupant.user_index,
              prepared) == ERROR_SUCCESS);
    const auto submitted = fixture.snapshot();
    REQUIRE(submitted.rumble.has_value());
    const auto submitted_rumble =
        submitted.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(submitted_rumble.value == normalize(vibration));
    CHECK(submitted_rumble.provenance.foreground.value_or(focus::foreground_epoch{}) ==
          initial_epoch);
}

TEST_CASE("xinput.output_pending_restore_and_newer_set_state_follow_commit_order",
    "[component][xinput][output][concurrency]") {
    const auto verify_order = [](const bool restore_first) {
        output_fixture fixture{};
        const auto service = fixture.make_service(fixture.make_bridge());
        const auto occupant = fixture.assign(*service);
        service->enable(FALSE);
        const XINPUT_VIBRATION cached_vibration{.wLeftMotorSpeed = 0U, .wRightMotorSpeed = 41000U};
        REQUIRE(set_state(*service, occupant.user_index, cached_vibration) == ERROR_SUCCESS);

        auto capacity = fill_internal_capacity(fixture);
        service->enable(TRUE);
        const auto failed_restore = service->get_rumble_output_status(occupant.user_index);
        REQUIRE(failed_restore.has_value());
        const auto failed_status = failed_restore.value_or(xinput::rumble_output_status{});
        CHECK(failed_status.restore_pending);
        CHECK(failed_status.last_restore_error == ERROR_NOT_ENOUGH_MEMORY);
        CHECK((!fixture.snapshot().rumble.has_value()));

        const XINPUT_VIBRATION newer_vibration{.wLeftMotorSpeed = 52000U,
            .wRightMotorSpeed = 7000U};
        xinput::detail::service_test_access::prepared_rumble prepared{};
        REQUIRE(xinput::detail::service_test_access::prepare_rumble(*service,
                    occupant.user_index,
                    &newer_vibration,
                    prepared) == ERROR_SUCCESS);
        REQUIRE(release_one(capacity));

        if (restore_first) {
            service->maintain_output();
            const auto restored = fixture.snapshot();
            REQUIRE(restored.rumble.has_value());
            const auto restored_rumble =
                restored.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
            CHECK(restored_rumble.value == normalize(cached_vibration));
            CHECK(xinput::detail::service_test_access::commit_rumble(*service,
                      occupant.user_index,
                      prepared) == ERROR_SUCCESS);
        } else {
            CHECK(xinput::detail::service_test_access::commit_rumble(*service,
                      occupant.user_index,
                      prepared) == ERROR_SUCCESS);
            service->maintain_output();
        }

        const auto final_snapshot = fixture.snapshot();
        REQUIRE(final_snapshot.rumble.has_value());
        const auto final_rumble =
            final_snapshot.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
        CHECK(final_rumble.value == normalize(newer_vibration));
        const auto final_status = service->get_rumble_output_status(occupant.user_index);
        REQUIRE(final_status.has_value());
        const auto final_status_value = final_status.value_or(xinput::rumble_output_status{});
        CHECK(final_status_value.desired_state_cached);
        CHECK((!final_status_value.restore_pending));
        CHECK(final_status_value.last_restore_error == ERROR_SUCCESS);
        CHECK(final_status_value.request_revision == 2U);
        CHECK(final_status_value.logical_version == final_rumble.provenance.accepted_commit_order);
    };

    SECTION("the newer request commits before a pending restore") {
        verify_order(false);
    }
    SECTION("the pending restore commits before the newer request") {
        verify_order(true);
    }
}

} // namespace
