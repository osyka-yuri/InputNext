#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/bootstrap/query_front.hpp"

namespace inputnext::runtime_detail {
namespace {

constinit query_front unused_front{query_front_operations{}, nullptr};

} // namespace

query_front& runtime_query_front() noexcept {
    return unused_front;
}

bootstrap_owner* runtime_bootstrap_owner() noexcept {
    return nullptr;
}

} // namespace inputnext::runtime_detail
