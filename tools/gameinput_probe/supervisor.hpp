#pragma once

#include "worker.hpp"

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace inputnext::gameinput_detail {

struct supervised_result final {
    int exit_code{};
};

struct owned_process_request final {
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::vector<std::wstring> arguments;
    std::chrono::milliseconds timeout{};
};

struct owned_process_result final {
    bool timed_out{};
    unsigned long exit_code{};
    bool cleanup_confirmed{};
};

struct process_streams final {
    std::ostream& output;
    std::ostream& errors;
};

[[nodiscard]] supervised_result run_supervised_probe(const probe_options& options,
    process_streams streams);
[[nodiscard]] owned_process_result run_owned_process(const owned_process_request& request,
    process_streams streams);

} // namespace inputnext::gameinput_detail
