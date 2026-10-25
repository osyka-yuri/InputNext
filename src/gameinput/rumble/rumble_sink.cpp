#include "rumble_sink.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "gameinput/runtime/output_access.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>
#include <memory>
#include <new>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

class gameinput_rumble_sink final : public output::native_sink {
public:
    gameinput_rumble_sink(runtime_output_lease lease,
        std::unique_ptr<native_rumble_endpoint> endpoint) noexcept
        : lease_(std::move(lease)),
          endpoint_(std::move(endpoint)) {}

    [[nodiscard]] output::native_result write(
        const output::native_command& command) noexcept override {
        if (!lease_.valid() || endpoint_ == nullptr || command.backend.target != lease_.target()) {
            return output::native_result{.status = output::native_status::disconnected};
        }
        if (command.output.channels != output::rumble_mask) {
            return output::native_result{.status = output::native_status::unsupported};
        }
        if (command.output.body.has_value() &&
            command.output.body->route == output::body_actuator_route::hd) {
            return output::native_result{.status = output::native_status::unsupported};
        }
        return endpoint_->set(command.output.values.rumble);
    }

private:
    runtime_output_lease lease_;
    std::unique_ptr<native_rumble_endpoint> endpoint_;
};

} // namespace

native_result<std::unique_ptr<output::native_sink>> make_rumble_sink(runtime_output_lease lease) {
    if (!lease.valid()) {
        return std::unexpected(native_failure{.cause = native_failure_cause::native_failure});
    }

    auto endpoint = lease.create_rumble_endpoint();
    if (!endpoint.has_value()) {
        return std::unexpected(endpoint.error());
    }

    try {
        return std::unique_ptr<output::native_sink>{
            new gameinput_rumble_sink{std::move(lease), std::move(*endpoint)}};
    } catch (const std::bad_alloc&) {
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    }
}

} // namespace inputnext::gameinput_detail
