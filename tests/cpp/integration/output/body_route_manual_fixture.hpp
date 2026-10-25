#pragma once

#include "cpp/component/output/execution_engine_scope.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution/engine.hpp"

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace inputnext::test::output {

class manual_body_route_fixture final {
public:
    explicit manual_body_route_fixture(const std::atomic<std::int64_t>* clock_ticks = nullptr,
        const std::chrono::milliseconds body_ttl = std::chrono::seconds{5},
        const inputnext::output::channel_mask channels = inputnext::output::supported_channel_mask)
        : native_state(std::make_shared<controlled_native_state>()) {
        if (clock_ticks != nullptr) {
            inputnext::output::detail::authority_test_access::use_clock(authority.owner,
                *clock_ticks);
        }

        const auto attached = authority.trusted.audio_routes().attach(authority.connection.target,
            inputnext::output::body_route_support::hd,
            true);
        if (!attached.has_value()) {
            throw std::runtime_error{"manual route fixture could not attach audio route"};
        }
        audio = *attached;

        const auto opened = authority.owner.open(authority.connection.target);
        if (!opened.has_value()) {
            throw std::runtime_error{"manual route fixture could not open context"};
        }
        context = *opened;

        const auto acquired = authority.owner.submit_body(context,
            inputnext::output::body_request{.audio = audio,
                .sequence = 1U,
                .ttl = body_ttl,
                .preference = inputnext::output::body_route_preference::prefer_hd,
                .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}});
        if (!acquired.has_value()) {
            throw std::runtime_error{"manual route fixture could not acquire body lease"};
        }
        body = *acquired;

        engine = std::make_unique<execution_engine_scope>(authority.owner);
        std::unique_ptr<inputnext::output::native_sink> candidate =
            std::make_unique<controlled_native_sink>(native_state);
        const auto registered = engine->register_backend(authority.connection.target,
            candidate,
            channels,
            std::nullopt,
            inputnext::output::body_route_support::hd);
        if (!registered.has_value()) {
            throw std::runtime_error{"manual route fixture could not register backend"};
        }
        backend = *registered;

        const auto selected_lane = engine->engine().lane_for(backend);
        if (!selected_lane.has_value()) {
            throw std::runtime_error{"manual route fixture could not find backend lane"};
        }
        lane = *selected_lane;
    }

    ~manual_body_route_fixture() noexcept = default;

    manual_body_route_fixture(const manual_body_route_fixture&) = delete;
    manual_body_route_fixture& operator=(const manual_body_route_fixture&) = delete;
    manual_body_route_fixture(manual_body_route_fixture&&) = delete;
    manual_body_route_fixture& operator=(manual_body_route_fixture&&) = delete;

    [[nodiscard]] inputnext::output::result<inputnext::output::logical_snapshot> snapshot() {
        return authority.owner.read_snapshot(authority.connection.target);
    }

    [[nodiscard]] inputnext::output::execution_result<void> publish(
        const inputnext::output::logical_snapshot& current) const noexcept {
        return engine->engine().publish(backend, current);
    }

    [[nodiscard]] inputnext::output::execution_result<bool> qualifies(
        const inputnext::output::body_route_ack& acknowledgement) const noexcept {
        return engine->engine().qualifies_route(acknowledgement);
    }

    [[nodiscard]] inputnext::output::execution_result<inputnext::output::backend_progress>
    progress() const noexcept {
        return engine->engine().progress(backend);
    }

    [[nodiscard]] inputnext::output::body_route_ack accepted_ack() const {
        const auto current = progress();
        if (!current.has_value() || !current->acknowledged_body_route.has_value()) {
            throw std::runtime_error{"manual route fixture has no accepted body route"};
        }
        return *current->acknowledged_body_route;
    }

    [[nodiscard]] bool drive() const noexcept { return engine->engine().drive_one(lane); }

    [[nodiscard]] inputnext::output::result<inputnext::output::mutation_receipt> submit(
        const inputnext::output::public_context& owner,
        const inputnext::output::submit_request& request) {
        return authority.owner.submit(owner, request);
    }

    [[nodiscard]] inputnext::output::result<void> clear_body(const std::uint64_t sequence) {
        return authority.owner.clear_body(context,
            inputnext::output::body_clear_request{.lease = body.identity.lease,
                .audio = audio,
                .sequence = sequence});
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::mutation_receipt> clear_channel(
        const inputnext::output::public_context& owner,
        const inputnext::output::channel_mask selected_channels,
        const std::uint64_t sequence,
        const inputnext::output::channel_revisions revisions) {
        return authority.owner.clear(owner,
            inputnext::output::clear_request{.selected_channels = selected_channels,
                .sequence = sequence,
                .expected_revisions = revisions});
    }

    inputnext::test::output::authority_fixture authority;
    const std::shared_ptr<controlled_native_state> native_state;
    std::unique_ptr<execution_engine_scope> engine;
    inputnext::output::public_context context{};
    inputnext::output::audio_instance audio{};
    inputnext::output::body_receipt body{};
    inputnext::output::backend_handle backend{};
    inputnext::output::detail::lane_token lane{};
};

} // namespace inputnext::test::output
