#pragma once

#include "device_info.hpp"
#include "device_properties.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "format.hpp"
#include "input_access.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <inputnext/focus.hpp>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::dinput {
class buffered_subscription;
class event_registration;

// Owns one device's mutable input configuration and acquisition transaction.
// Native resources remain with the session's exclusive ownership service.
class device_input final {
public:
    device_input(const device_identity& identity,
        std::shared_ptr<input_access> access,
        native_input_factory factory);
    ~device_input();
    device_input(const device_input&) = delete;
    device_input& operator=(const device_input&) = delete;
    device_input(device_input&&) = delete;
    device_input& operator=(device_input&&) = delete;
    [[nodiscard]] HRESULT acquire() noexcept;
    [[nodiscard]] HRESULT unacquire() noexcept;
    [[nodiscard]] HRESULT get_state(DWORD size, void* out) noexcept;
    [[nodiscard]] HRESULT
    get_data(DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) noexcept;
    [[nodiscard]] HRESULT poll() noexcept;
    [[nodiscard]] HRESULT set_format(const DIDATAFORMAT* format) noexcept;
    [[nodiscard]] HRESULT set_notification(HANDLE event) noexcept;
    [[nodiscard]] HRESULT set_cooperative(HWND window, DWORD flags) noexcept;
    [[nodiscard]] HRESULT get_property(const GUID* property, DIPROPHEADER* header) noexcept;
    [[nodiscard]] HRESULT set_property(const GUID* property, const DIPROPHEADER* header) noexcept;
    [[nodiscard]] std::optional<std::size_t> select_object(DWORD object, DWORD how) const noexcept;

private:
    struct exclusive_attempt;
    enum class acquisition : std::uint8_t { unacquired, acquired, lost };

    struct state_capture final {
        compiled_format format{};
        axis_configuration axes{};
        std::uint64_t generation{};
    };

    HRESULT acquire_impl();
    HRESULT acquire_immediate(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch,
        std::unique_lock<std::mutex> lock);
    // The subscription fence helpers require and return a held lock.
    std::expected<std::uint64_t, HRESULT> begin_acquisition(std::unique_lock<std::mutex>& lock);
    void discard_buffered_subscription(std::unique_lock<std::mutex>& lock);
    HRESULT acquire_exclusive(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch,
        std::unique_lock<std::mutex> lock);
    HRESULT prepare_exclusive_buffer(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch,
        std::uint64_t expected,
        exclusive_attempt& attempt);
    HRESULT complete_exclusive(const input_access::call& call,
        const exclusive_request& request,
        std::uint64_t expected,
        bool buffered,
        exclusive_attempt& attempt);
    void retire_exclusive() noexcept;
    bool target_available(const input_access::call& call) const;
    static bool window_matches(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch) noexcept;
    HRESULT acquire_buffered(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch,
        std::unique_lock<std::mutex> lock);
    bool advance_generation() noexcept;
    void mark_lost() noexcept;
    HRESULT acquisition_error() const noexcept;
    void observe_acquisition_loss() noexcept;
    HRESULT lose(std::uint64_t expected);
    HRESULT configure_allowed() noexcept;
    HRESULT check_acquisition(const input_access::call& call);
    HRESULT capture_state(const input_access::call& call, DWORD size, state_capture& out);
    HRESULT revalidate(const input_access::call& call, std::uint64_t generation);
    HRESULT revalidate_locked(const input_access::call& call, std::uint64_t generation);
    HRESULT capture_live_acquisition(const input_access::call& call, std::uint64_t& generation);
    HRESULT poll_eligibility();
    const compiled_format* active_format() const noexcept;
    const device_identity identity_;
    const std::shared_ptr<input_access> access_;
    const native_input_factory factory_;
    mutable std::mutex gate_;
    compiled_format format_{};
    axis_configuration axes_{};
    std::shared_ptr<buffered_subscription> buffered_;
    std::shared_ptr<event_registration> notification_;
    std::shared_ptr<exclusive_lease> exclusive_;
    std::shared_ptr<exclusive_lease> retiring_;
    HWND window_{};
    DWORD window_thread_{};
    DWORD cooperation_{};
    DWORD requested_buffer_size_{};
    std::uint64_t generation_{1U};
    acquisition acquisition_{acquisition::unacquired};
    HRESULT loss_{DIERR_INPUTLOST};
    std::optional<focus::window_epoch> acquired_window_;
    bool has_format_{};
    bool acquiring_{};
};
} // namespace inputnext::dinput
