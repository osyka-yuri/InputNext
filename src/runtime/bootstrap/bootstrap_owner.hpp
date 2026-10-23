#pragma once

#include "runtime/session/native_session.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace inputnext::xinput {
class service;
} // namespace inputnext::xinput

namespace inputnext::runtime_detail {

struct bootstrap_owner_test_access;

enum class bootstrap_state : std::uint8_t {
    cold,
    preparing,
    ready,
    degraded,
};

using native_session_factory = std::unique_ptr<native_session> (*)(void* context);

class bootstrap_owner final {
public:
    bootstrap_owner(std::unique_ptr<focus::source> focus_source,
        native_session_factory prepare_session,
        void* borrowed_context);
    ~bootstrap_owner() noexcept;

    bootstrap_owner(const bootstrap_owner&) = delete;
    bootstrap_owner& operator=(const bootstrap_owner&) = delete;
    bootstrap_owner(bootstrap_owner&&) = delete;
    bootstrap_owner& operator=(bootstrap_owner&&) = delete;

    [[nodiscard]] xinput::service* bootstrap() noexcept;
    [[nodiscard]] xinput::service* presentation_if_published() noexcept;
    void accept_enable(BOOL enabled);

    [[nodiscard]] bootstrap_state state() const noexcept;
    [[nodiscard]] const gameinput_result<void>& terminal_outcome() const noexcept;

private:
    friend struct bootstrap_owner_test_access;

    [[nodiscard]] focus::evidence sample_focus() const noexcept;
    void finish_retryable_failure() noexcept;

    const focus::process_instance process_;
    std::unique_ptr<focus::source> focus_source_;
    native_session_factory const prepare_session_;
    void* const borrowed_context_;

    mutable std::mutex bootstrap_gate_;
    std::condition_variable changed_;
    std::atomic<bootstrap_state> state_{bootstrap_state::cold};
    std::unique_ptr<native_session> session_;
    std::atomic<native_session*> published_session_{nullptr};
    gameinput_result<void> terminal_outcome_;

    mutable std::mutex control_gate_;
    bool desired_enabled_{true};
};

} // namespace inputnext::runtime_detail
