#include <Windows.h>
#include <string_view>

#include "rumble_window.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <system_error>
#include <utility>

namespace inputnext::gameinput_detail {

struct rumble_window_state final {
    rumble_window_state() = default;
    rumble_window_state(const rumble_window_state&) = delete;
    rumble_window_state& operator=(const rumble_window_state&) = delete;
    rumble_window_state(rumble_window_state&&) = delete;
    rumble_window_state& operator=(rumble_window_state&&) = delete;

    ~rumble_window_state() noexcept {
        if (window != nullptr) {
            DestroyWindow(window);
        }
    }

    HWND window{};
    HWND message{};
    HWND status{};
    HWND start{};
    HWND abort{};
    bool start_requested{};
    bool abort_requested{};
    bool close_requested{};
};

namespace {

constexpr auto window_class_name = std::to_array(L"InputNextGameInputRumbleProbeWindow");
constexpr auto state_property_name = std::to_array(L"InputNextGameInputRumbleProbeState");
constexpr int start_button_id = 101;
constexpr int abort_button_id = 102;

struct child_bounds final {
    int x{};
    int y{};
    int width{};
    int height{};
};

[[nodiscard]] LRESULT CALLBACK rumble_window_proc(HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* const state =
        static_cast<rumble_window_state*>(GetPropW(window, state_property_name.data()));

    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == start_button_id) {
            state->start_requested = true;
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == abort_button_id) {
            state->abort_requested = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        state->close_requested = true;
        state->abort_requested = true;
        return 0;
    case WM_DESTROY:
        state->window = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        RemovePropW(window, state_property_name.data());
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] HWND create_child(HWND parent,
    const wchar_t* class_name,
    const wchar_t* text,
    const DWORD style,
    const child_bounds bounds) {
    const HWND child = CreateWindowExW(0U,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        bounds.x,
        bounds.y,
        bounds.width,
        bounds.height,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    return child;
}

[[nodiscard]] bool set_control_id(HWND control, const int control_id) noexcept {
    if (control == nullptr) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(control, GWLP_ID, static_cast<LONG_PTR>(control_id)) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        DestroyWindow(control);
        return false;
    }
    return true;
}

} // namespace

rumble_window::rumble_window() : state_(std::make_unique<rumble_window_state>()) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = rumble_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = window_class_name.data();
    if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "RegisterClassExW");
    }

    state_->window = CreateWindowExW(0U,
        window_class_name.data(),
        L"InputNext GameInput Rumble Diagnostic",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        520,
        220,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    if (state_->window == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateWindowExW");
    }
    if (SetPropW(state_->window, state_property_name.data(), state_.get()) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "SetPropW(rumble_window_state)");
    }

    state_->message = create_child(state_->window,
        L"STATIC",
        L"Foreground-only GameInput rumble. Click Start to send available motor pulse(s), zero, "
        L"disable, cache, restore, and final-zero requests.",
        SS_LEFT,
        {.x = 16, .y = 16, .width = 480, .height = 45});
    state_->status = create_child(state_->window,
        L"STATIC",
        L"Starting runtime...",
        SS_LEFT,
        {.x = 16, .y = 72, .width = 480, .height = 30});
    state_->start = create_child(state_->window,
        L"BUTTON",
        L"Start sequence",
        BS_PUSHBUTTON,
        {.x = 16, .y = 125, .width = 150, .height = 32});
    if (!set_control_id(state_->start, start_button_id)) {
        state_->start = nullptr;
    }
    state_->abort = create_child(state_->window,
        L"BUTTON",
        L"Abort",
        BS_PUSHBUTTON,
        {.x = 180, .y = 125, .width = 100, .height = 32});
    if (!set_control_id(state_->abort, abort_button_id)) {
        state_->abort = nullptr;
    }
    if (state_->message == nullptr || state_->status == nullptr || state_->start == nullptr ||
        state_->abort == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateWindowExW(control)");
    }

    EnableWindow(state_->abort, FALSE);
    ShowWindow(state_->window, SW_SHOWNOACTIVATE);
    UpdateWindow(state_->window);
}

rumble_window::~rumble_window() noexcept {
    close();
}

bool rumble_window::valid() const noexcept {
    return state_ != nullptr && state_->window != nullptr;
}

bool rumble_window::pump_messages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
        if (message.message == WM_QUIT) {
            if (state_ != nullptr) {
                state_->close_requested = true;
                state_->abort_requested = true;
            }
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return valid() && !state_->close_requested;
}

bool rumble_window::take_start_request() noexcept {
    if (state_ == nullptr) {
        return false;
    }
    return std::exchange(state_->start_requested, false);
}

bool rumble_window::abort_requested() const noexcept {
    return state_ == nullptr || state_->abort_requested || state_->close_requested;
}

bool rumble_window::is_foreground() const noexcept {
    return valid() && GetForegroundWindow() == state_->window;
}

void rumble_window::set_status(const std::string_view status) noexcept {
    if (state_ == nullptr || state_->status == nullptr) {
        return;
    }
    std::array<wchar_t, 256U> text{};
    const auto length = std::min(status.size(), text.size() - 1U);
    for (std::size_t index = 0U; index < length; ++index) {
        text[index] = static_cast<wchar_t>(static_cast<unsigned char>(status[index]));
    }
    SetWindowTextW(state_->status, text.data());
}

void rumble_window::set_running(const bool running) noexcept {
    if (state_ == nullptr) {
        return;
    }
    if (state_->start != nullptr) {
        EnableWindow(state_->start, running ? FALSE : TRUE);
    }
    if (state_->abort != nullptr) {
        EnableWindow(state_->abort, running ? TRUE : FALSE);
    }
}

void rumble_window::close() noexcept {
    if (state_ != nullptr && state_->window != nullptr) {
        DestroyWindow(state_->window);
        state_->window = nullptr;
    }
}

} // namespace inputnext::gameinput_detail
