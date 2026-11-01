#include "runtime_resolver.hpp"

#include "runtime/abi/xinput/api.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace inputnext::xinput_facade_detail {
namespace {

constexpr std::uint32_t runtime_abi_version = 1U;
constexpr DWORD path_limit = 32'768U;
constexpr DWORD initial_path_capacity = 512U;

constinit INIT_ONCE resolver_once = INIT_ONCE_STATIC_INIT;
constinit std::atomic<const input_next_runtime_api_v1*> cached_api{nullptr};
constinit thread_local bool resolving_on_this_thread{};

static_assert(std::is_trivially_destructible_v<decltype(resolver_once)>);
static_assert(std::is_trivially_destructible_v<decltype(cached_api)>);

[[nodiscard]] bool has_extended_prefix(const std::wstring_view path) noexcept {
    return path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\");
}

[[nodiscard]] bool is_absolute_path(const std::wstring_view path) noexcept {
    if (path.size() >= 3U && path[1U] == L':' && (path[2U] == L'\\' || path[2U] == L'/')) {
        return true;
    }
    return path.starts_with(L"\\\\");
}

[[nodiscard]] bool get_path_from_module(const HMODULE module, std::wstring& path) {
    std::vector<wchar_t> buffer(initial_path_capacity);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return false;
        }
        if (length < buffer.size()) {
            path.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= path_limit) {
            return false;
        }
        const auto next_size = buffer.size() * 2U;
        buffer.resize(next_size > path_limit ? path_limit : next_size);
    }
}

[[nodiscard]] bool make_absolute_path(std::wstring& path) {
    if (is_absolute_path(path)) {
        return true;
    }

    std::vector<wchar_t> buffer(initial_path_capacity);
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(),
            static_cast<DWORD>(buffer.size()),
            buffer.data(),
            nullptr);
        if (length == 0U) {
            return false;
        }
        if (length < buffer.size()) {
            path.assign(buffer.data(), length);
            return true;
        }
        if (length >= path_limit) {
            return false;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1U);
    }
}

[[nodiscard]] std::wstring with_extended_prefix(std::wstring path) {
    if (has_extended_prefix(path)) {
        return path;
    }
    if (path.starts_with(L"\\\\")) {
        return std::wstring{L"\\\\?\\UNC\\"} + path.substr(2U);
    }
    return std::wstring{L"\\\\?\\"} + path;
}

[[nodiscard]] bool sibling_runtime_path(std::wstring& runtime_path) {
    HMODULE facade_module{};
    const auto* const module_address = reinterpret_cast<LPCWSTR>(&cached_api);
    constexpr DWORD module_flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(module_flags, module_address, &facade_module) == FALSE) {
        return false;
    }

    std::wstring facade_path;
    if (!get_path_from_module(facade_module, facade_path) || !make_absolute_path(facade_path)) {
        return false;
    }

    const auto separator = facade_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return false;
    }
    facade_path.resize(separator + 1U);
    facade_path.append(L"inputnext_runtime.dll");
    runtime_path = with_extended_prefix(std::move(facade_path));
    return true;
}

template <typename Address>
[[nodiscard]] LPCWSTR as_module_address(const Address address) noexcept {
    return reinterpret_cast<LPCWSTR>(address);
}

[[nodiscard]] bool callback_addresses_belong_to(const HMODULE provider,
    const input_next_runtime_api_v1* const api) noexcept {
    const std::array<LPCWSTR, 14U> addresses{
        as_module_address(api),
        as_module_address(api->get_state),
        as_module_address(api->set_state),
        as_module_address(api->get_capabilities),
        as_module_address(api->enable),
        as_module_address(api->get_battery_information),
        as_module_address(api->get_keystroke),
        as_module_address(api->get_audio_device_ids),
        as_module_address(api->get_dsound_audio_device_guids),
        as_module_address(api->get_state_ex),
        as_module_address(api->get_capabilities_ex),
        as_module_address(api->wait_for_guide_button),
        as_module_address(api->cancel_guide_button_wait),
        as_module_address(api->power_off_controller),
    };
    constexpr DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    for (const LPCWSTR address : addresses) {
        HMODULE address_module{};
        if (GetModuleHandleExW(flags, address, &address_module) == FALSE ||
            address_module != provider) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const input_next_runtime_api_v1* query_runtime_module(
    const HMODULE runtime_module) noexcept {
    const FARPROC query_address = GetProcAddress(runtime_module, "input_next_runtime_query_api");
    if (query_address == nullptr) {
        return nullptr;
    }

    const auto query = reinterpret_cast<input_next_runtime_query_api_fn>(query_address);
    const auto* const api = query(runtime_abi_version);
    if (!is_valid_input_next_runtime_api_v1(api) ||
        !callback_addresses_belong_to(runtime_module, api)) {
        return nullptr;
    }

    HMODULE pinned_module{};
    constexpr DWORD pin_flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (GetModuleHandleExW(pin_flags, as_module_address(api->get_state), &pinned_module) == FALSE) {
        return nullptr;
    }
    if (pinned_module != runtime_module) {
        static_cast<void>(FreeLibrary(pinned_module));
        return nullptr;
    }
    if (FreeLibrary(pinned_module) == FALSE) {
        return nullptr;
    }
    return api;
}

[[nodiscard]] BOOL CALLBACK resolve_once([[maybe_unused]] PINIT_ONCE once,
    [[maybe_unused]] PVOID parameter,
    [[maybe_unused]] PVOID* context) noexcept {
    try {
        std::wstring runtime_path;
        if (!sibling_runtime_path(runtime_path)) {
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

const input_next_runtime_api_v1* runtime_api() noexcept {
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

} // namespace inputnext::xinput_facade_detail
