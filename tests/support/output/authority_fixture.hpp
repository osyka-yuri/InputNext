#pragma once

#include "output/composition/composition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/source_lifetime.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::test::output {

class registry_source final : public source_lifetime {};

class foreground_source final : public inputnext::focus::source {
public:
    explicit foreground_source(inputnext::focus::process_instance process) noexcept
        : process_(process),
          observations_(0U) {}

    [[nodiscard]] inputnext::focus::process_instance process() const noexcept override {
        return process_;
    }

    [[nodiscard]] inputnext::focus::reading observe() noexcept override {
        {
            std::scoped_lock lock{mutex_};
            observations_.fetch_add(1U, std::memory_order_release);
        }
        changed_.notify_all();
        return inputnext::focus::reading{.process = process_,
            .state = inputnext::focus::evidence::foreground};
    }

    [[nodiscard]] std::uint64_t observations() const noexcept {
        return observations_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool wait_for_observations(std::uint64_t expected,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, expected] {
            return observations() >= expected;
        });
    }

private:
    inputnext::focus::process_instance process_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::atomic<std::uint64_t> observations_;
};

[[nodiscard]] inline runtime_id runtime_identity(std::byte first) noexcept {
    runtime_id value{};
    value.bytes[0U] = first;
    return value;
}

class authority_fixture final {
public:
    explicit authority_fixture(std::byte first = std::byte{0x51U},
        inputnext::output::authority_seed seed = {})
        : runtime(runtime_identity(first)),
          registry(runtime),
          process{.pid = 41U, .creation_time_100ns = 12345U},
          owned_focus_source(std::make_unique<foreground_source>(process)),
          focus_source_view(owned_focus_source.get()),
          focus_observer(
              runtime_identity(static_cast<std::byte>(std::to_integer<unsigned int>(first) + 1U)),
              std::move(owned_focus_source)),
          trusted(runtime, registry, focus_observer, seed),
          owner(trusted.public_authority()) {
        auto candidate = std::unique_ptr<source_lifetime>{std::make_unique<registry_source>()};
        auto admitted = registry.admit(candidate);
        if (!admitted.has_value()) {
            throw std::runtime_error{"output fixture could not admit registry source"};
        }
        connection = admitted.value();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto observed = focus_observer.published();
            if (observed.open && observed.state == inputnext::focus::evidence::foreground) {
                return;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error{"output fixture did not publish foreground evidence"};
    }

    authority_fixture(const authority_fixture&) = delete;
    authority_fixture& operator=(const authority_fixture&) = delete;
    authority_fixture(authority_fixture&&) = delete;
    authority_fixture& operator=(authority_fixture&&) = delete;
    ~authority_fixture() = default;

    [[nodiscard]] foreground_source& focus_source() const noexcept { return *focus_source_view; }

    runtime_id runtime;
    device_registry registry;
    inputnext::focus::process_instance process;
    std::unique_ptr<foreground_source> owned_focus_source;
    foreground_source* focus_source_view{};
    inputnext::focus::observer focus_observer;
    inputnext::output::detail::composition trusted;
    inputnext::output::authority& owner;
    inputnext::admitted_connection connection;
};

[[nodiscard]] inline inputnext::output::detail::internal_producer make_internal_producer(
    inputnext::output::detail::composition& trusted,
    const inputnext::output::internal_context_options& options) {
    auto created = trusted.make_producer(options);
    if (!created.has_value()) {
        throw std::runtime_error{"output fixture could not issue internal producer"};
    }
    return std::move(*created);
}

struct rumble_intent final {
    inputnext::output::channel_mask channels{};
    std::uint64_t sequence{};
    std::chrono::milliseconds ttl{};
    float low = 0.0F;
    float high = 0.0F;
};

[[nodiscard]] inline inputnext::output::submit_request rumble_request(
    rumble_intent intent) noexcept {
    inputnext::output::submit_request request;
    request.selected_channels = intent.channels;
    request.sequence = intent.sequence;
    request.ttl = intent.ttl;
    request.values.rumble =
        inputnext::output::rumble_value{.low_frequency = intent.low, .high_frequency = intent.high};
    return request;
}

} // namespace inputnext::test::output
