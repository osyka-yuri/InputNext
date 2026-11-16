#pragma once

#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/service.hpp"
#include "output/authority_fixture.hpp"

#include <Windows.h>
#include <dinput.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace inputnext::dinput::feedback_test_support {

[[nodiscard]] force_feedback::effect_parameters constant(LONG magnitude = 8000);

class feedback_fixture final {
public:
    feedback_fixture();
    ~feedback_fixture();

    feedback_fixture(const feedback_fixture&) = delete;
    feedback_fixture& operator=(const feedback_fixture&) = delete;
    feedback_fixture(feedback_fixture&&) = delete;
    feedback_fixture& operator=(feedback_fixture&&) = delete;

    [[nodiscard]] force_feedback::eligibility acquisition();
    [[nodiscard]] std::uint64_t create(const void* device, LONG magnitude = 8000) const;
    [[nodiscard]] bool wait_for_amplitude(float value);
    [[nodiscard]] bool wait_for_stopped(std::uint64_t id) const;
    [[nodiscard]] bool pause_next_query() noexcept;
    [[nodiscard]] static force_feedback::motors query_motors(void* context,
        connection_target requested) noexcept;

    const runtime_id runtime = test::output::runtime_identity(std::byte{0x71U});
    device_registry registry;
    dinput_test_support::focus_window_fixture focus;
    output::detail::composition trusted;
    connection_target target{};
    std::atomic<bool> available{true};
    std::atomic<bool> low{true};
    std::atomic<bool> high{true};
    std::atomic<std::uint64_t> backend_id{1U};
    output::execution_owner backend_owner{};
    std::atomic<bool> pause_query;
    std::atomic<bool> query_entered;
    std::atomic<bool> release_query;
    std::atomic<DWORD> paused_query_thread;
    std::unique_ptr<output::executor> executor;
    std::unique_ptr<force_feedback::service> feedback;
};

} // namespace inputnext::dinput::feedback_test_support
