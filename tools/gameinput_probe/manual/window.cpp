#include "gameinput_probe/manual/window.hpp"

#include "gameinput_probe/manual/sequence.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace inputnext::gameinput_detail {

struct manual_window_state final {
    manual_window_state() = default;
    manual_window_state(const manual_window_state&) = delete;
    manual_window_state& operator=(const manual_window_state&) = delete;
    manual_window_state(manual_window_state&&) = delete;
    manual_window_state& operator=(manual_window_state&&) = delete;

    ~manual_window_state() noexcept {
        if (window != nullptr) {
            DestroyWindow(window);
        }
    }

    HWND window{};
    HWND message{};
    HWND status{};
    HWND start{};
    HWND abort{};
    HWND passed{};
    HWND failed{};
    std::optional<manual_step_confirmation> step_confirmation;
    manual_confirmation_id active_readiness_id{};
    bool start_requested{};
    bool abort_requested{};
    bool close_requested{};
};

namespace {

constexpr auto window_class_name = std::to_array(L"InputNextGameInputRumbleProbeWindow");
constexpr auto state_property_name = std::to_array(L"InputNextGameInputRumbleProbeState");
constexpr int start_button_id = 101;
constexpr int abort_button_id = 102;
constexpr int passed_button_id = 103;
constexpr int failed_button_id = 104;

struct child_bounds final {
    int x{};
    int y{};
    int width{};
    int height{};
};

[[nodiscard]] LRESULT CALLBACK manual_window_proc(HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* const state =
        static_cast<manual_window_state*>(GetPropW(window, state_property_name.data()));

    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && state->active_readiness_id != 0U &&
            !state->step_confirmation.has_value() &&
            (LOWORD(wparam) == passed_button_id || LOWORD(wparam) == failed_button_id)) {
            state->step_confirmation = manual_step_confirmation{
                .result = LOWORD(wparam) == passed_button_id ? manual_step_result::passed
                                                             : manual_step_result::failed,
                .readiness_id = state->active_readiness_id,
                .accepted_at = manual_sequence::clock::now()};
            return 0;
        }
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

manual_window::manual_window(const manual_window_options options)
    : state_(std::make_unique<manual_window_state>()) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = manual_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = window_class_name.data();
    if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "RegisterClassExW");
    }

    const std::wstring title_text{options.title};
    const std::wstring instructions_text{options.instructions};
    state_->window = CreateWindowExW(0U,
        window_class_name.data(),
        title_text.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        680,
        280,
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
            "SetPropW(manual_window_state)");
    }

    state_->message = create_child(state_->window,
        L"STATIC",
        instructions_text.c_str(),
        SS_LEFT,
        {.x = 16, .y = 16, .width = 630, .height = 48});
    state_->status = create_child(state_->window,
        L"STATIC",
        L"Starting runtime...",
        SS_LEFT,
        {.x = 16, .y = 76, .width = 630, .height = 70});
    state_->start = create_child(state_->window,
        L"BUTTON",
        L"Start sequence",
        BS_PUSHBUTTON,
        {.x = 16, .y = 166, .width = 120, .height = 36});
    if (!set_control_id(state_->start, start_button_id)) {
        state_->start = nullptr;
    }
    state_->abort = create_child(state_->window,
        L"BUTTON",
        L"Abort",
        BS_PUSHBUTTON,
        {.x = 550, .y = 166, .width = 96, .height = 36});
    if (!set_control_id(state_->abort, abort_button_id)) {
        state_->abort = nullptr;
    }
    state_->passed = create_child(state_->window,
        L"BUTTON",
        L"Works - Next",
        BS_PUSHBUTTON,
        {.x = 148, .y = 166, .width = 160, .height = 36});
    if (!set_control_id(state_->passed, passed_button_id)) {
        state_->passed = nullptr;
    }
    state_->failed = create_child(state_->window,
        L"BUTTON",
        L"Does not work - Next",
        BS_PUSHBUTTON,
        {.x = 320, .y = 166, .width = 218, .height = 36});
    if (!set_control_id(state_->failed, failed_button_id)) {
        state_->failed = nullptr;
    }
    if (state_->message == nullptr || state_->status == nullptr || state_->start == nullptr ||
        state_->abort == nullptr || state_->passed == nullptr || state_->failed == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateWindowExW(control)");
    }

    EnableWindow(state_->abort, FALSE);
    disable_confirmation();
    ShowWindow(state_->window, SW_SHOWNOACTIVATE);
    UpdateWindow(state_->window);
}

manual_window::~manual_window() noexcept {
    close();
}

bool manual_window::valid() const noexcept {
    return state_ != nullptr && state_->window != nullptr;
}

bool manual_window::pump_messages() noexcept {
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

bool manual_window::take_start_request() noexcept {
    if (state_ == nullptr) {
        return false;
    }
    return std::exchange(state_->start_requested, false);
}

bool manual_window::abort_requested() const noexcept {
    return state_ == nullptr || state_->abort_requested || state_->close_requested;
}

std::optional<manual_step_confirmation> manual_window::take_step_result() noexcept {
    return state_ != nullptr ? std::exchange(state_->step_confirmation, std::nullopt)
                             : std::nullopt;
}

void manual_window::enable_confirmation(const manual_confirmation_id readiness_id) noexcept {
    if (state_ == nullptr) {
        return;
    }
    if (readiness_id == 0U) {
        disable_confirmation();
        return;
    }
    state_->active_readiness_id = readiness_id;
    EnableWindow(state_->passed, TRUE);
    EnableWindow(state_->failed, TRUE);
}

void manual_window::disable_confirmation() noexcept {
    if (state_ == nullptr) {
        return;
    }
    state_->active_readiness_id = 0U;
    EnableWindow(state_->passed, FALSE);
    EnableWindow(state_->failed, FALSE);
}

bool manual_window::is_foreground() const noexcept {
    return valid() && GetForegroundWindow() == state_->window;
}

void manual_window::set_status(const std::string_view status) noexcept {
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

void manual_window::set_running(const bool running) noexcept {
    if (state_ == nullptr) {
        return;
    }
    disable_confirmation();
    if (state_->start != nullptr) {
        EnableWindow(state_->start, running ? FALSE : TRUE);
    }
    if (state_->abort != nullptr) {
        EnableWindow(state_->abort, running ? TRUE : FALSE);
    }
}

void manual_window::close() noexcept {
    if (state_ != nullptr && state_->window != nullptr) {
        DestroyWindow(state_->window);
        state_->window = nullptr;
    }
}

} // namespace inputnext::gameinput_detail
