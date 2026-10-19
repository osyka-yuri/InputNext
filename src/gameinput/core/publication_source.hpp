#pragma once

#include "gameinput/history/history_capture.hpp"
#include "gameinput/reading_parts.hpp"
#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/source_lifetime.hpp>

#include <cstdint>
#include <mutex>
#include <optional>

namespace inputnext::gameinput_detail {

struct seed_ticket final {
    std::uint64_t pad_epoch{};
    std::uint64_t guide_epoch{};
};

// Narrow constructor seam for exercising terminal epoch exhaustion.
struct publication_epoch_seed final {
    std::uint64_t pad_epoch{};
    std::uint64_t guide_epoch{};
};

enum class publication_outcome : std::uint8_t {
    published,
    partial_only,
    stale_seed,
    invalid,
    closed,
    revision_exhausted,
    epoch_exhausted,
};

enum class publication_fault : std::uint8_t {
    none,
    pad_epoch_exhausted,
    guide_epoch_exhausted,
};

enum class bind_outcome : std::uint8_t {
    bound,
    already_bound,
    closed,
};

struct source_status final {
    bool closed{};
    bool pad_known{};
    bool guide_known{};
    publication_fault fault{publication_fault::none};
    std::uint64_t pad_epoch{};
    std::uint64_t guide_epoch{};
};

// Portable latest-state publication for the GameInput source. The registry owns this object
// after admission; its producer binding borrows that registry's stable record storage.
class publication_source final : public source_lifetime {
public:
    explicit publication_source(publication_epoch_seed seed = {}) noexcept;

    [[nodiscard]] bind_outcome bind(producer_binding binding);
    [[nodiscard]] seed_ticket before_seed() const;
    [[nodiscard]] publication_outcome on_callback(const reading_parts& parts);
    [[nodiscard]] publication_outcome on_seed(seed_ticket ticket, const reading_parts& parts);
    [[nodiscard]] history_capture& history() noexcept;
    void close();
    [[nodiscard]] source_status status() const;

private:
    [[nodiscard]] publication_outcome
    apply(const reading_parts& parts, seed_ticket ticket, bool is_seed);
    [[nodiscard]] bool advance_callback_epochs(bool accept_pad, bool accept_guide) noexcept;
    void commit_lanes(const reading_parts& parts, bool accept_pad, bool accept_guide) noexcept;
    [[nodiscard]] publication_outcome
    publish_selected(const reading_parts& parts, bool accept_pad, bool accept_guide, bool is_seed);
    [[nodiscard]] publication_outcome map_publication_failure(error failure) noexcept;

    mutable std::mutex mutex_;
    std::optional<producer_binding> producer_;
    history_capture history_;
    gamepad_state pad_state_{};
    bool closed_{};
    bool pad_known_{};
    bool guide_known_{};
    bool guide_pressed_{};
    publication_fault fault_{publication_fault::none};
    std::uint64_t pad_epoch_{};
    std::uint64_t guide_epoch_{};
};

} // namespace inputnext::gameinput_detail
