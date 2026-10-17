#pragma once

#include "device_identity.hpp"
#include "input_binding.hpp"
#include "source_lifetime.hpp"
#include <inputnext/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace inputnext {

// Constructor seam for exhaustion boundaries; production uses the default seed.
struct identity_seed final {
    std::uint64_t next_device{1U};
    std::uint64_t next_connection{1U};
};

// The composition root supplies a runtime ID unique across its relevant incarnations.
class device_registry final {
public:
    explicit device_registry(runtime_id runtime, identity_seed seed = {});
    ~device_registry() noexcept;

    device_registry(const device_registry&) = delete;
    device_registry& operator=(const device_registry&) = delete;
    device_registry(device_registry&&) = delete;
    device_registry& operator=(device_registry&&) = delete;

    // Success transfers candidate ownership; every error leaves it with the caller.
    [[nodiscard]] result<admitted_connection> admit(std::unique_ptr<source_lifetime>& candidate);
    // Replacement preserves the device key and assigns a fresh connection identity.
    [[nodiscard]] result<admitted_connection> replace(connection_target expected,
        std::unique_ptr<source_lifetime>& candidate);
    [[nodiscard]] result<void> retire(connection_target target);
    // Revokes all live endpoints and reclaims retired sources that have no pins.
    void retire_all();

    [[nodiscard]] result<read_pin> capture(connection_target target) const;
    [[nodiscard]] result<connection_snapshot> read(connection_target target) const;

    [[nodiscard]] std::size_t drain_retired();
    // Stop new binding calls and join their callers before destroying this registry.
    // Shutdown waits for pins; do not call it while holding a pin needed by the caller.
    void shutdown();

    [[nodiscard]] std::size_t live_count() const noexcept;
    [[nodiscard]] std::size_t resident_count() const noexcept;

private:
    struct storage;
    std::unique_ptr<storage> storage_;
};

} // namespace inputnext
