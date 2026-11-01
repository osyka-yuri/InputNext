#include "runtime/bootstrap/query_front.hpp"

#include "ar1_fixture_protocol.hpp"
#include "runtime/abi/xinput/api.hpp"

#include <sal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace inputnext::xinput_dll_fixture {
namespace {

struct ar1_fixture_state final {
    std::atomic<std::uint32_t> pin_attempts;
    std::atomic<std::uint32_t> owner_factory_calls;
    std::atomic<std::uint32_t> stable_api_calls;
    std::atomic<std::uint32_t> dependency_constructions;
    std::atomic<std::uint32_t> graph_constructions;
    std::atomic<std::uint32_t> native_starts;
    std::atomic<std::uint32_t> operator_new_calls;
};

constinit ar1_fixture_state state{};

[[nodiscard]] BOOL reject_pin(void* const context) noexcept {
    auto& counters = *static_cast<ar1_fixture_state*>(context);
    static_cast<void>(counters.pin_attempts.fetch_add(1U, std::memory_order_relaxed));
    return FALSE;
}

[[nodiscard]] inputnext::runtime_detail::bootstrap_owner* create_owner(void* const context) {
    auto& counters = *static_cast<ar1_fixture_state*>(context);
    static_cast<void>(counters.owner_factory_calls.fetch_add(1U, std::memory_order_relaxed));
    static_cast<void>(counters.dependency_constructions.fetch_add(1U, std::memory_order_relaxed));
    static_cast<void>(counters.graph_constructions.fetch_add(1U, std::memory_order_relaxed));
    static_cast<void>(counters.native_starts.fetch_add(1U, std::memory_order_relaxed));
    return nullptr;
}

[[nodiscard]] const input_next_runtime_api_v1* stable_api(void* const context,
    inputnext::runtime_detail::bootstrap_owner* owner) noexcept {
    static_cast<void>(owner);
    auto& counters = *static_cast<ar1_fixture_state*>(context);
    static_cast<void>(counters.stable_api_calls.fetch_add(1U, std::memory_order_relaxed));
    return nullptr;
}

constinit inputnext::runtime_detail::query_front fixture_front{
    inputnext::runtime_detail::query_front_operations{.pin_module = &reject_pin,
        .create_owner = &create_owner,
        .stable_api = &stable_api},
    &state};

[[nodiscard]] ar1_fixture_snapshot snapshot() noexcept {
    return ar1_fixture_snapshot{.pin_attempts = state.pin_attempts.load(std::memory_order_relaxed),
        .owner_factory_calls = state.owner_factory_calls.load(std::memory_order_relaxed),
        .stable_api_calls = state.stable_api_calls.load(std::memory_order_relaxed),
        .dependency_constructions = state.dependency_constructions.load(std::memory_order_relaxed),
        .graph_constructions = state.graph_constructions.load(std::memory_order_relaxed),
        .native_starts = state.native_starts.load(std::memory_order_relaxed),
        .operator_new_calls = state.operator_new_calls.load(std::memory_order_relaxed)};
}

} // namespace
} // namespace inputnext::xinput_dll_fixture

_Use_decl_annotations_ void* operator new(const std::size_t size) {
    using namespace inputnext::xinput_dll_fixture;
    static_cast<void>(state.operator_new_calls.fetch_add(1U, std::memory_order_relaxed));
    if (void* const allocation = std::malloc(size == 0U ? 1U : size); allocation != nullptr) {
        return allocation;
    }
    throw std::bad_alloc{};
}

_Use_decl_annotations_ void* operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* const block) noexcept {
    std::free(block);
}

void operator delete[](void* const block) noexcept {
    std::free(block);
}

void operator delete(void* const block, const std::size_t size) noexcept {
    static_cast<void>(size);
    std::free(block);
}

void operator delete[](void* const block, const std::size_t size) noexcept {
    static_cast<void>(size);
    std::free(block);
}

extern "C" const input_next_runtime_api_v1* __cdecl input_next_runtime_query_api(
    const std::uint32_t requested_abi) noexcept {
    return inputnext::runtime_detail::query_runtime_api(
        inputnext::xinput_dll_fixture::fixture_front,
        requested_abi);
}

extern "C" BOOL __cdecl inputnext_ar1_get_snapshot(
    inputnext::xinput_dll_fixture::ar1_fixture_snapshot* const result) noexcept {
    if (result == nullptr) {
        return FALSE;
    }
    *result = inputnext::xinput_dll_fixture::snapshot();
    return TRUE;
}

extern "C" void __cdecl inputnext_ar1_reset_snapshot() noexcept {
    using namespace inputnext::xinput_dll_fixture;
    state.pin_attempts.store(0U, std::memory_order_relaxed);
    state.owner_factory_calls.store(0U, std::memory_order_relaxed);
    state.stable_api_calls.store(0U, std::memory_order_relaxed);
    state.dependency_constructions.store(0U, std::memory_order_relaxed);
    state.graph_constructions.store(0U, std::memory_order_relaxed);
    state.native_starts.store(0U, std::memory_order_relaxed);
    state.operator_new_calls.store(0U, std::memory_order_relaxed);
}
