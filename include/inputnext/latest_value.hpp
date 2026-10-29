#pragma once

#include "error.hpp"
#include "input_sample.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace inputnext {

class latest_value final {
public:
    explicit latest_value(std::uint64_t initial_revision = 0) noexcept;
    ~latest_value() = default;

    latest_value(const latest_value&) = delete;
    latest_value& operator=(const latest_value&) = delete;
    latest_value(latest_value&&) = delete;
    latest_value& operator=(latest_value&&) = delete;

    [[nodiscard]] result<std::uint64_t> publish(const input_update& update);
    [[nodiscard]] result<published_sample> read() const;
    [[nodiscard]] bool is_closed() const noexcept;
    void close();

private:
    mutable std::mutex mutex_;
    std::atomic<bool> closed_{false};
    bool has_sample_{};
    std::uint64_t revision_{};
    published_sample sample_{};
};

} // namespace inputnext
