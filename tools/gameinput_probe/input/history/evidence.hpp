#pragma once

#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>

namespace inputnext::gameinput_detail {

struct history_evidence final {
    std::uint64_t readings{};
    std::uint64_t native_readings{};
    std::uint64_t baselines{};
    std::uint64_t resets{};
    std::uint64_t failures{};
    bool native_baseline{};
    bool native_progress{};

    void observe(const history_batch& batch) noexcept {
        for (std::size_t index = 0U; index < batch.count; ++index) {
            const auto& record = batch.records[index];
            if (record.baseline) {
                ++baselines;
                native_baseline =
                    native_baseline || batch.source == history_source_kind::native_history;
            } else {
                ++readings;
                if (record.origin == history_record_origin::native_pad) {
                    ++native_readings;
                    native_progress = true;
                }
            }
        }
        native_progress = native_progress || (batch.source == history_source_kind::native_history &&
                                                 batch.caught_up && batch.checkpoint_qpc != 0U);
        if (batch.reset == history_reset::history_gap) {
            ++resets;
        }
        if (batch.failure.has_value()) {
            ++failures;
        }
    }

    [[nodiscard]] bool native_history_observed() const noexcept {
        return native_baseline && native_progress;
    }
};

} // namespace inputnext::gameinput_detail
