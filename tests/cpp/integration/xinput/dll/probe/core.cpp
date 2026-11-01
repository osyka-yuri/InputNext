#include "support.hpp"

#include "runtime/abi/xinput/api.hpp"
#include <input_next_core.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace inputnext::xinput_dll_probe {

int run_core_query(const std::filesystem::path& bundle) {
    require(runtime_module_count() == 0U, "Core query scenario started with a loaded runtime");
    module_handle runtime = load_library(bundle / L"inputnext_runtime.dll");
    require(runtime.get() != nullptr, "could not load the Core provider");

    const auto query = load_named_function<pfn_input_next_runtime_query_core_api>(runtime.get(),
        "input_next_runtime_query_core_api");
    const auto ordinal_query =
        load_ordinal_function<pfn_input_next_runtime_query_core_api>(runtime.get(), 3U);
    require(query == ordinal_query, "Core name and ordinal select different entry points");
    require(query(0U) == nullptr, "Core accepted an unsupported ABI");
    const auto* const core = query(INPUT_NEXT_CORE_ABI_VERSION);
    if (core == nullptr) {
        throw std::runtime_error{"Core returned a null table"};
    }
    require(core->struct_size == sizeof(*core) && core->abi_version == INPUT_NEXT_CORE_ABI_VERSION,
        "Core returned an invalid table prefix");
    require(core->get_runtime_info != nullptr && core->enumerate_devices != nullptr &&
                core->resolve_target != nullptr && core->read_latest != nullptr &&
                core->open_reader != nullptr && core->read_events != nullptr &&
                core->close_reader != nullptr,
        "Core returned an incomplete table");

    const auto xinput_query =
        load_ordinal_function<input_next_runtime_query_api_fn>(runtime.get(), 1U);
    require(is_valid_input_next_runtime_api_v1(xinput_query(1U)),
        "Core and XInput tables are not available together");
    require(GetModuleHandleW(L"GameInputRedist.dll") == nullptr,
        "querying API tables started the native provider");
    require(query(INPUT_NEXT_CORE_ABI_VERSION) == core, "Core table is not immutable");

    auto const* const retained = runtime.get();
    require(runtime.close(), "could not release the Core caller's module reference");
    require(GetModuleHandleW(L"inputnext_runtime.dll") == retained,
        "Core provider was not process retained");
    require(query(INPUT_NEXT_CORE_ABI_VERSION) == core, "Core table did not survive caller unload");
    require(runtime_module_count() == 1U, "Core query created multiple runtime modules");
    std::cout << "core_query_lifetime=passed\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
