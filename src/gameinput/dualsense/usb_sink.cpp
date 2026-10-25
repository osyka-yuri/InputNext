#include "usb_sink.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_raw_output_report.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "usb_report.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <utility>

namespace inputnext::gameinput_detail::dualsense {
namespace {

constexpr std::uint32_t dualsense_usb_output_report_id = 0x02U;

class dualsense_usb_sink final : public output::native_sink {
public:
    explicit dualsense_usb_sink(runtime_output_lease lease) noexcept : lease_(std::move(lease)) {}

    [[nodiscard]] output::native_result write(
        const output::native_command& command) noexcept override {
        if (!lease_.valid() || command.backend.target != lease_.target()) {
            return output::native_result{.status = output::native_status::disconnected};
        }
        if (command.output.channels != output::supported_channel_mask) {
            return output::native_result{.status = output::native_status::unsupported};
        }
        if (command.output.body.has_value() &&
            command.output.body->route == output::body_actuator_route::hd) {
            return output::native_result{.status = output::native_status::unsupported};
        }

        const auto image = encode_usb_report(command.output.values);
        if (report_ == nullptr) {
            auto created = lease_.create_raw_output_report(dualsense_usb_output_report_id);
            if (!created.has_value()) {
                return created.error();
            }
            if (!*created) {
                return output::native_result{.status = output::native_status::retryable_failure};
            }
            report_ = std::move(*created);
        }

        const auto payload = usb_report_payload(image, report_->capacity());
        if (!payload.has_value()) {
            return output::native_result{.status = output::native_status::unsupported};
        }
        if (!report_->set_data(*payload)) {
            return output::native_result{.status = output::native_status::retryable_failure};
        }
        return report_->send();
    }

private:
    runtime_output_lease lease_;
    std::unique_ptr<native_raw_output_report> report_;
};

} // namespace

native_result<std::unique_ptr<output::native_sink>> make_dualsense_usb_sink(
    runtime_output_lease lease) {
    if (!lease.valid()) {
        return std::unexpected(native_failure{.cause = native_failure_cause::native_failure});
    }

    try {
        return std::unique_ptr<output::native_sink>{new dualsense_usb_sink{std::move(lease)}};
    } catch (const std::bad_alloc&) {
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    }
}

} // namespace inputnext::gameinput_detail::dualsense
