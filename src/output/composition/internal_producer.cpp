#include "internal_producer.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <cstdint>

namespace inputnext::output::detail {

internal_producer::internal_producer(authority& owner, internal_context_options options) noexcept
    : owner_(owner),
      options_(options) {
}

result<internal_context> internal_producer::open(connection_target target) {
    return owner_.open_internal(target, options_);
}

result<mutation_receipt> internal_producer::submit(internal_context context,
    const submit_request& request) {
    return owner_.submit_internal(context, request, options_);
}

result<mutation_receipt> internal_producer::clear(internal_context context,
    const clear_request& request) {
    return owner_.clear_internal(context, request, options_);
}

result<close_receipt> internal_producer::close(internal_context context,
    std::uint64_t close_request_id) {
    return owner_.close_internal(context, close_request_id, options_);
}

result<void> internal_producer::acknowledge(const close_receipt& receipt) {
    return owner_.acknowledge_internal(receipt, options_);
}

} // namespace inputnext::output::detail
