#pragma once

#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace inputnext::xinput {

struct service::implementation final {
    struct position_record final {
        std::optional<assignment> occupant = std::nullopt;
        read_binding reader;
        bool packet_initialized{};
        XINPUT_GAMEPAD canonical_gamepad{};
        std::uint32_t packet_number{};
        std::uint64_t committed_revision{};
    };

    struct prepared_state_read final {
        assignment occupant;
        read_binding reader;
        focus::snapshot focus_word{};
        bool eligible_at_capture{};
        bool include_guide{};
        XINPUT_GAMEPAD canonical_gamepad{};
        std::uint64_t sample_revision{};
        std::optional<read_pin> sample_pin;
    };

    implementation(focus::observer& focus_observer,
        metadata_source& metadata_provider,
        presentation_seed seed);

    [[nodiscard]] DWORD
    prepare_state_read(DWORD user_index, bool include_guide, prepared_state_read& prepared);
    [[nodiscard]] DWORD commit_state_read(const prepared_state_read& prepared,
        XINPUT_STATE* output);

    focus::observer& focus;
    metadata_source& metadata;
    std::array<position_record, 4U> positions{};
    std::uint64_t next_assignment{};
    bool assignment_exhausted{};
    bool enabled{true};
    bool closed{};
};

} // namespace inputnext::xinput
