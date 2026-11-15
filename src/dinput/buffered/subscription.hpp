#pragma once

#include "dinput/device_properties.hpp"
#include "dinput/format.hpp"
#include "dinput/profile.hpp"

#include "history/reader_observation.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>

#include <Windows.h>

#include <array>
#include <memory>
#include <optional>

namespace inputnext::dinput {

inline constexpr DWORD maximum_buffer_size = 4096U;

class event_registration;

struct buffer_configuration final {
    connection_target target{};
    compiled_format format{};
    axis_configuration axes{};
    DWORD requested_size{};
    std::shared_ptr<event_registration> notification{nullptr};
    std::optional<focus::window_epoch> window{std::nullopt};
};

class buffered_subscription final {
public:
    explicit buffered_subscription(buffer_configuration configuration);
    ~buffered_subscription();

    buffered_subscription(const buffered_subscription&) = delete;
    buffered_subscription& operator=(const buffered_subscription&) = delete;
    buffered_subscription(buffered_subscription&&) = delete;
    buffered_subscription& operator=(buffered_subscription&&) = delete;

    [[nodiscard]] connection_target target() const noexcept;
    [[nodiscard]] std::optional<focus::window_epoch> window() const noexcept;
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] HRESULT wait_ready();

    void prepare(const history_detail::reader_boundary& boundary,
        std::optional<history_sample> baseline) noexcept;
    [[nodiscard]] bool activate() noexcept;
    void accept(const history_detail::observed_read& read,
        const std::array<DWORD, history_batch_capacity>& sequences) noexcept;
    [[nodiscard]] HRESULT read(DWORD stride,
        DIDEVICEOBJECTDATA* destination,
        DWORD requested,
        DWORD& returned,
        bool peek) noexcept;

    void cancel() noexcept;
    void lose() noexcept;
    void fail(HRESULT error) noexcept;
    void signal_pending() noexcept;
    void synchronize_signals() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::dinput
