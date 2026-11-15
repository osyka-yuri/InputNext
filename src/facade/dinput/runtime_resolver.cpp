#include "runtime_resolver.hpp"

#include "facade/runtime_module.hpp"
#include "runtime/abi/dinput/api.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <type_traits>

namespace inputnext::dinput_facade_detail {
namespace {

constexpr std::uint32_t runtime_abi_version = 1U;
constinit INIT_ONCE resolver_once = INIT_ONCE_STATIC_INIT;
constinit std::atomic<const input_next_directinput_api_v1*> cached_api{nullptr};
constinit thread_local bool resolving_on_this_thread{};

static_assert(std::is_trivially_destructible_v<decltype(resolver_once)>);
static_assert(std::is_trivially_destructible_v<decltype(cached_api)>);

[[nodiscard]] bool callback_addresses_belong_to(const HMODULE provider,
    const input_next_directinput_api_v1* const api) noexcept {
    const std::array<LPCWSTR, 5U> addresses{facade_detail::as_module_address(api),
        facade_detail::as_module_address(api->create),
        facade_detail::as_module_address(api->get_class_object),
        facade_detail::as_module_address(api->can_unload),
        facade_detail::as_module_address(api->joystick_format)};
    return facade_detail::callback_addresses_belong_to(provider, addresses);
}

[[nodiscard]] const input_next_directinput_api_v1* query_runtime_module(
    const HMODULE runtime_module) noexcept {
    const FARPROC query_address =
        GetProcAddress(runtime_module, "input_next_runtime_query_directinput_api");
    if (query_address == nullptr) {
        return nullptr;
    }

    const auto query = reinterpret_cast<input_next_query_directinput_api_fn>(query_address);
    const auto* const api = query(runtime_abi_version);
    if (!is_valid_input_next_directinput_api_v1(api) ||
        !callback_addresses_belong_to(runtime_module, api)) {
        return nullptr;
    }

    if (!facade_detail::pin_runtime_module(runtime_module,
            facade_detail::as_module_address(api->create))) {
        return nullptr;
    }
    return api;
}

[[nodiscard]] BOOL CALLBACK resolve_once([[maybe_unused]] PINIT_ONCE once,
    [[maybe_unused]] PVOID parameter,
    [[maybe_unused]] PVOID* context) noexcept {
    try {
        std::wstring runtime_path;
        if (!facade_detail::sibling_runtime_path(facade_detail::as_module_address(&cached_api),
                runtime_path)) {
            return FALSE;
        }

        constexpr DWORD search_flags =
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        const HMODULE runtime_module = LoadLibraryExW(runtime_path.c_str(), nullptr, search_flags);
        if (runtime_module == nullptr) {
            return FALSE;
        }

        const auto* const candidate_api = query_runtime_module(runtime_module);
        const BOOL released = FreeLibrary(runtime_module);
        if (released == FALSE || candidate_api == nullptr) {
            return FALSE;
        }

        cached_api.store(candidate_api, std::memory_order_release);
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

class resolver_recursion_scope final {
public:
    resolver_recursion_scope() noexcept { resolving_on_this_thread = true; }

    ~resolver_recursion_scope() noexcept { resolving_on_this_thread = false; }

    resolver_recursion_scope(const resolver_recursion_scope&) = delete;
    resolver_recursion_scope& operator=(const resolver_recursion_scope&) = delete;
    resolver_recursion_scope(resolver_recursion_scope&&) = delete;
    resolver_recursion_scope& operator=(resolver_recursion_scope&&) = delete;
};

} // namespace

const input_next_directinput_api_v1* runtime_api() noexcept {
    if (const auto* const published = cached_api.load(std::memory_order_acquire);
        published != nullptr) {
        return published;
    }
    if (resolving_on_this_thread) {
        return nullptr;
    }

    const resolver_recursion_scope recursion_scope;
    if (InitOnceExecuteOnce(&resolver_once, &resolve_once, nullptr, nullptr) == FALSE) {
        return nullptr;
    }
    return cached_api.load(std::memory_order_acquire);
}

} // namespace inputnext::dinput_facade_detail
