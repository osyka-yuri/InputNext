#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace inputnext::output::detail {

class pcm_memory_port {
public:
    pcm_memory_port() = default;
    virtual ~pcm_memory_port() = default;
    pcm_memory_port(const pcm_memory_port&) = delete;
    pcm_memory_port& operator=(const pcm_memory_port&) = delete;
    pcm_memory_port(pcm_memory_port&&) = delete;
    pcm_memory_port& operator=(pcm_memory_port&&) = delete;

    [[nodiscard]] virtual float* allocate(std::size_t sample_count) = 0;
    virtual void deallocate(float* samples) noexcept = 0;
};

struct pcm_descriptor final {
    std::size_t sample_count{};
    std::uint64_t storage_bytes{};
};

[[nodiscard]] result<pcm_descriptor> validate_pcm_descriptor(const body_pcm_input& input) noexcept;
[[nodiscard]] result<void> validate_pcm_samples(std::span<const float> samples) noexcept;

struct pcm_storage_metadata final {
    std::size_t sample_count{};
    std::uint64_t frame_count{};
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    float gain{};
    std::uint64_t bytes{};
};

class body_pcm_store final {
public:
    explicit body_pcm_store(std::shared_ptr<pcm_memory_port> memory);
    ~body_pcm_store() = default;
    body_pcm_store(const body_pcm_store&) = delete;
    body_pcm_store& operator=(const body_pcm_store&) = delete;
    body_pcm_store(body_pcm_store&&) = delete;
    body_pcm_store& operator=(body_pcm_store&&) = delete;

    [[nodiscard]] bool reserve(std::uint64_t bytes);
    void release(std::uint64_t bytes) noexcept;
    [[nodiscard]] body_pcm_usage usage() const;
    [[nodiscard]] pcm_memory_port& memory() const noexcept;

private:
    std::shared_ptr<pcm_memory_port> memory_;
    mutable std::mutex mutex_;
    std::uint64_t bytes_{};
    std::uint32_t buffers_{};
};

class prepared_body_pcm_storage final {
public:
    prepared_body_pcm_storage(std::shared_ptr<body_pcm_store> store,
        float* samples,
        pcm_storage_metadata metadata) noexcept;
    ~prepared_body_pcm_storage() noexcept;
    prepared_body_pcm_storage(const prepared_body_pcm_storage&) = delete;
    prepared_body_pcm_storage& operator=(const prepared_body_pcm_storage&) = delete;
    prepared_body_pcm_storage(prepared_body_pcm_storage&&) = delete;
    prepared_body_pcm_storage& operator=(prepared_body_pcm_storage&&) = delete;

    [[nodiscard]] body_pcm_view view() const noexcept;
    [[nodiscard]] std::uint64_t storage_bytes() const noexcept;
    [[nodiscard]] const body_pcm_store* store() const noexcept;

private:
    std::shared_ptr<body_pcm_store> store_;
    float* samples_{};
    pcm_storage_metadata metadata_{};
};

class body_pcm_preparer_test_access final {
public:
    [[nodiscard]] static body_pcm_preparer make_preparer(std::shared_ptr<pcm_memory_port> memory);
};

[[nodiscard]] std::shared_ptr<body_pcm_store> make_default_body_pcm_store();

} // namespace inputnext::output::detail
