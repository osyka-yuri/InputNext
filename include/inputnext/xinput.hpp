#pragma once

#include "device_identity.hpp"
#include "device_metadata.hpp"
#include "error.hpp"
#include "input_binding.hpp"
#include "xinput_types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::focus {
class observer;
}

namespace inputnext::xinput::detail {
struct service_test_access;
}

namespace inputnext::xinput {

struct assignment final {
    std::uint32_t user_index{};
    std::uint64_t id{};
    connection_target target{};

    friend bool operator==(const assignment&, const assignment&) noexcept = default;
};

struct presentation_seed final {
    std::uint64_t next_assignment{1U};
};

class metadata_source {
public:
    virtual ~metadata_source() = default;

    metadata_source(const metadata_source&) = delete;
    metadata_source& operator=(const metadata_source&) = delete;
    metadata_source(metadata_source&&) = delete;
    metadata_source& operator=(metadata_source&&) = delete;

    metadata_source() = default;

    // An available nonzero DirectSound GUID identifies a selected, unambiguous endpoint for this
    // target, independently of whether that endpoint's ID is available.
    [[nodiscard]] virtual metadata_result<metadata_snapshot> query(connection_target target) = 0;
};

// Borrows the observer and metadata source, and retains bindings that borrow their registry.
// Keep those owners alive and join service callers before destroying the service or registry.
class service final {
public:
    service(focus::observer& focus, metadata_source& metadata, presentation_seed seed = {});
    ~service();

    service(const service&) = delete;
    service& operator=(const service&) = delete;
    service(service&&) = delete;
    service& operator=(service&&) = delete;

    // Assignment IDs are scoped to this service instance. Do not pass them to another instance.
    [[nodiscard]] result<assignment> assign(connection_target target, read_binding reader);
    [[nodiscard]] result<void> unassign(assignment expected);
    [[nodiscard]] std::array<std::optional<assignment>, 4U> positions() const;
    void close();
    void enable(BOOL enabled);

    // Packet tracking uses the Guide-inclusive translated state; include_guide only projects the
    // response and does not change the packet number.
    [[nodiscard]] DWORD
    get_state(flavor version, DWORD user_index, XINPUT_STATE* state, bool include_guide = false);
    [[nodiscard]] DWORD get_capabilities(flavor version,
        DWORD user_index,
        DWORD flags,
        XINPUT_CAPABILITIES* capabilities);
    [[nodiscard]] DWORD get_capabilities_ex(flavor version,
        DWORD user_index,
        DWORD flags,
        capabilities_ex* capabilities);
    [[nodiscard]] DWORD get_battery_information(flavor version,
        DWORD user_index,
        BYTE device_type,
        XINPUT_BATTERY_INFORMATION* battery);
    [[nodiscard]] DWORD get_audio_device_ids(flavor version,
        DWORD user_index,
        LPWSTR render_id,
        UINT* render_count,
        LPWSTR capture_id,
        UINT* capture_count,
        std::uint32_t* write_flags = nullptr);
    [[nodiscard]] DWORD get_dsound_audio_device_guids(flavor version,
        DWORD user_index,
        GUID* render_guid,
        GUID* capture_guid);

private:
    friend struct detail::service_test_access;

    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::xinput
