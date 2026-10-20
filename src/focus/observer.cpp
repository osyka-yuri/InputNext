#include <inputnext/focus.hpp>

#include "observer_test_access.hpp"
#include <inputnext/device_identity.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace inputnext::focus {
namespace {

using namespace std::chrono_literals;

constexpr auto observation_interval = 4ms;
constexpr auto freshness_budget = 250ms;

[[nodiscard]] std::chrono::steady_clock::time_point production_clock_now(
    [[maybe_unused]] const void* context) noexcept {
    return std::chrono::steady_clock::now();
}

[[nodiscard]] bool valid_process_instance(process_instance value) noexcept {
    return value.pid != 0U && value.creation_time_100ns != 0U;
}

[[nodiscard]] bool known_evidence(evidence value) noexcept {
    switch (value) {
    case evidence::unavailable:
    case evidence::background:
    case evidence::foreground:
        return true;
    }
    return false;
}

[[nodiscard]] bool fresh_at(std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point started) noexcept {
    return now >= started && now - started < freshness_budget;
}

} // namespace

struct observer::implementation final {
    implementation(runtime_id incarnation_value,
        std::unique_ptr<source> source_value,
        observer::clock_callback now_value,
        const void* clock_context_value)
        : incarnation(incarnation_value),
          owned_source(std::move(source_value)),
          process(resolve_process(owned_source)),
          now(now_value),
          clock_context(clock_context_value),
          publication(snapshot{
              .observer_incarnation = incarnation,
              .process = process,
              .revision = 0U,
              .state = evidence::unavailable,
              .observation_started = {},
              .open = true,
              .exhausted = false,
              .completed_observations = 0U,
              .native_code = 0U,
          }),
          worker([this](const std::stop_token& token) { run_worker(token); }) {}

    ~implementation() {
        if (worker.joinable()) {
            worker.request_stop();
            wait_changed.notify_all();
            worker.join();
        }
    }

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    [[nodiscard]] static process_instance resolve_process(const std::unique_ptr<source>& provider) {
        if (!provider) {
            throw std::invalid_argument{"focus observer requires an owned source"};
        }
        const auto process_value = provider->process();
        if (!valid_process_instance(process_value)) {
            throw std::invalid_argument{"focus source requires a valid process identity"};
        }
        return process_value;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point current_time() const noexcept {
        return now(clock_context);
    }

    [[nodiscard]] bool advance_revision() noexcept {
        if (publication.revision == std::numeric_limits<std::uint64_t>::max()) {
            publication.exhausted = true;
            publication.state = evidence::unavailable;
            publication.observation_started = {};
            return false;
        }
        ++publication.revision;
        return true;
    }

    void expire_if_needed(std::chrono::steady_clock::time_point sampled_now) noexcept {
        if (publication.state == evidence::unavailable ||
            fresh_at(sampled_now, publication.observation_started)) {
            return;
        }

        if (advance_revision()) {
            publication.state = evidence::unavailable;
            publication.observation_started = {};
        }
    }

    [[nodiscard]] snapshot read_publication() noexcept {
        expire_if_needed(current_time());
        return publication;
    }

    [[nodiscard]] evidence normalize_reading(const reading& reading_value,
        std::chrono::steady_clock::time_point observation_started,
        std::chrono::steady_clock::time_point sampled_now) const noexcept {
        const auto next_state = reading_value.state;
        if (reading_value.process != process || !known_evidence(next_state)) {
            return evidence::unavailable;
        }
        if (next_state != evidence::unavailable && !fresh_at(sampled_now, observation_started)) {
            return evidence::unavailable;
        }
        return next_state;
    }

    void apply_reading(const reading& reading_value,
        std::chrono::steady_clock::time_point observation_started,
        std::chrono::steady_clock::time_point sampled_now) noexcept {
        expire_if_needed(sampled_now);

        if (publication.completed_observations != std::numeric_limits<std::uint64_t>::max()) {
            ++publication.completed_observations;
        }
        publication.native_code = reading_value.native_code;

        if (publication.exhausted) {
            publication.state = evidence::unavailable;
            publication.observation_started = {};
            return;
        }

        const auto next_state = normalize_reading(reading_value, observation_started, sampled_now);
        if (next_state == publication.state) {
            if (next_state != evidence::unavailable) {
                publication.observation_started = observation_started;
            }
            return;
        }
        if (!advance_revision()) {
            return;
        }

        publication.state = next_state;
        publication.observation_started = next_state == evidence::unavailable
                                              ? std::chrono::steady_clock::time_point{}
                                              : observation_started;
    }

    void run_worker(const std::stop_token& token) noexcept {
        try {
            observe_until_stopped(token);
        } catch (...) {
            close_on_failure();
        }
    }

    void close_on_failure() noexcept {
        try {
            std::scoped_lock lock{presentation_mutex};
            publication.open = false;
        } catch (const std::system_error&) {
            std::terminate();
        }
    }

    void observe_until_stopped(const std::stop_token& token) {
        while (!token.stop_requested()) {
            const auto observation_started = current_time();
            const auto reading_value = owned_source->observe();
            {
                std::scoped_lock lock{presentation_mutex};
                const auto sampled_now = current_time();
                if (!publication.open) {
                    return;
                }

                apply_reading(reading_value, observation_started, sampled_now);
            }

            if (token.stop_requested()) {
                return;
            }

            std::unique_lock wait_lock{wait_mutex};
            static_cast<void>(wait_changed.wait_for(wait_lock, observation_interval, [&token] {
                return token.stop_requested();
            }));
        }
    }

    const runtime_id incarnation;
    std::unique_ptr<source> owned_source;
    const process_instance process;
    const observer::clock_callback now;
    const void* const clock_context;
    mutable std::mutex presentation_mutex;
    snapshot publication;
    std::mutex wait_mutex;
    std::condition_variable wait_changed;
    std::jthread worker;
};

observer::observer(runtime_id incarnation, std::unique_ptr<source> owned_source) {
    if (!incarnation.valid()) {
        throw std::invalid_argument{"focus observer requires a valid incarnation"};
    }
    implementation_ = std::make_unique<implementation>(incarnation,
        std::move(owned_source),
        &production_clock_now,
        nullptr);
}

observer::observer(runtime_id incarnation,
    std::unique_ptr<source> owned_source,
    clock_callback now,
    const void* clock_context) {
    if (!incarnation.valid()) {
        throw std::invalid_argument{"focus observer requires a valid incarnation"};
    }
    if (now == nullptr) {
        throw std::invalid_argument{"focus observer requires a clock callback"};
    }
    implementation_ =
        std::make_unique<implementation>(incarnation, std::move(owned_source), now, clock_context);
}

observer::~observer() {
    stop();
}

observer::presentation_guard observer::lock_presentation() const {
    auto& state = *implementation_;
    std::unique_lock lock{state.presentation_mutex};
    return presentation_guard{std::move(lock), &state};
}

snapshot observer::published() const {
    auto& state = *implementation_;
    std::scoped_lock lock{state.presentation_mutex};
    return state.read_publication();
}

void observer::stop() {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.presentation_mutex};
        state.publication.open = false;
    }
    state.worker.request_stop();
    state.wait_changed.notify_all();
    if (state.worker.joinable()) {
        state.worker.join();
    }
}

