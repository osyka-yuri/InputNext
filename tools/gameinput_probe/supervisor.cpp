#include "worker.hpp"

#include <exception>
#include <ios>

#include <Windows.h>

#include "supervisor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

constexpr DWORD timeout_exit_code = 0xE000'0001U;
constexpr auto cleanup_period = std::chrono::seconds{10};
constexpr DWORD output_budget_per_pass = 65'536U;
constexpr std::size_t output_chunk_size = 4096U;

class unique_handle final {
public:
    explicit unique_handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~unique_handle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

class attribute_list final {
public:
    attribute_list() {
        SIZE_T size{};
        (void)InitializeProcThreadAttributeList(nullptr, 1U, 0U, &size);
        if (size == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "InitializeProcThreadAttributeList(size)");
        }
        storage_.resize(size);
        value_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (InitializeProcThreadAttributeList(value_, 1U, 0U, &size) == FALSE) {
            value_ = nullptr;
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "InitializeProcThreadAttributeList");
        }
    }

    ~attribute_list() {
        if (value_ != nullptr) {
            DeleteProcThreadAttributeList(value_);
        }
    }

    attribute_list(const attribute_list&) = delete;
    attribute_list& operator=(const attribute_list&) = delete;
    attribute_list(attribute_list&&) = delete;
    attribute_list& operator=(attribute_list&&) = delete;

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return value_; }

private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST value_{};
};

struct child_process final {
    unique_handle process;
    unique_handle job;
    bool assigned{};

    child_process() = default;
    child_process(const child_process&) = delete;
    child_process& operator=(const child_process&) = delete;
    child_process(child_process&&) noexcept = default;
    child_process& operator=(child_process&&) noexcept = default;
    ~child_process() = default;
};

struct output_pipe_state final {
    bool closed{};
    bool pending_carriage_return{};
};

[[nodiscard]] std::wstring quote_argument(std::wstring_view value) {
    std::wstring result{L"\""};
    std::size_t backslashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append((backslashes * 2U) + 1U, L'\\');
        } else {
            result.append(backslashes, L'\\');
        }
        result.push_back(character);
        backslashes = 0U;
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] std::wstring command_line(const owned_process_request& request) {
    std::wstring command = quote_argument(request.executable.native());
    for (const auto& argument : request.arguments) {
        command.push_back(L' ');
        command.append(quote_argument(argument));
    }
    return command;
}

[[nodiscard]] unique_handle make_output_pipe(unique_handle& child_write) {
    SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE};
    HANDLE read_handle{};
    HANDLE write_handle{};
    if (CreatePipe(&read_handle, &write_handle, &security, 0U) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreatePipe");
    }

    unique_handle parent_read{read_handle};
    child_write.reset(write_handle);
    if (SetHandleInformation(parent_read.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "SetHandleInformation");
    }
    return parent_read;
}

void spawn_child(const owned_process_request& request, HANDLE output_handle, child_process& child) {
    child.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (child.job.get() == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateJobObjectW");
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(child.job.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "SetInformationJobObject");
    }

    const attribute_list attributes;
    std::array<HANDLE, 1U> inherited_handles{output_handle};
    if (UpdateProcThreadAttribute(attributes.get(),
            0U,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            static_cast<void*>(inherited_handles.data()),
            sizeof(inherited_handles),
            nullptr,
            nullptr) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "UpdateProcThreadAttribute");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = output_handle;
    startup.StartupInfo.hStdOutput = output_handle;
    startup.StartupInfo.hStdError = output_handle;
    startup.lpAttributeList = attributes.get();

    auto command = command_line(request);
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(request.executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
            nullptr,
            request.working_directory.empty() ? nullptr : request.working_directory.c_str(),
            &startup.StartupInfo,
            &process_info) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateProcessW");
    }
    child.process.reset(process_info.hProcess);
    const unique_handle thread{process_info.hThread};

    if (AssignProcessToJobObject(child.job.get(), child.process.get()) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "AssignProcessToJobObject");
    }
    child.assigned = true;

    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "ResumeThread");
    }
}

void flush_pending_carriage_return(std::ostream& output, bool& pending_carriage_return) {
    if (pending_carriage_return) {
        output.put('\r');
        output.flush();
        pending_carriage_return = false;
    }
}

