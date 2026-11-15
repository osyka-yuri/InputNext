#include "input_access.hpp"

#include <atomic>
#include <chrono>
#include <utility>

namespace inputnext::dinput {

input_access::input_access(core::service& core,
    gameinput_runtime& runtime,
    focus::observer& focus,
    const std::chrono::milliseconds discovery_budget)
    : core_(core),
      runtime_(runtime),
      focus_(focus),
      discovery_(discovery_budget) {
}

input_access::~input_access() {
    close_admission();
    drain();
}

input_access::call::call(input_access* owner) noexcept : owner_(owner) {
}

input_access::call::~call() {
    if (owner_ != nullptr) {
        owner_->leave();
    }
}

input_access::call::call(call&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {
}

input_access::call& input_access::call::operator=(call&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) {
            owner_->leave();
        }
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

input_access::call::operator bool() const noexcept {
    return owner_ != nullptr;
}

core::service& input_access::call::core() const noexcept {
    return owner_->core_;
}

gameinput_runtime& input_access::call::runtime() const noexcept {
    return owner_->runtime_;
}

focus::observer& input_access::call::focus() const noexcept {
    return owner_->focus_;
}

input_access::call input_access::enter() noexcept {
    auto observed = admission_.load(std::memory_order_acquire);
    while ((observed & closed_bit) == 0U && observed < closed_bit - 1U) {
        if (admission_.compare_exchange_weak(observed,
                observed + 1U,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return call{this};
        }
    }
    return {};
}

catalogue_discovery& input_access::discovery() noexcept {
    return discovery_;
}

void input_access::notify_discovery() noexcept {
    discovery_.notify();
}

void input_access::leave() noexcept {
    if (admission_.fetch_sub(1U, std::memory_order_release) == closed_bit + 1U) {
        admission_.notify_all();
    }
}

void input_access::close_admission() noexcept {
    admission_.fetch_or(closed_bit, std::memory_order_acq_rel);
    discovery_.close();
}

void input_access::drain() noexcept {
    auto observed = admission_.load(std::memory_order_acquire);
    while ((observed & ~closed_bit) != 0U) {
        admission_.wait(observed, std::memory_order_acquire);
        observed = admission_.load(std::memory_order_acquire);
    }
}

bool input_access::open() const noexcept {
    return (admission_.load(std::memory_order_acquire) & closed_bit) == 0U;
}

} // namespace inputnext::dinput
