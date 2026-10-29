#include <inputnext/latest_value.hpp>

#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>

namespace inputnext {

static_assert(std::atomic<bool>::is_always_lock_free,
    "Sample closure observation requires lock-free atomics");

latest_value::latest_value(std::uint64_t initial_revision) noexcept : revision_(initial_revision) {
}

result<std::uint64_t> latest_value::publish(const input_update& update) {
    std::scoped_lock lock{mutex_};

    if (closed_.load(std::memory_order_seq_cst)) {
        return std::unexpected{error::not_connected};
    }
    if (update.observation_qpc == 0U || !is_valid_gamepad_state(update.state)) {
        return std::unexpected{error::invalid_argument};
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected{error::revision_exhausted};
    }

    const auto next_revision = revision_ + 1U;
    const published_sample next_sample{
        .state = update.state,
        .source_timestamp_us = update.source_timestamp_us,
        .observation_qpc = update.observation_qpc,
        .revision = next_revision,
    };

    sample_ = next_sample;
    revision_ = next_revision;
    has_sample_ = true;
    return next_revision;
}

result<published_sample> latest_value::read() const {
    std::scoped_lock lock{mutex_};

    if (closed_.load(std::memory_order_seq_cst)) {
        return std::unexpected{error::not_connected};
    }
    if (!has_sample_) {
        return std::unexpected{error::no_data};
    }
    return sample_;
}

bool latest_value::is_closed() const noexcept {
    return closed_.load(std::memory_order_seq_cst);
}

void latest_value::close() {
    std::scoped_lock lock{mutex_};
    closed_.store(true, std::memory_order_seq_cst);
}

} // namespace inputnext
