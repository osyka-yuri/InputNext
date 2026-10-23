#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/latest_value.hpp>
#include <inputnext/source_lifetime.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::detail {

enum class connection_phase : std::uint8_t {
    free,
    reserved,
    live,
    retired,
    draining,
};

struct connection_record final {
    std::mutex gate;
    connection_target target{};
    std::atomic<std::size_t> pin_count{0U};
    std::atomic<connection_phase> phase{connection_phase::free};
    // Reclamation may reset target, latest, and source only after retirement with no pins.
    std::optional<latest_value> latest;
    std::unique_ptr<source_lifetime> source;

    // SC orders release against retirement so shutdown cannot miss both zero and retired.
    [[nodiscard]] bool release_pin() noexcept {
        const auto previous = pin_count.fetch_sub(1U, std::memory_order_seq_cst);
        assert(previous > 0U);
        return previous == 1U;
    }

    // Postzero notification touches only these atomics; registry storage must outlive the release
    // call.
    void notify_pin_release() noexcept {
        if (phase.load(std::memory_order_seq_cst) == connection_phase::retired) {
            pin_count.notify_all();
        }
    }

    // Shutdown closes admission and holds lifecycle across this wait, so
    // the count cannot be reused.
    void wait_for_pins(std::size_t observed) const noexcept {
        while (observed != 0U) {
            pin_count.wait(observed, std::memory_order_seq_cst);
            observed = pin_count.load(std::memory_order_seq_cst);
        }
    }
};

} // namespace inputnext::detail
