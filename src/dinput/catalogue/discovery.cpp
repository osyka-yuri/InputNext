#include "discovery.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext::dinput {

catalogue_discovery::waiter::waiter(std::shared_ptr<state> owner, const HANDLE event) noexcept
    : owner_(std::move(owner)),
      event_(event) {
}

catalogue_discovery::waiter::~waiter() {
    std::scoped_lock lock{owner_->gate};
    auto** current = &owner_->waiters;
    while (*current != this) {
        current = &(*current)->next_;
    }
    *current = next_;
    static_cast<void>(CloseHandle(event_));
    --owner_->waiter_count;
}

bool catalogue_discovery::waiter::prepare() noexcept {
    std::scoped_lock lock{owner_->gate};
    return !owner_->closed && clock::now() < owner_->deadline && ResetEvent(event_) != FALSE;
}

HANDLE catalogue_discovery::waiter::event() const noexcept {
    return event_;
}

catalogue_discovery::clock::time_point catalogue_discovery::waiter::deadline() const noexcept {
    std::scoped_lock lock{owner_->gate};
    return owner_->deadline;
}

catalogue_discovery::catalogue_discovery(const std::chrono::milliseconds budget)
    : state_(std::make_shared<state>(budget)) {
}

catalogue_discovery::~catalogue_discovery() {
    close();
}

std::unique_ptr<catalogue_discovery::waiter> catalogue_discovery::begin() {
    const auto owner = state_;
    std::scoped_lock lock{owner->gate};
    if (owner->closed || owner->waiter_count >= waiter_capacity) {
        return {};
    }
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return {};
    }
    std::unique_ptr<waiter> result{new (std::nothrow) waiter{owner, event}};
    if (result == nullptr) {
        CloseHandle(event);
        return {};
    }

    if (owner->waiter_count == 0U) {
        owner->deadline = clock::now() + owner->budget;
    }
    result->next_ = owner->waiters;
    owner->waiters = result.get();
    ++owner->waiter_count;
    return result;
}

void catalogue_discovery::notify() noexcept {
    const auto owner = state_;
    std::scoped_lock lock{owner->gate};
    for (const auto* current = owner->waiters; current != nullptr; current = current->next_) {
        static_cast<void>(SetEvent(current->event_));
    }
}

void catalogue_discovery::close() noexcept {
    const auto owner = state_;
    std::scoped_lock lock{owner->gate};
    if (owner->closed) {
        return;
    }
    owner->closed = true;
    for (const auto* current = owner->waiters; current != nullptr; current = current->next_) {
        static_cast<void>(SetEvent(current->event_));
    }
}

} // namespace inputnext::dinput
