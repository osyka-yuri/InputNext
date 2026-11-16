#include "internal_producer.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_submission.hpp>

#include <cstdint>
#include <utility>

namespace inputnext::output::detail {

internal_producer::internal_producer(authority& owner, internal_grant grant) noexcept
    : owner_(owner),
      grant_(std::move(grant)) {
}

result<internal_context> internal_producer::open(connection_target target) {
    return owner_.open_internal(target, grant_);
}

result<mutation_receipt> internal_producer::submit(internal_context context,
    const submit_request& request) {
    return owner_.submit_internal(context, request, grant_);
}

result<mutation_receipt> internal_producer::submit(internal_context context,
    const submit_request& request,
    const finite_submission_window& window) {
    return owner_.submit_internal(context, request, grant_, &window);
}

result<mutation_receipt> internal_producer::clear(internal_context context,
    const clear_request& request) {
    return owner_.clear_internal(context, request, grant_);
}

result<close_receipt> internal_producer::close(internal_context context,
    std::uint64_t close_request_id) {
    return owner_.close_internal(context, close_request_id, grant_);
}

result<void> internal_producer::acknowledge(const close_receipt& receipt) {
    return owner_.acknowledge_internal(receipt, grant_);
}

void internal_producer::retire_window_context(context_identity identity) noexcept {
    owner_.retire_window_context(identity, grant_);
}

} // namespace inputnext::output::detail
