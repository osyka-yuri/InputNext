#pragma once

#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/system_input.hpp" // IWYU pragma: export

#include <memory>

namespace inputnext::dinput {

// Callbacks/context are borrowed and must outlive returned COM objects.
struct host final {
    std::shared_ptr<input_access> (*session)(void* context) noexcept {};
    void* context{};
    HRESULT (*create_system)(void*, HINSTANCE, DWORD, REFIID, void**, IUnknown*) noexcept {
        &create_system_input};
    // Custom providers retain their context/code through asynchronous ownership work.
    std::shared_ptr<void> system_lifetime{nullptr};
    bool (*is_runtime_provider)(void*, HINSTANCE) noexcept {};
};

[[nodiscard]] HRESULT create_root(const host& host,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept;

namespace detail {

// Class-factory activation creates a root without initializing the system provider.
[[nodiscard]] HRESULT
create_uninitialized_root(const host& host, IUnknown* outer, REFIID iid, void** out) noexcept;

} // namespace detail

[[nodiscard]] HRESULT
get_class_object(const host& host, REFCLSID clsid, REFIID iid, void** out) noexcept;
[[nodiscard]] HRESULT can_unload() noexcept;

// All DI COM implementations contribute to one runtime-local unload observation.
void object_created() noexcept;
void object_destroyed() noexcept;

} // namespace inputnext::dinput
