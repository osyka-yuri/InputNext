#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <memory>

namespace inputnext::output {

class authority;
class native_sink;

namespace detail {
struct executor_test_access;
}

// Owns the bounded native-output workers for one authority. The authority must outlive the
// executor. shutdown() waits for every admitted native call and sink destructor to return.
class executor final {
public:
    [[nodiscard]] static execution_result<std::unique_ptr<executor>> create(authority& owner);
    ~executor() noexcept;

    executor(const executor&) = delete;
    executor& operator=(const executor&) = delete;
    executor(executor&&) = delete;
    executor& operator=(executor&&) = delete;

    [[nodiscard]] execution_result<backend_handle> register_backend(connection_target target,
        std::unique_ptr<native_sink>& candidate,
        channel_mask supported_channels,
        body_route_support route_support = body_route_support::classic_only);
    [[nodiscard]] execution_result<backend_handle> replace_backend(backend_handle expected,
        connection_target target,
        std::unique_ptr<native_sink>& candidate,
        channel_mask supported_channels,
        body_route_support route_support = body_route_support::classic_only);

    [[nodiscard]] execution_result<void> request_retry(backend_handle backend);
    void request_drive() noexcept;
    [[nodiscard]] execution_result<retirement_receipt> retire(backend_handle backend);
    [[nodiscard]] execution_result<backend_progress> progress(backend_handle backend) const;
    [[nodiscard]] execution_result<bool> qualifies_route(
        const body_route_ack& acknowledgement) const;
    [[nodiscard]] execution_result<void> acknowledge_retirement(const retirement_receipt& receipt);

    void shutdown() noexcept;

private:
    friend struct detail::executor_test_access;
    struct implementation;

    explicit executor(std::unique_ptr<implementation> implementation) noexcept;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::output
