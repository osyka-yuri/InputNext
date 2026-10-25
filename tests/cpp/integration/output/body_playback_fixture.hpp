#pragma once

#include "cpp/component/output/execution_engine_scope.hpp"
#include "output/authority_fixture.hpp"
#include "output/controlled_native_sink.hpp"

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace inputnext::test::output {

class body_playback_fixture final {
public:
    body_playback_fixture() : native_state(std::make_shared<controlled_native_state>()) {
        const auto attached = authority.trusted.audio_routes().attach(authority.connection.target,
            inputnext::output::body_route_support::hd,
            true);
        if (!attached.has_value()) {
            throw std::runtime_error{"playback fixture could not attach audio route"};
        }
        audio = *attached;

        const auto opened = authority.owner.open(authority.connection.target);
        if (!opened.has_value()) {
            throw std::runtime_error{"playback fixture could not open context"};
        }
        context = *opened;

        engine = std::make_unique<execution_engine_scope>(authority.owner);
        std::unique_ptr<inputnext::output::native_sink> candidate =
            std::make_unique<controlled_native_sink>(native_state);
        const auto registered = engine->register_backend(authority.connection.target,
            candidate,
            inputnext::output::rumble_mask,
            std::nullopt,
            inputnext::output::body_route_support::hd);
        if (!registered.has_value()) {
            throw std::runtime_error{"playback fixture could not register backend"};
        }
        backend = *registered;

        const auto selected_lane = engine->engine().lane_for(backend);
        if (!selected_lane.has_value()) {
            throw std::runtime_error{"playback fixture could not find backend lane"};
        }
        lane = *selected_lane;
    }

    body_playback_fixture(const body_playback_fixture&) = delete;
    body_playback_fixture& operator=(const body_playback_fixture&) = delete;
    body_playback_fixture(body_playback_fixture&&) = delete;
    body_playback_fixture& operator=(body_playback_fixture&&) = delete;
    ~body_playback_fixture() = default;

    [[nodiscard]] inputnext::output::body_request request(
        const std::uint64_t sequence) const noexcept {
        return inputnext::output::body_request{.lease = body.identity.lease,
            .audio = audio,
            .sequence = sequence,
            .ttl = std::chrono::seconds{5},
            .preference = inputnext::output::body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.3F, .high_frequency = 0.5F}};
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::prepared_body_pcm>
    prepare(const std::span<const float> samples, const float gain = 1.0F) const {
        return authority.owner.pcm_preparer().prepare(inputnext::output::body_pcm_input{
            .samples = samples,
            .frame_count = samples.size(),
            .channels = 1U,
            .sample_rate = inputnext::output::body_pcm_sample_rate,
            .gain = gain,
        });
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::body_playback_receipt>
    replace(inputnext::output::prepared_body_pcm replacement, const std::uint64_t sequence = 1U) {
        auto submitted = authority.owner.submit_body(context,
            request(sequence),
            inputnext::output::body_playback_update{
                .command = inputnext::output::body_pcm_command::replace,
                .replacement = std::move(replacement),
            });
        if (submitted.has_value()) {
            body = submitted->body;
        }
        return submitted;
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::body_playback_receipt> keep(
        const std::uint64_t sequence) {
        auto submitted = authority.owner.submit_body(context,
            request(sequence),
            inputnext::output::body_playback_update{});
        if (submitted.has_value()) {
            body = submitted->body;
        }
        return submitted;
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::body_playback_receipt> stop(
        const std::uint64_t sequence) {
        auto submitted = authority.owner.submit_body(context,
            request(sequence),
            inputnext::output::body_playback_update{
                .command = inputnext::output::body_pcm_command::stop,
                .replacement = {},
            });
        if (submitted.has_value()) {
            body = submitted->body;
        }
        return submitted;
    }

    [[nodiscard]] bool publish_and_drive() noexcept {
        const auto snapshot = authority.owner.read_snapshot(authority.connection.target);
        if (!snapshot.has_value()) {
            return false;
        }
        const auto published = engine->engine().publish(backend, *snapshot);
        return published.has_value() && engine->engine().drive_one(lane);
    }

    [[nodiscard]] std::optional<inputnext::output::body_route_ack>
    acknowledgement() const noexcept {
        const auto current = engine->engine().progress(backend);
        if (!current.has_value()) {
            return std::nullopt;
        }
        return current->acknowledged_body_route;
    }

    [[nodiscard]] inputnext::output::execution_result<
        std::optional<inputnext::output::body_render_permit>>
    admit(inputnext::output::body_playback_identity identity,
        const std::size_t frames = 1U) const noexcept {
        const auto accepted = acknowledgement();
        if (!accepted.has_value()) {
            return std::unexpected{inputnext::output::execution_error::pending};
        }
        return engine->engine().admit_body_render(*accepted, identity, frames);
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

[[nodiscard]] inline float shaped_sample(const float input) noexcept {
    return input - (input * input * input / 6.0F);
}

} // namespace inputnext::test::output
