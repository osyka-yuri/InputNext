#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_raw_output_report.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "owner_identity.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/input_binding.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::gameinput_detail {

struct rumble_capabilities final {
    bool low_frequency{};
    bool high_frequency{};

    friend constexpr bool operator==(const rumble_capabilities&,
        const rumble_capabilities&) noexcept = default;
};

struct output_description final {
    connection_target target{};
    std::uint64_t catalogue_generation{};
    std::optional<rumble_capabilities> capabilities{std::nullopt};
    std::optional<metadata_device_model> model{std::nullopt};
};

class runtime_output_lease final {
public:
    runtime_output_lease() noexcept = default;
    ~runtime_output_lease();

    runtime_output_lease(const runtime_output_lease&) = delete;
    runtime_output_lease& operator=(const runtime_output_lease&) = delete;
    runtime_output_lease(runtime_output_lease&& other) noexcept;
    runtime_output_lease& operator=(runtime_output_lease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return runtime_ != nullptr; }

    [[nodiscard]] connection_target target() const noexcept { return target_; }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>>
    create_rumble_endpoint() const;
    [[nodiscard]] raw_report_result create_raw_output_report(std::uint32_t report_id) const;

private:
    friend class runtime_output_access;

    runtime_output_lease(gameinput_runtime& runtime,
        runtime_owner_identity identity,
        native_device_handle device,
        connection_target target,
        read_pin residency) noexcept;
    void reset() noexcept;

    gameinput_runtime* runtime_{};
    std::size_t slot_index_{};
    std::uint64_t generation_{};
    native_device_handle device_{};
    connection_target target_{};
    read_pin residency_;
};

class runtime_output_access final {
public:
    [[nodiscard]] static gameinput_result<std::unique_ptr<runtime_output_access>> create(
        gameinput_runtime& runtime);
    ~runtime_output_access() noexcept;

    runtime_output_access(const runtime_output_access&) = delete;
    runtime_output_access& operator=(const runtime_output_access&) = delete;
    runtime_output_access(runtime_output_access&&) = delete;
    runtime_output_access& operator=(runtime_output_access&&) = delete;

    [[nodiscard]] gameinput_result<output_description> describe(connection_target target) const;
    [[nodiscard]] metadata_result<bool> qualify_dualsense_usb(connection_target target,
        std::uint64_t expected_catalogue_generation) const;
    [[nodiscard]] gameinput_result<runtime_output_lease> reserve(connection_target target);
    void close_admission() noexcept;

private:
    explicit runtime_output_access(gameinput_runtime& runtime) noexcept : runtime_(&runtime) {}

    gameinput_runtime* runtime_{};
    bool active_{false};
};

} // namespace inputnext::gameinput_detail
