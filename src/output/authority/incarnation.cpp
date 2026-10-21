#include "incarnation.hpp"

#include <inputnext/output.hpp>

#include <objbase.h>

#include <bit>
#include <stdexcept>

namespace inputnext::output::detail {

authority_incarnation create_authority_incarnation(const incarnation_creation creation) {
    if (creation == incarnation_creation::force_failure_for_test) {
        throw std::runtime_error{"output authority incarnation generation failed"};
    }

    GUID value{};
    if (CoCreateGuid(&value) != S_OK) {
        throw std::runtime_error{"output authority incarnation generation failed"};
    }

    authority_incarnation incarnation;
    incarnation.bytes = std::bit_cast<decltype(incarnation.bytes)>(value);
    if (!incarnation.valid()) {
        throw std::runtime_error{"output authority incarnation generation failed"};
    }
    return incarnation;
}

} // namespace inputnext::output::detail
