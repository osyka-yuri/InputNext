#pragma once

#include <inputnext/output.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::xinput {
class service;
}

namespace inputnext::test::audio {
struct controlled_native_port_state;
}

namespace inputnext::runtime_detail {

class bootstrap_owner;
struct query_front;

struct runtime_api_test_options final {
    bool fail_native_start{};
    bool dualsense_hd_audio{};
};

struct bootstrap_owner_test_access final {
    [[nodiscard]] static bool control_gate_available(bootstrap_owner& owner);
};

class runtime_api_test_fixture final {
public:
    explicit runtime_api_test_fixture(bool fail_native_start = false);
    explicit runtime_api_test_fixture(runtime_api_test_options options);
    ~runtime_api_test_fixture();

    runtime_api_test_fixture(const runtime_api_test_fixture&) = delete;
    runtime_api_test_fixture& operator=(const runtime_api_test_fixture&) = delete;
    runtime_api_test_fixture(runtime_api_test_fixture&&) = delete;
    runtime_api_test_fixture& operator=(runtime_api_test_fixture&&) = delete;

    void block_next_prepare();
    [[nodiscard]] bool wait_for_prepare(std::chrono::milliseconds timeout);
    void release_prepare();
    void block_before_publication();
    [[nodiscard]] inputnext::xinput::service* wait_for_prepared_presentation(
        std::chrono::milliseconds timeout);
    void release_publication_factory();
    [[nodiscard]] bool wait_for_publication_factory_resume(std::chrono::milliseconds timeout);
    // The fake does not replay devices already connected when its watch is registered.
    void connect_native_device();

    [[nodiscard]] std::uint32_t pin_calls() const noexcept;
    [[nodiscard]] std::uint32_t owner_creations() const noexcept;
    [[nodiscard]] std::uint32_t focus_observations() const noexcept;
    [[nodiscard]] std::uint32_t prepare_attempts() const noexcept;
    [[nodiscard]] bool native_initialized() const noexcept;
    [[nodiscard]] bool owner_is_ready() const noexcept;
    [[nodiscard]] bool owner_is_degraded() const noexcept;
    [[nodiscard]] std::size_t rumble_writes() const noexcept;
    [[nodiscard]] std::optional<output::rumble_value> last_rumble_value() const;
    [[nodiscard]] std::shared_ptr<test::audio::controlled_native_port_state>
    controlled_audio_state() const noexcept;

private:
    friend query_front& runtime_query_front() noexcept;
    friend bootstrap_owner* runtime_bootstrap_owner() noexcept;

    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::runtime_detail
