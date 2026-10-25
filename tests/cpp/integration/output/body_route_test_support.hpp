#pragma once

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include <atomic>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::test::output {

class body_route_fixture final {
public:
    explicit body_route_fixture(
        const std::optional<inputnext::output::body_route_support> support = std::nullopt,
        const std::atomic<std::int64_t>* clock_ticks = nullptr)
        : backend_support(support.value_or(inputnext::output::body_route_support::classic_only)),
          native_state(std::make_shared<controlled_native_state>()) {
        if (clock_ticks != nullptr) {
            inputnext::output::detail::authority_test_access::use_clock(authority.owner,
                *clock_ticks);
        }
        auto created = inputnext::output::executor::create(authority.owner);
        if (!created.has_value()) {
            throw std::runtime_error{"output route fixture could not create executor"};
        }
        running = std::move(*created);

        std::unique_ptr<inputnext::output::native_sink> candidate =
            std::make_unique<controlled_native_sink>(native_state);
        auto registered = [&] {
            if (support.has_value()) {
                return running->register_backend(authority.connection.target,
                    candidate,
                    inputnext::output::rumble_mask,
                    backend_support);
            }
            return running->register_backend(authority.connection.target,
                candidate,
                inputnext::output::rumble_mask);
        }();
        if (!registered.has_value()) {
            throw std::runtime_error{"output route fixture could not register backend"};
        }
        backend = *registered;
        if (!wait_for_initial_write()) {
            throw std::runtime_error{"output route fixture initial write did not complete"};
        }
    }

    ~body_route_fixture() noexcept = default;

    body_route_fixture(const body_route_fixture&) = delete;
    body_route_fixture& operator=(const body_route_fixture&) = delete;
    body_route_fixture(body_route_fixture&&) = delete;
    body_route_fixture& operator=(body_route_fixture&&) = delete;

    [[nodiscard]] inputnext::output::result<inputnext::output::public_context> open_context() {
        auto opened = authority.owner.open(authority.connection.target);
        if (opened.has_value()) {
            context = *opened;
        }
        return opened;
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::audio_instance> attach_audio(
        const inputnext::output::body_route_support support =
            inputnext::output::body_route_support::hd,
        const bool ready = true) {
        auto attached =
            authority.trusted.audio_routes().attach(authority.connection.target, support, ready);
        if (attached.has_value()) {
            audio = *attached;
        }
        return attached;
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::body_receipt> acquire_body(
        const std::chrono::milliseconds ttl = std::chrono::seconds{5},
        const inputnext::output::body_route_preference preference =
            inputnext::output::body_route_preference::prefer_hd) {
        if (!context.has_value() || !audio.has_value()) {
            return std::unexpected{inputnext::output::error::invalid_argument};
        }
        inputnext::output::body_request const request{.audio = *audio,
            .sequence = 1U,
            .ttl = ttl,
            .preference = preference,
            .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}};
        auto accepted = authority.owner.submit_body(*context, request);
        if (accepted.has_value()) {
            body = *accepted;
        }
        return accepted;
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::body_receipt> renew_body(
        const std::uint64_t sequence,
        const std::chrono::milliseconds ttl = std::chrono::seconds{5}) {
        if (!context.has_value() || !audio.has_value() || !body.has_value()) {
            return std::unexpected{inputnext::output::error::invalid_argument};
        }
        const auto& identity = body->identity;
        inputnext::output::body_request const request{.lease = identity.lease,
            .audio = *audio,
            .sequence = sequence,
            .ttl = ttl,
            .preference = inputnext::output::body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}};
        auto renewed = authority.owner.submit_body(*context, request);
        if (renewed.has_value()) {
            body = *renewed;
        }
        return renewed;
    }

    [[nodiscard]] inputnext::output::result<void> clear_body(const std::uint64_t sequence) {
        if (!context.has_value() || !audio.has_value() || !body.has_value()) {
            return std::unexpected{inputnext::output::error::invalid_argument};
        }
        return authority.owner.clear_body(*context,
            inputnext::output::body_clear_request{.lease = body->identity.lease,
                .audio = *audio,
                .sequence = sequence});
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::close_receipt> close_context(
        const std::uint64_t request_id = 1U) {
        if (!context.has_value()) {
            return std::unexpected{inputnext::output::error::invalid_argument};
        }
        return authority.owner.close(*context, request_id);
    }

    [[nodiscard]] inputnext::output::result<void> detach_audio() {
        if (!audio.has_value()) {
            return std::unexpected{inputnext::output::error::invalid_argument};
        }
        auto detached = authority.trusted.audio_routes().detach(*audio);
        if (detached.has_value()) {
            audio.reset();
        }
        return detached;
    }

    [[nodiscard]] inputnext::output::execution_result<inputnext::output::backend_progress>
    progress() const {
        return running->progress(backend);
    }

    [[nodiscard]] inputnext::output::result<inputnext::output::logical_snapshot> snapshot() {
        return authority.owner.read_snapshot(authority.connection.target);
    }

    [[nodiscard]] bool wait_for_initial_write() const {
        if (!native_state->wait_for_write_count(1U)) {
            return false;
        }
        return wait_until([this] {
            const auto current = progress();
            return current.has_value() && !current->write_in_flight &&
                   current->last_completed_attempt.has_value() &&
                   current->last_completed_attempt->publication == current->observed_publication;
        });
    }

    [[nodiscard]] bool wait_until(auto&& predicate,
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::yield();
        }
        return predicate();
    }

    authority_fixture authority;
    const inputnext::output::body_route_support backend_support;
    std::shared_ptr<controlled_native_state> native_state;
    std::unique_ptr<inputnext::output::executor> running;
    inputnext::output::backend_handle backend{};
    std::optional<inputnext::output::public_context> context;
    std::optional<inputnext::output::audio_instance> audio;
    std::optional<inputnext::output::body_receipt> body;
};

} // namespace inputnext::test::output
