#include "bootstrap_owner.hpp"

#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/xinput.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace inputnext::runtime_detail {
namespace {

constinit thread_local bootstrap_owner* active_owner{};

[[nodiscard]] bool terminal(const bootstrap_state state) noexcept {
    return state == bootstrap_state::ready || state == bootstrap_state::degraded;
}

class owner_recursion_scope final {
public:
    explicit owner_recursion_scope(bootstrap_owner& owner) noexcept : previous_{active_owner} {
        active_owner = &owner;
    }

    ~owner_recursion_scope() { active_owner = previous_; }

    owner_recursion_scope(const owner_recursion_scope&) = delete;
    owner_recursion_scope& operator=(const owner_recursion_scope&) = delete;
    owner_recursion_scope(owner_recursion_scope&&) = delete;
    owner_recursion_scope& operator=(owner_recursion_scope&&) = delete;

private:
    bootstrap_owner* const previous_;
};

} // namespace

static_assert(std::is_nothrow_move_assignable_v<gameinput_result<void>>);

bootstrap_owner::bootstrap_owner(std::unique_ptr<focus::source> focus_source,
    const native_session_factory prepare_session,
    void* const borrowed_context)
    : process_(focus_source != nullptr ? focus_source->process() : focus::process_instance{}),
      focus_source_(std::move(focus_source)),
      prepare_session_{prepare_session},
      borrowed_context_{borrowed_context} {
    if (focus_source_ == nullptr || process_.pid == 0U || process_.creation_time_100ns == 0U ||
        prepare_session_ == nullptr) {
        throw std::invalid_argument{"bootstrap owner requires scoped focus and session inputs"};
    }
}

bootstrap_owner::~bootstrap_owner() noexcept {
    if (session_ != nullptr) {
        session_->shutdown();
    }
}

xinput::service* bootstrap_owner::bootstrap() noexcept {
    if (auto* const session = published_session_.load(std::memory_order_acquire);
        session != nullptr) {
        return &session->presentation();
    }
    if (active_owner == this) {
        return nullptr;
    }
    const owner_recursion_scope recursion{*this};

    try {
        for (;;) {
            std::unique_lock lock{bootstrap_gate_};
            auto current = state_.load(std::memory_order_acquire);
            if (terminal(current)) {
                auto* const session = published_session_.load(std::memory_order_acquire);
                return session != nullptr ? &session->presentation() : nullptr;
            }
            if (current == bootstrap_state::preparing) {
                changed_.wait(lock, [this] {
                    return state_.load(std::memory_order_acquire) != bootstrap_state::preparing;
                });
                continue;
            }

            lock.unlock();
            const auto observed_focus = sample_focus();
            lock.lock();

            current = state_.load(std::memory_order_acquire);
            if (terminal(current)) {
                auto* const session = published_session_.load(std::memory_order_acquire);
                return session != nullptr ? &session->presentation() : nullptr;
            }
            if (current == bootstrap_state::preparing) {
                changed_.wait(lock, [this] {
                    return state_.load(std::memory_order_acquire) != bootstrap_state::preparing;
                });
                continue;
            }
            if (observed_focus != focus::evidence::foreground) {
                return nullptr;
            }

            state_.store(bootstrap_state::preparing, std::memory_order_release);
            lock.unlock();
            break;
        }
    } catch (...) {
        return nullptr;
    }

    std::unique_ptr<native_session> candidate;
    try {
        candidate = prepare_session_(borrowed_context_);
        if (candidate == nullptr) {
            finish_retryable_failure();
            return nullptr;
        }

        auto outcome = candidate->start();
        const auto final_state =
            outcome.has_value() ? bootstrap_state::ready : bootstrap_state::degraded;

        native_session* published_session{};
        xinput::service* presentation{};
        {
            std::unique_lock control_lock{control_gate_};
            candidate->presentation().enable(desired_enabled_ ? TRUE : FALSE);

            std::unique_lock bootstrap_lock{bootstrap_gate_};
            session_ = std::move(candidate);
            terminal_outcome_ = std::move(outcome);
            published_session = session_.get();
            presentation = &published_session->presentation();
            published_session_.store(published_session, std::memory_order_release);
            state_.store(final_state, std::memory_order_release);
        }

        published_session->activate();
        changed_.notify_all();
        return presentation;
    } catch (...) {
        candidate.reset();
        finish_retryable_failure();
        return nullptr;
    }
}

xinput::service* bootstrap_owner::presentation_if_published() noexcept {
    auto* const session = published_session_.load(std::memory_order_acquire);
    return session != nullptr ? &session->presentation() : nullptr;
}

native_session* bootstrap_owner::session_if_published() noexcept {
    return published_session_.load(std::memory_order_acquire);
}

void bootstrap_owner::accept_enable(const BOOL enabled) {
    std::scoped_lock lock{control_gate_};
    const bool normalized = enabled != FALSE;
    if (auto* const session = published_session_.load(std::memory_order_acquire);
        session != nullptr) {
        session->presentation().enable(normalized ? TRUE : FALSE);
    }
    desired_enabled_ = normalized;
}

bootstrap_state bootstrap_owner::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

const gameinput_result<void>& bootstrap_owner::terminal_outcome() const noexcept {
    return terminal_outcome_;
}

focus::evidence bootstrap_owner::sample_focus() const noexcept {
    const auto reading = focus_source_->observe();
    if (reading.process != process_) {
        return focus::evidence::unavailable;
    }
    switch (reading.state) {
    case focus::evidence::unavailable:
    case focus::evidence::background:
    case focus::evidence::foreground:
        return reading.state;
    }
    return focus::evidence::unavailable;
}

void bootstrap_owner::finish_retryable_failure() noexcept {
    {
        std::scoped_lock lock{bootstrap_gate_};
        state_.store(bootstrap_state::cold, std::memory_order_release);
    }
    changed_.notify_all();
}

} // namespace inputnext::runtime_detail
