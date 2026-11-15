#include "api.hpp"

#include "dinput/com/factory.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/abi/session_access.hpp"
#include "runtime/bootstrap/query_front.hpp"
#include "runtime/session/native_paths.hpp"
#include "runtime/session/native_session.hpp"

#include <cstdint>
#include <memory>

namespace inputnext::runtime_detail {
namespace {

std::shared_ptr<dinput::input_access> directinput_session([[maybe_unused]] void* context) noexcept {
    auto* const session = session_for(session_access::initialize_ready);
    return session != nullptr ? session->directinput() : nullptr;
}

bool is_runtime_provider([[maybe_unused]] void* context, HINSTANCE instance) noexcept {
    if (instance == nullptr || instance == GetModuleHandleW(nullptr)) {
        return false;
    }
    try {
        return GetModuleHandleW(runtime_library_path().c_str()) == instance;
    } catch (...) {
        return false;
    }
}

constinit dinput::host directinput_host{.session = &directinput_session,
    .is_runtime_provider = &is_runtime_provider};

HRESULT WINAPI create_directinput(HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept {
    return dinput::create_root(directinput_host, instance, version, iid, out, outer);
}

HRESULT WINAPI directinput_class_object(REFCLSID clsid, REFIID iid, void** out) noexcept {
    return dinput::get_class_object(directinput_host, clsid, iid, out);
}

HRESULT WINAPI directinput_can_unload() noexcept {
    return dinput::can_unload();
}

LPCDIDATAFORMAT WINAPI directinput_format() noexcept {
    return dinput::system_joystick_format();
}

constinit const input_next_directinput_api_v1 directinput_api{
    .struct_size = sizeof(input_next_directinput_api_v1),
    .abi_version = 1U,
    .create = &create_directinput,
    .get_class_object = &directinput_class_object,
    .can_unload = &directinput_can_unload,
    .joystick_format = &directinput_format};

} // namespace
} // namespace inputnext::runtime_detail

extern "C" const input_next_directinput_api_v1* __cdecl input_next_runtime_query_directinput_api(
    const std::uint32_t requested_abi) noexcept {
    using namespace inputnext::runtime_detail;
    if (requested_abi != 1U || query_runtime_api(runtime_query_front(), 1U) == nullptr) {
        return nullptr;
    }
    return &directinput_api;
}
