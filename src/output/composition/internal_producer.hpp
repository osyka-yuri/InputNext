#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <cstdint>

namespace inputnext::output::detail {

class composition;

// Borrows its composition-owned authority. Copies share the grant and do not extend owner lifetime.
class internal_producer final {
public:
    ~internal_producer() noexcept = default;

    internal_producer(const internal_producer&) noexcept = default;
    internal_producer(internal_producer&&) noexcept = default;
    internal_producer& operator=(const internal_producer&) = delete;
    internal_producer& operator=(internal_producer&&) = delete;

    [[nodiscard]] result<internal_context> open(connection_target target);
    [[nodiscard]] result<mutation_receipt> submit(internal_context context,
        const submit_request& request);
    [[nodiscard]] result<mutation_receipt> clear(internal_context context,
        const clear_request& request);
    [[nodiscard]] result<close_receipt> close(internal_context context,
        std::uint64_t close_request_id);
    [[nodiscard]] result<void> acknowledge(const close_receipt& receipt);

private:
    friend class composition;
    internal_producer(authority& owner, internal_context_options options) noexcept;

    authority& owner_;
    const internal_context_options options_;
};

} // namespace inputnext::output::detail
