#pragma once

#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>

namespace inputnext::gameinput_detail {

class native_raw_output_report {
public:
    virtual ~native_raw_output_report() noexcept = default;

    native_raw_output_report(const native_raw_output_report&) = delete;
    native_raw_output_report& operator=(const native_raw_output_report&) = delete;
    native_raw_output_report(native_raw_output_report&&) = delete;
    native_raw_output_report& operator=(native_raw_output_report&&) = delete;

    // Exact size required by the owned raw report buffer.
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    // Successful filling replaces the complete payload without resizing the buffer.
    [[nodiscard]] virtual bool set_data(std::span<const std::byte> data) noexcept = 0;
    [[nodiscard]] virtual output::native_result send() noexcept = 0;

protected:
    native_raw_output_report() = default;
};

using raw_report_result =
    std::expected<std::unique_ptr<native_raw_output_report>, output::native_result>;

} // namespace inputnext::gameinput_detail
