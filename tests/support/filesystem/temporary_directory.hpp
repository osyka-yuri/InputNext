#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace inputnext::test {

class temporary_directory final {
public:
    temporary_directory()
        : base_(std::filesystem::canonical(std::filesystem::temp_directory_path())) {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            const auto sequence = sequence_.fetch_add(1U, std::memory_order_relaxed);
            const auto candidate = base_ / ("inputnext-test-" + std::to_string(timestamp) + '-' +
                                               std::to_string(sequence));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error{"Could not create test directory",
                    candidate,
                    error};
            }
        }
    }

    ~temporary_directory() noexcept {
        try {
            std::error_code error;
            const auto resolved = std::filesystem::weakly_canonical(path_, error);
            if (!error && resolved == path_ && resolved.parent_path() == base_) {
                std::filesystem::remove_all(path_, error);
            }
        } catch (...) {
            static_cast<void>(
                std::fputs("Test directory cleanup could not be completed.\n", stderr));
        }
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;
    temporary_directory(temporary_directory&&) = delete;
    temporary_directory& operator=(temporary_directory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    inline static std::atomic<std::uint64_t> sequence_{};
    const std::filesystem::path base_;
    std::filesystem::path path_;
};

} // namespace inputnext::test