void write_normalized_output(std::ostream& output,
    std::string_view data,
    bool& pending_carriage_return) {
    std::array<char, output_chunk_size + 1U> normalized{};
    std::size_t normalized_size{};
    for (const char value : data) {
        if (pending_carriage_return) {
            if (value == '\n') {
                normalized[normalized_size++] = '\n';
                pending_carriage_return = false;
                continue;
            }
            normalized[normalized_size++] = '\r';
            pending_carriage_return = false;
        }
        if (value == '\r') {
            pending_carriage_return = true;
        } else {
            normalized[normalized_size++] = value;
        }
    }
    if (normalized_size != 0U) {
        output.write(normalized.data(), static_cast<std::streamsize>(normalized_size));
        output.flush();
    }
}

void copy_available_output(HANDLE pipe, std::ostream& output, output_pipe_state& state) {
    DWORD copied{};
    while (!state.closed && copied < output_budget_per_pass) {
        DWORD available{};
        if (PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                state.closed = true;
                flush_pending_carriage_return(output, state.pending_carriage_return);
                return;
            }
            throw std::system_error(static_cast<int>(error),
                std::system_category(),
                "PeekNamedPipe");
        }
        if (available == 0U) {
            return;
        }

        std::array<char, output_chunk_size> buffer{};
        DWORD count{};
        const auto request_size = std::min<DWORD>(
            {available, static_cast<DWORD>(buffer.size()), output_budget_per_pass - copied});
        if (ReadFile(pipe, buffer.data(), request_size, &count, nullptr) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                state.closed = true;
                flush_pending_carriage_return(output, state.pending_carriage_return);
                return;
            }
            throw std::system_error(static_cast<int>(error), std::system_category(), "ReadFile");
        }
        write_normalized_output(output,
            std::string_view{buffer.data(), count},
            state.pending_carriage_return);
        copied += count;
    }
}

[[nodiscard]] DWORD milliseconds_left(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<DWORD>(std::max<std::int64_t>(remaining, 1));
}

[[nodiscard]] bool wait_for_signal(HANDLE handle,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (WaitForSingleObject(handle, 0U) == WAIT_OBJECT_0) {
        return true;
    }
    const DWORD remaining = milliseconds_left(deadline);
    return remaining > 0U && WaitForSingleObject(handle, remaining) == WAIT_OBJECT_0;
}

[[nodiscard]] bool stop_child_tree(child_process& child,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (!child.assigned) {
        (void)TerminateProcess(child.process.get(), 3U);
        return wait_for_signal(child.process.get(), deadline);
    }
    if (wait_for_signal(child.job.get(), std::chrono::steady_clock::now())) {
        return WaitForSingleObject(child.process.get(), 0U) == WAIT_OBJECT_0;
    }
    if (TerminateJobObject(child.job.get(), 3U) == FALSE) {
        if (wait_for_signal(child.job.get(), std::chrono::steady_clock::now())) {
            return WaitForSingleObject(child.process.get(), 0U) == WAIT_OBJECT_0;
        }
        child.job.reset();
        (void)wait_for_signal(child.process.get(), deadline);
        return false;
    }
    if (wait_for_signal(child.job.get(), deadline)) {
        return WaitForSingleObject(child.process.get(), 0U) == WAIT_OBJECT_0;
    }

    // Closing the last job handle applies KILL_ON_JOB_CLOSE if the termination wait expires.
    child.job.reset();
    (void)wait_for_signal(child.process.get(), deadline);
    return false;
}

[[nodiscard]] bool drain_output(HANDLE pipe,
    std::ostream& output,
    output_pipe_state& state,
    std::chrono::steady_clock::time_point deadline) {
    while (!state.closed && std::chrono::steady_clock::now() < deadline) {
        copy_available_output(pipe, output, state);
        if (!state.closed) {
            Sleep(5U);
        }
    }
    return state.closed;
}

[[nodiscard]] std::filesystem::path current_executable_path() {
    std::vector<wchar_t> buffer(256U);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "GetModuleFileNameW");
        }
        if (length < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), length}};
        }
        if (buffer.size() >= 32'768U) {
            throw std::length_error("The executable path exceeds the Windows path limit.");
        }
        buffer.resize(buffer.size() * 2U);
    }
}

[[nodiscard]] std::vector<std::wstring> worker_arguments(const probe_options& options) {
    std::vector<std::wstring> arguments{L"--inputnext-worker",
        L"--duration-ms",
        std::to_wstring(options.duration_ms)};
    if (options.require_device) {
        arguments.emplace_back(L"--require-device");
    }
    if (options.require_reading) {
        arguments.emplace_back(L"--require-reading");
    }
    if (options.require_history) {
        arguments.emplace_back(L"--require-history");
    } else if (options.history) {
        arguments.emplace_back(L"--history");
    }
    return arguments;
}

