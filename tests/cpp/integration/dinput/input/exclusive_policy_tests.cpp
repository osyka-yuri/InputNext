#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/com/device.hpp"
#include "dinput/device_info.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/native_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>

using namespace inputnext;
using namespace inputnext::dinput_test_support;

namespace {

struct creation_counter final {
    std::atomic<unsigned> calls;
};

HRESULT unexpected_creation(void* context,
    [[maybe_unused]] HINSTANCE instance,
    [[maybe_unused]] DWORD version,
    [[maybe_unused]] REFIID iid,
    void** output,
    [[maybe_unused]] IUnknown* outer) noexcept {
    static_cast<creation_counter*>(context)->calls.fetch_add(1U, std::memory_order_relaxed);
    *output = nullptr;
    return E_UNEXPECTED;
}

} // namespace

TEST_CASE("dinput.exclusive_policy_requires_exact_path_and_safe_factory_lifetime",
    "[integration][dinput][exclusive][policy]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const auto access = make_access(owners, *owners.focus_observer);
    const auto counter = std::make_shared<creation_counter>();
    parent_unknown parent;
    auto identity = identity_for(target);
    dinput::native_input_factory factory{.create = &unexpected_creation,
        .context = counter.get(),
        .instance = GetModuleHandleW(nullptr),
        .lifetime = counter};
    SECTION("missing native path") {
    }
    SECTION("missing provider lifetime") {
        DIPROPGUIDANDPATH path{};
        path.guidClass = GUID_Joystick;
        wcscpy_s(path.wszPath, L"Exact native path");
        identity.path = path;
        factory.lifetime.reset();
    }
    void* raw{};
    REQUIRE(dinput::make_device(identity, access, parent, true, &raw, factory) == DI_OK);
    const com_owner<IDirectInputDevice8W> device{static_cast<IDirectInputDevice8W*>(raw)};
    const hidden_window window;
    configure_compact_device(*device, window.get());
    CHECK(device->SetCooperativeLevel(window.get(), DISCL_FOREGROUND | DISCL_EXCLUSIVE) ==
          DIERR_UNSUPPORTED);
    CHECK(device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_EXCLUSIVE) ==
          DIERR_UNSUPPORTED);
    REQUIRE(device->Acquire() == DI_OK);
    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(device->Unacquire() == DI_OK);
    CHECK(counter->calls.load(std::memory_order_relaxed) == 0U);
}
