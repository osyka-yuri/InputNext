#include "body_pcm_internal.hpp"

#include <inputnext/output_body_pcm.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace inputnext::output::detail {

namespace {

class heap_pcm_memory final : public pcm_memory_port {
public:
    [[nodiscard]] float* allocate(const std::size_t sample_count) override {
        try {
            return new (std::nothrow) float[sample_count];
        } catch (const std::bad_array_new_length&) {
            return nullptr;
        }
    }

    void deallocate(float* const samples) noexcept override { delete[] samples; }
};

} // namespace

body_pcm_store::body_pcm_store(std::shared_ptr<pcm_memory_port> memory)
    : memory_(std::move(memory)) {
    if (!memory_) {
        throw std::invalid_argument{"body PCM store requires a memory port"};
    }
}

bool body_pcm_store::reserve(const std::uint64_t bytes) {
    std::scoped_lock lock{mutex_};
    if (buffers_ >= body_pcm_buffer_capacity || bytes_ > body_pcm_storage_budget_bytes ||
        bytes > body_pcm_storage_budget_bytes - bytes_) {
        return false;
    }
    bytes_ += bytes;
    ++buffers_;
    return true;
}

void body_pcm_store::release(const std::uint64_t bytes) noexcept {
    std::scoped_lock lock{mutex_};
    if (buffers_ == 0U || bytes > bytes_) {
        std::terminate();
    }
    bytes_ -= bytes;
    --buffers_;
}

body_pcm_usage body_pcm_store::usage() const {
    std::scoped_lock lock{mutex_};
    return body_pcm_usage{.bytes = bytes_, .buffers = buffers_};
}

pcm_memory_port& body_pcm_store::memory() const noexcept {
    return *memory_;
}

body_pcm_preparer body_pcm_preparer_test_access::make_preparer(
    std::shared_ptr<pcm_memory_port> memory) {
    return body_pcm_preparer{std::make_shared<body_pcm_store>(std::move(memory))};
}

std::shared_ptr<body_pcm_store> make_default_body_pcm_store() {
    return std::make_shared<body_pcm_store>(std::make_shared<heap_pcm_memory>());
}

} // namespace inputnext::output::detail
