#pragma once

#include "dinput/native_types.hpp"
#include "output/composition/internal_producer.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::output::detail {
class composition;
class window_acquisition;
} // namespace inputnext::output::detail

namespace inputnext::dinput::force_feedback {

enum class mutation_policy : std::uint8_t { revoke_on_failure, preserve_on_failure };

class contribution final {
public:
    contribution() noexcept = default;
    ~contribution() = default;
    contribution(const contribution&) = delete;
    contribution& operator=(const contribution&) = delete;
    contribution(contribution&&) = delete;
    contribution& operator=(contribution&&) = delete;

    [[nodiscard]] HRESULT open(output::detail::composition& composition,
        std::shared_ptr<const output::detail::window_acquisition> token,
        output::backend_handle backend,
        bool& drive_requested) noexcept;
    [[nodiscard]] HRESULT submit(output::rumble_value value,
        std::chrono::steady_clock::time_point now,
        bool& drive_requested,
        std::chrono::steady_clock::time_point boundary,
        mutation_policy policy = mutation_policy::revoke_on_failure) noexcept;
    [[nodiscard]] HRESULT clear(bool& drive_requested,
        mutation_policy policy = mutation_policy::revoke_on_failure) noexcept;
    [[nodiscard]] HRESULT close(bool& drive_requested) noexcept;
    void retire(bool& drive_requested) noexcept;

    [[nodiscard]] const std::shared_ptr<const output::detail::window_acquisition>&
    token() const noexcept;
    [[nodiscard]] bool matches(
        const std::shared_ptr<const output::detail::window_acquisition>& token) const noexcept;
    [[nodiscard]] bool occupied() const noexcept;
    [[nodiscard]] bool closing() const noexcept;
    [[nodiscard]] output::context_identity identity() const noexcept;

private:
    struct publication final {
        output::rumble_value value{};
        std::chrono::steady_clock::time_point expires_at;
    };

    enum class phase : std::uint8_t { vacant, open, closing };

    [[nodiscard]] HRESULT progress_close(bool& drive_requested) noexcept;
    void reset() noexcept;

    std::shared_ptr<const output::detail::window_acquisition> token_;
    std::optional<output::detail::internal_producer> producer_;
    std::optional<output::internal_context> context_;
    std::optional<output::close_receipt> receipt_;
    std::optional<publication> publication_;
    output::channel_revisions revisions_{};
    std::uint64_t sequence_{1U};
    std::uint64_t close_request_id_{};
    phase phase_{phase::vacant};
};

} // namespace inputnext::dinput::force_feedback
