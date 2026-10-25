#pragma once

#include <inputnext/output_authority.hpp>

namespace inputnext::output::detail {

// Declare before control locks so collection runs after those locks are released.
class body_pcm_collection_on_exit final {
public:
    explicit body_pcm_collection_on_exit(authority& owner) noexcept : owner_(owner) {}

    body_pcm_collection_on_exit(const body_pcm_collection_on_exit&) = delete;
    body_pcm_collection_on_exit& operator=(const body_pcm_collection_on_exit&) = delete;
    body_pcm_collection_on_exit(body_pcm_collection_on_exit&&) = delete;
    body_pcm_collection_on_exit& operator=(body_pcm_collection_on_exit&&) = delete;

    ~body_pcm_collection_on_exit() noexcept { owner_.collect_body_pcm(); }

private:
    authority& owner_;
};

} // namespace inputnext::output::detail
