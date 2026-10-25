#pragma once

#include <inputnext/output.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace inputnext::output {

inline constexpr std::uint32_t body_pcm_sample_rate = 48000U;
inline constexpr std::uint64_t body_pcm_storage_budget_bytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t body_pcm_buffer_capacity = 64U;

struct body_pcm_input final {
    std::span<const float> samples;
    std::uint64_t frame_count{};
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    float gain{};
};

struct body_pcm_view final {
    std::span<const float> samples;
    std::uint64_t frame_count{};
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    float gain{};
};

struct body_pcm_usage final {
    std::uint64_t bytes{};
    std::uint32_t buffers{};

    friend constexpr bool operator==(const body_pcm_usage&,
        const body_pcm_usage&) noexcept = default;
};

namespace detail {
class body_pcm_preparer_test_access;
class body_pcm_store;
class prepared_body_pcm_storage;
} // namespace detail

class prepared_body_pcm final {
public:
    prepared_body_pcm() noexcept = default;
    prepared_body_pcm(const prepared_body_pcm&) noexcept = default;
    prepared_body_pcm& operator=(const prepared_body_pcm&) noexcept = default;
    prepared_body_pcm(prepared_body_pcm&&) noexcept = default;
    prepared_body_pcm& operator=(prepared_body_pcm&&) noexcept = default;
    ~prepared_body_pcm() = default;

    [[nodiscard]] bool valid() const noexcept;
    // The view borrows sample storage; keep an owning prepared handle alive while using it.
    [[nodiscard]] body_pcm_view view() const noexcept;
    [[nodiscard]] std::uint64_t storage_bytes() const noexcept;

private:
    explicit prepared_body_pcm(
        std::shared_ptr<const detail::prepared_body_pcm_storage> storage) noexcept;

    std::shared_ptr<const detail::prepared_body_pcm_storage> storage_;

    friend class body_pcm_preparer;
};

class body_pcm_preparer final {
public:
    body_pcm_preparer();
    body_pcm_preparer(const body_pcm_preparer&) noexcept = default;
    body_pcm_preparer& operator=(const body_pcm_preparer&) noexcept = default;
    body_pcm_preparer(body_pcm_preparer&& other) noexcept;
    body_pcm_preparer& operator=(body_pcm_preparer&& other) noexcept;
    ~body_pcm_preparer() = default;

    [[nodiscard]] result<prepared_body_pcm> prepare(const body_pcm_input& input) const;
    [[nodiscard]] bool owns(const prepared_body_pcm& prepared) const noexcept;
    [[nodiscard]] body_pcm_usage usage() const;

private:
    explicit body_pcm_preparer(std::shared_ptr<detail::body_pcm_store> store) noexcept;

    std::shared_ptr<detail::body_pcm_store> store_;

    friend class detail::body_pcm_preparer_test_access;
};

} // namespace inputnext::output
