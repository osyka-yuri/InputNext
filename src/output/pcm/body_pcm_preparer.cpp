#include "body_pcm_internal.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace inputnext::output {

namespace {

class reservation final {
public:
    reservation(std::shared_ptr<detail::body_pcm_store> store, const std::uint64_t bytes) noexcept
        : store_(std::move(store)),
          bytes_(bytes) {}

    reservation(const reservation&) = delete;
    reservation& operator=(const reservation&) = delete;
    reservation(reservation&&) = delete;
    reservation& operator=(reservation&&) = delete;

    ~reservation() noexcept {
        if (active_) {
            store_->release(bytes_);
        }
    }

    void dismiss() noexcept { active_ = false; }

private:
    std::shared_ptr<detail::body_pcm_store> store_;
    std::uint64_t bytes_{};
    bool active_{true};
};

class sample_allocation final {
public:
    explicit sample_allocation(detail::pcm_memory_port& memory) noexcept : memory_(memory) {}

    sample_allocation(const sample_allocation&) = delete;
    sample_allocation& operator=(const sample_allocation&) = delete;
    sample_allocation(sample_allocation&&) = delete;
    sample_allocation& operator=(sample_allocation&&) = delete;

    ~sample_allocation() noexcept {
        if (samples_ != nullptr) {
            memory_.deallocate(samples_);
        }
    }

    [[nodiscard]] float* allocate(const std::size_t sample_count) {
        samples_ = memory_.allocate(sample_count);
        return samples_;
    }

    void release() noexcept { samples_ = nullptr; }

private:
    detail::pcm_memory_port& memory_;
    float* samples_{};
};

} // namespace

body_pcm_preparer::body_pcm_preparer() : store_{detail::make_default_body_pcm_store()} {
}

body_pcm_preparer::body_pcm_preparer(std::shared_ptr<detail::body_pcm_store> store) noexcept
    : store_(std::move(store)) {
}

body_pcm_preparer::body_pcm_preparer(body_pcm_preparer&&) noexcept = default;

body_pcm_preparer& body_pcm_preparer::operator=(body_pcm_preparer&&) noexcept = default;

result<prepared_body_pcm> body_pcm_preparer::prepare(const body_pcm_input& input) const {
    if (store_ == nullptr) {
        return std::unexpected{error::resources};
    }

    const result<detail::pcm_descriptor> descriptor = detail::validate_pcm_descriptor(input);
    if (!descriptor.has_value()) {
        return std::unexpected{descriptor.error()};
    }
    if (descriptor->storage_bytes > body_pcm_storage_budget_bytes ||
        !store_->reserve(descriptor->storage_bytes)) {
        return std::unexpected{error::capacity_exceeded};
    }

    reservation held_reservation{store_, descriptor->storage_bytes};
    sample_allocation allocation{store_->memory()};
    try {
        float* const copied_samples = allocation.allocate(descriptor->sample_count);
        if (copied_samples == nullptr) {
            return std::unexpected{error::resources};
        }

        std::copy_n(input.samples.data(), descriptor->sample_count, copied_samples);
        const result<void> copied_samples_validation = detail::validate_pcm_samples(
            std::span<const float>{copied_samples, descriptor->sample_count});
        if (!copied_samples_validation.has_value()) {
            return std::unexpected{copied_samples_validation.error()};
        }

        auto storage = std::make_shared<detail::prepared_body_pcm_storage>(store_,
            copied_samples,
            detail::pcm_storage_metadata{.sample_count = descriptor->sample_count,
                .frame_count = input.frame_count,
                .channels = input.channels,
                .sample_rate = input.sample_rate,
                .gain = input.gain,
                .bytes = descriptor->storage_bytes});
        allocation.release();
        held_reservation.dismiss();
        return prepared_body_pcm{std::move(storage)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{error::resources};
    }
}

bool body_pcm_preparer::owns(const prepared_body_pcm& prepared) const noexcept {
    return store_ != nullptr && prepared.storage_ != nullptr &&
           prepared.storage_->store() == store_.get();
}

body_pcm_usage body_pcm_preparer::usage() const {
    return store_ == nullptr ? body_pcm_usage{} : store_->usage();
}

} // namespace inputnext::output
