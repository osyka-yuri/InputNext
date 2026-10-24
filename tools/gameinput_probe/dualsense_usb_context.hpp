#pragma once

#include "output/composition/internal_producer.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

class rumble_session;

struct dualsense_usb_context_close_result final {
    bool close_submitted{};
    bool acknowledged{};
};

class dualsense_usb_context final {
public:
    dualsense_usb_context() = default;
    ~dualsense_usb_context() = default;
    dualsense_usb_context(const dualsense_usb_context&) = delete;
    dualsense_usb_context& operator=(const dualsense_usb_context&) = delete;
    dualsense_usb_context(dualsense_usb_context&&) = delete;
    dualsense_usb_context& operator=(dualsense_usb_context&&) = delete;

    [[nodiscard]] output::result<void> open(rumble_session& session, connection_target target);
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const output::internal_context* context() const noexcept;
    [[nodiscard]] const output::mutation_receipt* latest_receipt() const noexcept;

    [[nodiscard]] output::result<output::mutation_receipt>
    submit(rumble_session& session, output::channel_values values, std::chrono::milliseconds ttl);
    [[nodiscard]] output::result<output::mutation_receipt> clear(rumble_session& session);
    [[nodiscard]] dualsense_usb_context_close_result close_and_acknowledge();

private:
    std::optional<output::detail::internal_producer> producer_;
    std::optional<output::internal_context> context_;
    std::optional<output::mutation_receipt> latest_receipt_;
    std::uint64_t sequence_{};
};

} // namespace inputnext::gameinput_detail