[[nodiscard]] owned_process_result supervise_child(child_process& child,
    HANDLE output_pipe,
    std::chrono::milliseconds timeout,
    process_streams streams) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    output_pipe_state output_state{};
    while (true) {
        copy_available_output(output_pipe, streams.output, output_state);
        if (std::chrono::steady_clock::now() >= deadline) {
            const auto cleanup_deadline = std::chrono::steady_clock::now() + cleanup_period;
            const bool tree_stopped = stop_child_tree(child, cleanup_deadline);
            const bool pipe_closed =
                drain_output(output_pipe, streams.output, output_state, cleanup_deadline);
            streams.errors << "child=timeout termination_confirmed=" << tree_stopped
                           << " output_closed=" << pipe_closed << '\n';
            return {.timed_out = true,
                .exit_code = timeout_exit_code,
                .cleanup_confirmed = tree_stopped && pipe_closed};
        }

        const DWORD wait = WaitForSingleObject(child.process.get(), 25U);
        if (wait == WAIT_FAILED) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "WaitForSingleObject");
        }
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }

    const auto cleanup_deadline = std::chrono::steady_clock::now() + cleanup_period;
    const bool tree_stopped = wait_for_signal(child.job.get(), std::chrono::steady_clock::now()) ||
                              stop_child_tree(child, cleanup_deadline);
    const bool process_signaled = WaitForSingleObject(child.process.get(), 0U) == WAIT_OBJECT_0;
    const bool pipe_closed =
        drain_output(output_pipe, streams.output, output_state, cleanup_deadline);
    if (!tree_stopped || !process_signaled || !pipe_closed) {
        streams.errors << "child=exited tree_signaled=" << tree_stopped
                       << " process_signaled=" << process_signaled
                       << " output_closed=" << pipe_closed << '\n';
        return {.timed_out = false, .exit_code = 6U, .cleanup_confirmed = false};
    }

    DWORD exit_code{};
    if (GetExitCodeProcess(child.process.get(), &exit_code) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "GetExitCodeProcess");
    }
    return {.timed_out = false, .exit_code = exit_code, .cleanup_confirmed = true};
}

} // namespace

owned_process_result run_owned_process(const owned_process_request& request,
    process_streams streams) {
    unique_handle child_write;
    child_process child;
    try {
        if (!request.executable.is_absolute() || request.timeout.count() <= 0 ||
            request.timeout > std::chrono::minutes{10}) {
            throw std::invalid_argument(
                "Child executable must be absolute and timeout must be within ten minutes.");
        }

        const auto parent_read = make_output_pipe(child_write);
        spawn_child(request, child_write.get(), child);
        child_write.reset();
        return supervise_child(child, parent_read.get(), request.timeout, streams);
    } catch (const std::exception& error) {
        const bool cleanup_confirmed =
            child.process.get() == nullptr ||
            stop_child_tree(child, std::chrono::steady_clock::now() + cleanup_period);
        streams.errors << "child=supervisor_failed cleanup_confirmed=" << cleanup_confirmed
                       << " message=" << error.what() << '\n';
        return {.timed_out = false, .exit_code = 6U, .cleanup_confirmed = cleanup_confirmed};
    }
}

supervised_result run_supervised_probe(const probe_options& options, process_streams streams) {
    try {
        const auto executable = current_executable_path();
        const owned_process_request request{
            .executable = executable,
            .working_directory = executable.parent_path(),
            .arguments = worker_arguments(options),
            .timeout = std::chrono::milliseconds{options.duration_ms} + cleanup_period,
        };
        const auto child = run_owned_process(request, streams);
        if (child.timed_out) {
            streams.errors << "probe=timeout cleanup_confirmed=" << child.cleanup_confirmed
                           << " drain=unverified\n";
            return {5};
        }
        if (child.exit_code > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            streams.errors << "probe=child_failed exit_code=" << child.exit_code << '\n';
            return {6};
        }
        return {static_cast<int>(child.exit_code)};
    } catch (const std::exception& error) {
        streams.errors << "probe=supervisor_failed message=" << error.what() << '\n';
        return {6};
    }
}

} // namespace inputnext::gameinput_detail
