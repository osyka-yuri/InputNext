#pragma once

#include "catalogue_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/exclusive/lease.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/native_types.hpp"
#include "support/focus/manual_clock.hpp"

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace inputnext::dinput {

struct exclusive_input_test_access final {
    [[nodiscard]] static auto reserve(exclusive_input& input,
        exclusive_request request,
        std::shared_ptr<test::focus::manual_clock> clock) noexcept {
        return input.reserve(std::move(request),
            &test::focus::manual_clock::read,
            std::move(clock));
    }
};

} // namespace inputnext::dinput

namespace inputnext::dinput_native_test_support {

using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;

inline constexpr std::string_view device_path{R"(\\?\HID#VID_054C&PID_0CE6#NativeActor)"};
inline constexpr std::wstring_view native_device_path{LR"(\\?\HID#VID_054C&PID_0CE6#NativeActor)"};

using lease_result = std::expected<std::shared_ptr<dinput::exclusive_lease>, HRESULT>;

class native_service_fixture final {
public:
    explicit native_service_fixture(const focus_window_fixture::clock_callback now = nullptr,
        const void* clock_context = nullptr)
        : focus{now, clock_context} {
        focus.set_window(window.id(), 2U);
        if (!focus.wait_for_window(window.id(), 2U)) {
            throw std::runtime_error{"foreground observation unavailable"};
        }
        catalogue.context.access = make_access(*catalogue.owners, focus.observer());
        catalogue.add_system_gamepad(native_guid, std::wstring{native_device_path});
        catalogue.system->controls = std::make_shared<native_control_script>();
        target = catalogue.connect(0U, std::string{device_path});
        if (!catalogue.wait_for_catalogue(1U)) {
            throw std::runtime_error{"input catalogue unavailable"};
        }
        provider = std::make_shared<native_factory_context>();
        provider->system = catalogue.system;
        factory = dinput::native_input_factory{.create = &create_native_system,
            .context = provider.get(),
            .instance = GetModuleHandleW(nullptr),
            .lifetime = provider};
    }

    ~native_service_fixture() {
        release_all_native_blocks();
        catalogue.context.access->close_admission();
        catalogue.context.access->drain();
    }

    native_service_fixture(const native_service_fixture&) = delete;
    native_service_fixture& operator=(const native_service_fixture&) = delete;
    native_service_fixture(native_service_fixture&&) = delete;
    native_service_fixture& operator=(native_service_fixture&&) = delete;

    [[nodiscard]] dinput::exclusive_request request(const GUID requested_guid = native_guid,
        const std::wstring_view requested_path = native_device_path) {
        const auto presentation = focus.observer().lock_presentation();
        const auto epoch = presentation.current_foreground_window_epoch(window.id());
        if (!epoch.has_value()) {
            throw std::runtime_error{"foreground window epoch unavailable"};
        }

        DIPROPGUIDANDPATH path{};
        path.diph.dwSize = static_cast<DWORD>(sizeof(path));
        path.diph.dwHeaderSize = static_cast<DWORD>(sizeof(DIPROPHEADER));
        path.diph.dwHow = DIPH_DEVICE;
        path.guidClass = GUID_Joystick;
        const std::wstring path_value{requested_path};
        if (wcscpy_s(path.wszPath, path_value.c_str()) != 0) {
            throw std::runtime_error{"native path exceeds DirectInput capacity"};
        }
        return dinput::exclusive_request{.target = target,
            .native_instance = requested_guid,
            .native_path = path,
            .window = *epoch,
            .factory = factory};
    }

    [[nodiscard]] lease_result reserve(dinput::exclusive_request value) const {
        const auto call = catalogue.context.access->enter();
        if (!call) {
            return std::unexpected{DIERR_INPUTLOST};
        }
        return call.exclusives().reserve(std::move(value));
    }

    [[nodiscard]] lease_result reserve_background(std::shared_ptr<test::focus::manual_clock> clock,
        HWND background_window = nullptr) {
        auto value = request();
        value.window = {};
        value.background_window = {.handle = background_window == nullptr ? window.get()
                                                                          : background_window,
            .owner_thread = GetCurrentThreadId()};
        const auto call = catalogue.context.access->enter();
        if (!call) {
            return std::unexpected{DIERR_INPUTLOST};
        }
        if (clock == nullptr) {
            return call.exclusives().reserve(std::move(value));
        }
        return dinput::exclusive_input_test_access::reserve(call.exclusives(),
            std::move(value),
            std::move(clock));
    }

    void release_all_native_blocks() const noexcept {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(native_operation::count);
            ++index) {
            catalogue.system->controls->release_block(static_cast<native_operation>(index));
        }
        catalogue.system->controls->clear_send_message();
    }

    [[nodiscard]] std::shared_ptr<native_control_script> controls() const noexcept {
        return catalogue.system->controls;
    }

    hidden_window window;
    focus_window_fixture focus;
    environment catalogue;
    connection_target target{};
    std::shared_ptr<native_factory_context> provider;
    dinput::native_input_factory factory;

private:
    inline static const GUID native_guid = guid(1U);
};

[[nodiscard]] inline std::size_t selected_count(const native_control_script& controls,
    const native_operation operation) {
    const auto observations = controls.observations();
    return static_cast<std::size_t>(
        std::ranges::count_if(observations, [operation](const native_observation& observation) {
            return observation.operation == operation && observation.selected_device;
        }));
}

} // namespace inputnext::dinput_native_test_support
