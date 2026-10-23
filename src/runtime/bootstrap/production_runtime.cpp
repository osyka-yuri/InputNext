#include "bootstrap_owner.hpp"

#include "query_front.hpp"
#include "runtime/abi/runtime_api.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/session/native_session.hpp"
#include <inputnext/focus.hpp>

#include <atomic>
#include <memory>

namespace inputnext::runtime_detail {
namespace {

extern query_front runtime_front;

[[nodiscard]] std::unique_ptr<native_session> prepare_production_session(
    [[maybe_unused]] void* context) {
    return native_session::prepare();
}

BOOL pin_production_module([[maybe_unused]] void* context) noexcept {
    HMODULE module{};
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    return GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&runtime_front), &module);
}

bootstrap_owner* create_production_owner(void* const context) {
    return new bootstrap_owner{focus::make_current_process_source(),
        &prepare_production_session,
        context};
}

const input_next_runtime_api_v1* stable_production_api([[maybe_unused]] void* context,
    bootstrap_owner* const owner) noexcept {
    return owner != nullptr ? runtime_api_v1() : nullptr;
}

constinit query_front runtime_front{query_front_operations{.pin_module = &pin_production_module,
                                        .create_owner = &create_production_owner,
                                        .stable_api = &stable_production_api},
    nullptr};

} // namespace

query_front& runtime_query_front() noexcept {
    return runtime_front;
}

bootstrap_owner* runtime_bootstrap_owner() noexcept {
    return runtime_front.owner.load(std::memory_order_acquire);
}

} // namespace inputnext::runtime_detail
