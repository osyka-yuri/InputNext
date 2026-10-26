#pragma once

#include "audio/native_port.hpp"
#include "audio/native_types.hpp"
#include "detail.hpp"

#include <inputnext/gameinput_runtime.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::audio_detail {

struct native_worker_state final {
    bool sta_attempted{};
    bool has_resource{};
    native_resource_key resource{};
    std::uint64_t generation{};
    std::uint32_t buffer_size{};
};

enum class identity_refresh : std::uint8_t {
    refreshed,
    retry_metadata,
};

void record_error(audio_slot& slot, native_error error) noexcept;
void wake_control(audio_slot& slot) noexcept;
void fail_worker(audio_slot& slot,
    bool retry,
    std::optional<native_error> error = std::nullopt) noexcept;

[[nodiscard]] native_result<identity_refresh> qualify_fresh_identity(native_port& port,
    gameinput_runtime& runtime,
    audio_slot& slot,
    const native_resource_key& resource,
    std::uint64_t expected_generation);

[[nodiscard]] bool prepare_native_worker(gameinput_runtime& runtime,
    native_port& port,
    audio_slot& slot,
    native_worker_state& state) noexcept;

} // namespace inputnext::audio_detail
