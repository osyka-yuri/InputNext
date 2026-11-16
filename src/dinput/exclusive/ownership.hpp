#pragma once

#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "lease.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include <chrono>
#include <expected>
#include <memory>

namespace inputnext::dinput {

using native_input_create =
    HRESULT (*)(void*, HINSTANCE, DWORD, REFIID, void**, IUnknown*) noexcept;

struct native_input_factory final {
    native_input_create create{};
    void* context{};
    HINSTANCE instance{};
    std::shared_ptr<void> lifetime{nullptr};

    [[nodiscard]] bool supported() const noexcept;
};

struct background_exclusive_window final {
    HWND handle{};
    DWORD owner_thread{};
};

struct exclusive_request final {
    connection_target target{};
    GUID native_instance{};
    DIPROPGUIDANDPATH native_path{};
    focus::window_epoch window{};
    native_input_factory factory{};
    background_exclusive_window background_window{};
};

class exclusive_input final {
public:
    explicit exclusive_input(input_access& access);
    ~exclusive_input();

    exclusive_input(const exclusive_input&) = delete;
    exclusive_input& operator=(const exclusive_input&) = delete;
    exclusive_input(exclusive_input&&) = delete;
    exclusive_input& operator=(exclusive_input&&) = delete;

    [[nodiscard]] std::expected<std::shared_ptr<exclusive_lease>, HRESULT> reserve(
        exclusive_request request) noexcept;
    void refresh_eligibility(const input_access::call& call) noexcept;
    void close_admission() noexcept;
    void drain() noexcept;

private:
    friend struct exclusive_input_test_access;
    using proof_clock = std::chrono::steady_clock::time_point (*)(const void*) noexcept;
    [[nodiscard]] std::expected<std::shared_ptr<exclusive_lease>, HRESULT> reserve(
        exclusive_request request,
        proof_clock now,
        std::shared_ptr<const void> context) noexcept;

    struct actor;
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::dinput
