#include "control_wait.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace inputnext::dinput {
namespace {

struct wait_depth final {
    wait_depth() noexcept { ++native_wait_depth; }

    ~wait_depth() { --native_wait_depth; }

    wait_depth(const wait_depth&) = delete;
    wait_depth& operator=(const wait_depth&) = delete;
    wait_depth(wait_depth&&) = delete;
    wait_depth& operator=(wait_depth&&) = delete;

    static thread_local std::uint32_t native_wait_depth;
};

thread_local std::uint32_t wait_depth::native_wait_depth{};

[[nodiscard]] DWORD remaining_timeout(
    const std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
    if (!deadline.has_value()) {
        return INFINITE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now).count();
    const auto maximum = static_cast<long long>(std::numeric_limits<DWORD>::max() - 1U);
    return static_cast<DWORD>(std::min(remaining, maximum));
}

} // namespace

bool native_control_reentrant() noexcept {
    return wait_depth::native_wait_depth != 0U || InSendMessageEx(nullptr) != 0U;
}

HRESULT wait_for_native_control(const std::span<const HANDLE> events,
    std::size_t& signalled,
    const std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
    signalled = 0U;
    if (events.empty() || events.size() > static_cast<std::size_t>(MAXIMUM_WAIT_OBJECTS - 1U) ||
        std::ranges::any_of(events, [](const HANDLE event) { return event == nullptr; })) {
        return E_INVALIDARG;
    }
    if (native_control_reentrant()) {
        return E_PENDING;
    }

    const wait_depth active_wait{};
    const auto count = static_cast<DWORD>(events.size());
    for (;;) {
        const auto timeout = remaining_timeout(deadline);
        if (timeout == 0U) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(count,
            events.data(),
            timeout,
            QS_SENDMESSAGE,
            MWMO_INPUTAVAILABLE);
        if (result < WAIT_OBJECT_0 + count) {
            signalled = static_cast<std::size_t>(result - WAIT_OBJECT_0);
            return S_OK;
        }
        if (result == WAIT_OBJECT_0 + count) {
            MSG message{};
            // Select sent messages by queue category and leave posted input queued.
            static_cast<void>(
                PeekMessageW(&message, nullptr, 0U, 0U, PM_NOREMOVE | PM_QS_SENDMESSAGE));
            continue;
        }
        if (result == WAIT_TIMEOUT) {
            if (deadline.has_value() && std::chrono::steady_clock::now() < *deadline) {
                continue;
            }
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (result == WAIT_FAILED) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
        }
        return E_UNEXPECTED;
    }
}

} // namespace inputnext::dinput
