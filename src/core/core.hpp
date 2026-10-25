#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

namespace inputnext {
class device_registry;
class gameinput_runtime;

namespace focus {
class observer;
}

namespace xinput {
class service;
}
} // namespace inputnext

namespace inputnext::core {

namespace detail {
struct service_test_access;
}

inline constexpr std::size_t device_capacity = 16U;
inline constexpr std::size_t reader_capacity = 28U;
inline constexpr std::uint32_t left_trigger_channel = 0x01U;
inline constexpr std::uint32_t right_trigger_channel = 0x02U;
inline constexpr std::uint32_t rumble_channel = 0x04U;
inline constexpr std::uint32_t led_channel = 0x08U;

enum class status : std::uint8_t {
    ok = 0U,
    invalid_argument = 1U,
    unavailable = 2U,
    stale_target = 3U,
    busy = 7U,
    buffer_too_small = 9U,
    capacity_exceeded = 10U,
    stale_reader = 12U,
    source_retired = 14U,
    io_failed = 22U,
    no_data = 26U,
    ambiguous_target = 27U,
    not_admitted = 28U,
};

template <typename Value>
using result = std::expected<Value, status>;

enum class identity_quality : std::uint8_t { none, platform, ambiguous };
enum class selector_kind : std::uint8_t { xinput_slot = 1U, exact_target, hardware_fingerprint };
enum class reader_focus : std::uint8_t { foreground_process, background };

struct reader_handle final {
    runtime_id runtime{};
    std::uint64_t id{};
    friend constexpr bool operator==(const reader_handle&, const reader_handle&) noexcept = default;
};

struct runtime_info final {
    runtime_id runtime{};
    std::uint64_t qpc_frequency{};
};

struct device_record final {
    connection_target target{};
    std::uint32_t output_channels{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t revision{};
    physical_transport transport{physical_transport::unknown};
    identity_quality identity{identity_quality::none};
    std::array<std::uint8_t, 32U> hardware_fingerprint{};
    friend constexpr bool operator==(const device_record&, const device_record&) noexcept = default;
};

struct device_catalogue final {
    std::array<device_record, device_capacity> entries{};
    std::size_t count{};
    std::uint64_t revision{};
};

struct selector final {
    selector_kind kind{selector_kind::xinput_slot};
    std::uint32_t slot{};
    connection_target target{};
    std::array<std::uint8_t, 32U> hardware_fingerprint{};
};

struct reader_result final {
    status outcome{status::unavailable};
    history_batch batch{};
};

class service final {
public:
    service(runtime_id identity,
        std::uint64_t qpc_frequency,
        device_registry& registry,
        gameinput_runtime& runtime,
        focus::observer& focus,
        xinput::service& presentation);
    ~service() noexcept;
    service(const service&) = delete;
    service& operator=(const service&) = delete;
    service(service&&) = delete;
    service& operator=(service&&) = delete;

    [[nodiscard]] runtime_info info() const noexcept;
    [[nodiscard]] result<device_catalogue> enumerate();
    [[nodiscard]] result<connection_target> resolve(const selector& selection);
    [[nodiscard]] result<connection_snapshot> read_latest(connection_target target) const;
    [[nodiscard]] result<reader_handle> open_reader(connection_target target, reader_focus focus);
    [[nodiscard]] reader_result read_events(reader_handle reader, std::size_t capacity);
    [[nodiscard]] status close_reader(reader_handle reader);
    void close_admission() noexcept;

private:
    friend struct detail::service_test_access;

    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::core
