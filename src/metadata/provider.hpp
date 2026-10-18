#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>

#include <cstdint>

namespace inputnext {

struct metadata_request final {
    connection_target target{};
    std::uint64_t catalogue_generation{};
    native_device_facts facts{};
};

// Blocking caller-side provider. Callers must invoke query outside runtime/registry locks.
class metadata_provider {
public:
    metadata_provider(const metadata_provider&) = delete;
    metadata_provider& operator=(const metadata_provider&) = delete;
    metadata_provider(metadata_provider&&) = delete;
    metadata_provider& operator=(metadata_provider&&) = delete;
    virtual ~metadata_provider() = default;
    [[nodiscard]] virtual metadata_result<metadata_snapshot> query(
        const metadata_request& request) = 0;

protected:
    metadata_provider() = default;
};

} // namespace inputnext