observer::presentation_guard::presentation_guard(std::unique_lock<std::mutex>&& lock,
    implementation* state) noexcept
    : lock_(std::move(lock)),
      implementation_(state) {
}

observer::presentation_guard::presentation_guard(presentation_guard&& other) noexcept
    : lock_(std::move(other.lock_)),
      implementation_(std::exchange(other.implementation_, nullptr)) {
}

observer::presentation_guard& observer::presentation_guard::operator=(
    presentation_guard&& other) noexcept {
    if (this != &other) {
        lock_ = std::move(other.lock_);
        implementation_ = std::exchange(other.implementation_, nullptr);
    }
    return *this;
}

snapshot observer::presentation_guard::published() const noexcept {
    return implementation_ != nullptr ? implementation_->read_publication() : snapshot{};
}

bool observer::presentation_guard::matches_foreground(const snapshot& captured) const noexcept {
    if (implementation_ == nullptr) {
        return false;
    }

    const auto now = implementation_->current_time();
    implementation_->expire_if_needed(now);
    const auto& current = implementation_->publication;
    if (!captured.open || captured.exhausted || captured.state != evidence::foreground ||
        captured.revision == 0U || !fresh_at(now, captured.observation_started) || !current.open ||
        current.exhausted || current.state != evidence::foreground || current.revision == 0U ||
        !fresh_at(now, current.observation_started)) {
        return false;
    }

    return captured.observer_incarnation == current.observer_incarnation &&
           captured.process == current.process && captured.revision == current.revision;
}

std::unique_ptr<observer> detail::observer_test_access::make_with_clock(runtime_id incarnation,
    std::unique_ptr<source> owned_source,
    clock_callback now,
    const void* clock_context) {
    return std::unique_ptr<observer>{
        new observer{incarnation, std::move(owned_source), now, clock_context}};
}

void detail::observer_test_access::seed_counters(observer& target, const snapshot& seed) noexcept {
    auto& state = *target.implementation_;
    std::scoped_lock lock{state.presentation_mutex};
    state.publication.revision = seed.revision;
    state.publication.completed_observations = seed.completed_observations;
}

} // namespace inputnext::focus
