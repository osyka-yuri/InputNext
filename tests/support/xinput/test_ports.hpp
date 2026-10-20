#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/source_lifetime.hpp>
#include <inputnext/xinput.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::xinput_test_support {

[[nodiscard]] inline runtime_id make_runtime_id(std::uint8_t first_byte) noexcept {
    runtime_id result{};
    result.bytes[0] = static_cast<std::byte>(first_byte);
    return result;
}

class focus_source final : public focus::source {
public:
    [[nodiscard]] focus::process_instance process() const noexcept override {
        return {.pid = 1234U, .creation_time_100ns = 5678U};
    }

    [[nodiscard]] focus::reading observe() noexcept override {
        return {.process = process(),
            .state = state_.load(std::memory_order_relaxed),
            .native_code = 0U};
    }

    void set(focus::evidence state) noexcept { state_.store(state, std::memory_order_relaxed); }

private:
    std::atomic<focus::evidence> state_{focus::evidence::foreground};
};

class focus_fixture final {
public:
    focus_fixture()
        : source_(std::make_unique<focus_source>()),
          source_view_(source_.get()),
          observer_(make_runtime_id(0x21U), std::move(source_)) {
        wait_for(focus::evidence::foreground, 0U);
    }

    [[nodiscard]] focus::observer& observer() noexcept { return observer_; }

    [[nodiscard]] focus_source& source() noexcept { return *source_view_; }

    [[nodiscard]] focus::snapshot published() const { return observer_.published(); }

    void set(focus::evidence state) {
        const auto before = observer_.published();
        if (before.state == state) {
            return;
        }
        source_view_->set(state);
        wait_for(state, before.revision);
    }

private:
    void wait_for(focus::evidence state, std::uint64_t after_revision) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto current = observer_.published();
            if (current.state == state && current.revision > after_revision) {
                return;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error{"focus observation did not reach requested state"};
    }

    std::unique_ptr<focus_source> source_;
    focus_source* source_view_{};
    focus::observer observer_;
};

class test_source_lifetime final : public source_lifetime {};

struct test_connection final {
    admitted_connection connection;
};

class registry_fixture final {
public:
    registry_fixture() : registry_(make_runtime_id(0x41U)) {}

    [[nodiscard]] device_registry& registry() noexcept { return registry_; }

    [[nodiscard]] test_connection add(gamepad_state state = {}) {
        std::unique_ptr<source_lifetime> candidate = std::make_unique<test_source_lifetime>();
        auto admitted = registry_.admit(candidate);
        if (!admitted.has_value()) {
            throw std::runtime_error{"test device registry admission failed"};
        }
        const input_update update{.state = state, .source_timestamp_us = 1U, .observation_qpc = 1U};
        if (!admitted->producer.publish(update).has_value()) {
            throw std::runtime_error{"test sample publication failed"};
        }
        return test_connection{.connection = *admitted};
    }

private:
    device_registry registry_;
};

class metadata_source final : public xinput::metadata_source {
public:
    [[nodiscard]] metadata_result<metadata_snapshot> query(connection_target target) override {
        std::unique_lock lock{gate_};
        ++calls_;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !blocked_; });
        if (failure_.has_value()) {
            return std::unexpected{*failure_};
        }
        auto value = snapshot_;
        if (!mismatch_target_) {
            value.target = target;
        }
        return value;
    }

    void set_snapshot(metadata_snapshot value) {
        std::scoped_lock lock{gate_};
        snapshot_ = std::move(value);
    }

    void set_failure(std::optional<metadata_error> failure) {
        std::scoped_lock lock{gate_};
        failure_ = failure;
    }

    void mismatch_target(bool mismatch) {
        std::scoped_lock lock{gate_};
        mismatch_target_ = mismatch;
    }

    void block_queries() {
        std::scoped_lock lock{gate_};
        blocked_ = true;
        entered_ = false;
    }

    [[nodiscard]] bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release_queries() {
        std::scoped_lock lock{gate_};
        blocked_ = false;
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t calls() const {
        std::scoped_lock lock{gate_};
        return calls_;
    }

private:
    mutable std::mutex gate_;
    std::condition_variable changed_;
    metadata_snapshot snapshot_{};
    std::optional<metadata_error> failure_;
    std::size_t calls_{};
    bool mismatch_target_{};
    bool blocked_{};
    bool entered_{};
};

class release_queries_on_exit final {
public:
    explicit release_queries_on_exit(metadata_source& source) noexcept : source_(source) {}

    ~release_queries_on_exit() { source_.release_queries(); }

    release_queries_on_exit(const release_queries_on_exit&) = delete;
    release_queries_on_exit& operator=(const release_queries_on_exit&) = delete;
    release_queries_on_exit(release_queries_on_exit&&) = delete;
    release_queries_on_exit& operator=(release_queries_on_exit&&) = delete;

private:
    metadata_source& source_;
};

[[nodiscard]] inline metadata_snapshot metadata_for(connection_target target = {}) {
    metadata_snapshot result{};
    result.target = target;
    return result;
}

} // namespace inputnext::xinput_test_support
