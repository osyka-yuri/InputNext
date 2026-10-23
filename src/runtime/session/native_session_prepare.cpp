#include "native_session_state.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "gameinput/runtime/factory.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "metadata/windows/provider.hpp"
#include "native_session.hpp"
#include "native_session_dependencies.hpp"
#include "output/composition/composition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/output_executor.hpp>

#include <Windows.h>
#include <objbase.h>
#include <profileapi.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <algorithm>

#include <cstddef>

#include <filesystem>

#include <string>

#include <vector>

namespace inputnext::runtime_detail {
namespace {

constexpr auto gameinput_filename = L"GameInputRedist.dll";
constexpr std::size_t module_path_limit = 32768U;

[[nodiscard]] runtime_id create_runtime_identity() {
    GUID value{};
    if (CoCreateGuid(&value) != S_OK) {
        throw std::runtime_error{"native session runtime identity generation failed"};
    }

    runtime_id identity{};
    identity.bytes = std::bit_cast<decltype(identity.bytes)>(value);
    if (!identity.valid()) {
        throw std::runtime_error{"native session runtime identity is invalid"};
    }
    return identity;
}

[[nodiscard]] std::filesystem::path sibling_gameinput_path() {
    HMODULE module{};
    const auto* const address = reinterpret_cast<LPCWSTR>(&sibling_gameinput_path);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            address,
            &module) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "GetModuleHandleExW");
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(MAX_PATH));
    for (;;) {
        const auto length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "GetModuleFileNameW");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            const std::filesystem::path module_path{
                std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
            return module_path.parent_path() / gameinput_filename;
        }
        if (buffer.size() >= module_path_limit) {
            throw std::length_error{"native session module path exceeds Windows path limit"};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2U, module_path_limit));
    }
}

[[nodiscard]] std::uint64_t query_qpc_frequency() {
    LARGE_INTEGER value{};
    if (QueryPerformanceFrequency(&value) == FALSE || value.QuadPart <= 0) {
        const auto error = GetLastError();
        throw std::system_error(
            static_cast<int>(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error),
            std::system_category(),
            "QueryPerformanceFrequency");
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] std::thread
launch_worker([[maybe_unused]] void* launcher_context, const worker_entry entry, void* context) {
    return std::thread{entry, context};
}

[[noreturn]] void throw_preparation_failure(const char* message) {
    throw std::runtime_error{message};
}

} // namespace

native_session::implementation::implementation(const runtime_id runtime_identity)
    : identity(runtime_identity),
      registry(runtime_identity) {
}

native_session::implementation::~implementation() noexcept {
    shutdown();
}

std::unique_ptr<native_session> native_session::implementation::prepare(
    native_session_dependencies dependencies) {
    if (!dependencies.identity.valid() || !dependencies.runtime_library.is_absolute() ||
        dependencies.native == nullptr || dependencies.metadata == nullptr ||
        dependencies.focus_source == nullptr) {
        throw std::invalid_argument{"native session preparation requires valid owned ports"};
    }

    auto state = std::make_unique<implementation>(dependencies.identity);
    state->qpc_frequency = query_qpc_frequency();
    state->runtime = gameinput_detail::runtime_factory::create(state->registry,
        gameinput_options{.runtime_library = std::move(dependencies.runtime_library)},
        std::move(dependencies.native),
        std::move(dependencies.metadata));

    auto runtime_access = gameinput_detail::runtime_output_access::create(*state->runtime);
    if (!runtime_access.has_value()) {
        throw_preparation_failure("native session output access activation failed");
    }
    state->runtime_access = std::move(*runtime_access);

    state->focus_observer =
        std::make_unique<focus::observer>(state->identity, std::move(dependencies.focus_source));
    state->output_composition = std::make_unique<output::detail::composition>(state->identity,
        state->registry,
        *state->focus_observer);

    auto executor = output::executor::create(state->output_composition->public_authority());
    if (!executor.has_value()) {
        throw_preparation_failure("native session output executor creation failed");
    }
    state->output_executor = std::move(*executor);

    auto rumble = state->output_composition->make_xinput_rumble();
    if (!rumble.has_value()) {
        throw_preparation_failure("native session XInput output bridge creation failed");
    }
    state->presentation_service = std::make_unique<gameinput_xinput>(*state->runtime,
        *state->focus_observer,
        state->qpc_frequency,
        std::move(*rumble));

    auto connections =
        gameinput_detail::gameinput_output_connections::create(*state->runtime_access,
            *state->output_executor);
    if (!connections.has_value()) {
        throw_preparation_failure("native session output connections creation failed");
    }
    state->connections = std::move(*connections);

    auto owner = std::unique_ptr<native_session>{new native_session{std::move(state)}};
    auto& prepared = *owner->implementation_;
    prepared.launch(dependencies.launch_worker != nullptr ? dependencies.launch_worker
                                                          : &launch_worker,
        dependencies.launch_context);
    return owner;
}

std::unique_ptr<native_session> native_session::prepare() {
    native_session_dependencies options{};
    options.identity = create_runtime_identity();
    options.runtime_library = sibling_gameinput_path();
    options.native = gameinput_detail::make_native_gameinput();
    options.metadata = metadata_detail::make_windows_metadata_provider();
    options.focus_source = focus::make_current_process_source();
    return prepare(std::move(options));
}

std::unique_ptr<native_session> native_session::prepare(native_session_dependencies dependencies) {
    return implementation::prepare(std::move(dependencies));
}

} // namespace inputnext::runtime_detail
