#include "body_pcm_internal.hpp"

#include <inputnext/output_body_pcm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace inputnext::output::detail {

prepared_body_pcm_storage::prepared_body_pcm_storage(std::shared_ptr<body_pcm_store> store,
    float* const samples,
    const pcm_storage_metadata metadata) noexcept
    : store_(std::move(store)),
      samples_(samples),
      metadata_(metadata) {
}

prepared_body_pcm_storage::~prepared_body_pcm_storage() noexcept {
    store_->memory().deallocate(samples_);
    store_->release(metadata_.bytes);
}

body_pcm_view prepared_body_pcm_storage::view() const noexcept {
    return body_pcm_view{.samples = std::span<const float>{samples_, metadata_.sample_count},
        .frame_count = metadata_.frame_count,
        .channels = metadata_.channels,
        .sample_rate = metadata_.sample_rate,
        .gain = metadata_.gain};
}

std::uint64_t prepared_body_pcm_storage::storage_bytes() const noexcept {
    return metadata_.bytes;
}

const body_pcm_store* prepared_body_pcm_storage::store() const noexcept {
    return store_.get();
}

} // namespace inputnext::output::detail

namespace inputnext::output {

prepared_body_pcm::prepared_body_pcm(
    std::shared_ptr<const detail::prepared_body_pcm_storage> storage) noexcept
    : storage_(std::move(storage)) {
}

bool prepared_body_pcm::valid() const noexcept {
    return static_cast<bool>(storage_);
}

body_pcm_view prepared_body_pcm::view() const noexcept {
    return storage_ == nullptr ? body_pcm_view{} : storage_->view();
}

std::uint64_t prepared_body_pcm::storage_bytes() const noexcept {
    return storage_ == nullptr ? 0U : storage_->storage_bytes();
}

} // namespace inputnext::output
