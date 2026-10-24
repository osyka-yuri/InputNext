#include "profiles/parser.hpp"

#include "values/required_value.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

using namespace inputnext::profiles;
using inputnext::test::required_value;

namespace {

[[nodiscard]] inputnext::output::trigger_value left(const parsed_profile& parsed) {
    REQUIRE(parsed.settings.left_trigger.has_value());
    return required_value(parsed.settings.left_trigger);
}

[[nodiscard]] inputnext::output::trigger_value right(const parsed_profile& parsed) {
    REQUIRE(parsed.settings.right_trigger.has_value());
    return required_value(parsed.settings.right_trigger);
}

[[nodiscard]] bool has_diagnostic(const parsed_profile& parsed, const std::string_view text) {
    return std::ranges::any_of(parsed.diagnostics,
        [text](const diagnostic& value) { return value.message.contains(text); });
}

} // namespace

TEST_CASE("profiles.empty_and_absent_triggers_have_no_intent", "[unit][profiles]") {
    auto parsed = parse_profile("title = \"unrelated\"\n[game]\nname = \"ignored\"\n");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->settings.left_trigger.has_value());
    CHECK_FALSE(parsed->settings.right_trigger.has_value());
    CHECK(parsed->diagnostics.empty());

    parsed = parse_profile("[effects.left_trigger]\nmode = 'off'\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->settings.left_trigger.has_value());
    CHECK(required_value(parsed->settings.left_trigger) == inputnext::output::trigger_value{});
    CHECK(parsed->diagnostics.empty());

    parsed = parse_profile("[effects.left_trigger]\n[effects.right_trigger] # empty\n");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->settings.left_trigger.has_value());
    CHECK_FALSE(parsed->settings.right_trigger.has_value());
    CHECK(parsed->diagnostics.empty());
}

TEST_CASE("profiles.scalar_modes_decode_defaults_and_float_storage", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "feedback"
start = 0.2
strength = 0.6

[effects.right_trigger]
mode = "weapon"
start = 0.25
end = 0.75
strength = 0.8
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed).mode == inputnext::output::trigger_mode::feedback);
    CHECK(left(*parsed).layout == inputnext::output::trigger_layout::uniform);
    CHECK(left(*parsed).start == 0.2F);
    CHECK(left(*parsed).end == 1.0F);
    CHECK(left(*parsed).strength == 0.6F);
    CHECK(right(*parsed).mode == inputnext::output::trigger_mode::weapon);
    CHECK(right(*parsed).start == 0.25F);
    CHECK(right(*parsed).end == 0.75F);
    CHECK(right(*parsed).strength == 0.8F);

    parsed = parse_profile("[effects.left_trigger]\nmode = 'feedback'\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed).start == 0.0F);
    CHECK(left(*parsed).end == 1.0F);
    CHECK(left(*parsed).strength == 0.5F);
}

TEST_CASE("profiles.vibration_accepts_integral_float_frequency", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "vibration"
frequency = 120.0

[effects.right_trigger]
mode = "vibration"
frequency = 255
start = 0.4
strength = 0.75
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed).mode == inputnext::output::trigger_mode::vibration);
    CHECK(left(*parsed).frequency == 120.0F);
    CHECK(left(*parsed).start == 0.0F);
    CHECK(left(*parsed).strength == 0.5F);
    CHECK(right(*parsed).frequency == 255.0F);
}

TEST_CASE("profiles.curve_and_vibration_curve_decode_ten_integer_zones", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "curve"
strengths = [0, 0, 1, 2, 3, 5, 6, 7, 8, 8]

[effects.right_trigger]
mode = "vibration_curve"
amplitudes = [0, 1, 2, 3, 4, 5, 6, 7, 8, 8]
frequency = 120
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed).mode == inputnext::output::trigger_mode::feedback);
    CHECK(left(*parsed).layout == inputnext::output::trigger_layout::per_zone);
    CHECK(left(*parsed).zones ==
          std::array<std::uint8_t, 10U>{0U, 0U, 1U, 2U, 3U, 5U, 6U, 7U, 8U, 8U});
    CHECK(right(*parsed).mode == inputnext::output::trigger_mode::vibration);
    CHECK(right(*parsed).layout == inputnext::output::trigger_layout::per_zone);
    CHECK(right(*parsed).frequency == 120.0F);
    CHECK(right(*parsed).zones ==
          std::array<std::uint8_t, 10U>{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 8U});
}

TEST_CASE("profiles.slope_quantizes_endpoints_before_interpolating", "[unit][profiles]") {
    SECTION("ascending") {
        auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "slope"
start = 0.2
end = 0.8
start_strength = 0.1
end_strength = 1.0
)");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->diagnostics.empty());
        CHECK(left(*parsed).zones ==
              std::array<std::uint8_t, 10U>{0U, 0U, 1U, 2U, 3U, 5U, 6U, 7U, 8U, 8U});
    }

    SECTION("descending") {
        auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "slope"
start = 0.2
end = 0.8
start_strength = 1.0
end_strength = 0.1
)");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->diagnostics.empty());
        CHECK(left(*parsed).zones ==
              std::array<std::uint8_t, 10U>{0U, 0U, 8U, 7U, 6U, 5U, 3U, 2U, 1U, 1U});
    }

    SECTION("both boundaries in one zone") {
        auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "slope"
start = 0.21
end = 0.29
start_strength = 0.5
end_strength = 0.9
)");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->diagnostics.empty());
        CHECK(left(*parsed).zones ==
              std::array<std::uint8_t, 10U>{0U, 0U, 4U, 7U, 7U, 7U, 7U, 7U, 7U, 7U});
    }
}

TEST_CASE("profiles.zero_quantized_effects_are_canonical_off", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "feedback"
strength = 0.01

[effects.right_trigger]
mode = "curve"
strengths = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(right(*parsed) == inputnext::output::trigger_value{});

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "slope"
start = 0.2
end = 0.8
start_strength = 0
end_strength = 0

[effects.right_trigger]
mode = "vibration_curve"
amplitudes = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
frequency = 120
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(right(*parsed) == inputnext::output::trigger_value{});

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "vibration"
frequency = 1
strength = 0.01
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
}

TEST_CASE("profiles.weapon_enforces_quantized_hardware_ranges", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "weapon"
start = 0.25
end = 0.75
strength = 0.125
)");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    CHECK(left(*parsed).mode == inputnext::output::trigger_mode::weapon);

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "weapon"
start = 0.1
end = 0.75
)");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "hardware zone"));

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "weapon"
start = 0.25
end = 0.26
)");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK_FALSE(parsed->diagnostics.empty());

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "weapon"
start = 0.25
end = 1.0
)");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "hardware zone"));

    parsed = parse_profile(R"(
[effects.left_trigger]
mode = "weapon"
start = 0.25
end = 0.75
strength = 0.01
)");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "strength must quantize above zero"));
}

TEST_CASE("profiles.weapon_boundary_matches_usb_double_quantization", "[unit][profiles]") {
    const auto parsed = parse_profile(
        "[effects.left_trigger]\nmode = 'weapon'\nstart = 0.8333333134651184\nend = 0.9\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->settings.left_trigger.has_value());
    CHECK(required_value(parsed->settings.left_trigger).mode ==
          inputnext::output::trigger_mode::weapon);
    CHECK(parsed->diagnostics.empty());
}

TEST_CASE("profiles.semantic_errors_disable_only_the_affected_trigger", "[unit][profiles]") {
    auto parsed = parse_profile(R"(
[effects.left_trigger]
mode = "feedback"
strength = true

[effects.right_trigger]
mode = "vibration"
frequency = 120
)");
    REQUIRE(parsed.has_value());
    REQUIRE_FALSE(parsed->diagnostics.empty());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(right(*parsed).mode == inputnext::output::trigger_mode::vibration);
    CHECK(parsed->diagnostics.front().line == 4U);
    CHECK(parsed->diagnostics.front().column > 0U);

    parsed = parse_profile("[effects.left_trigger]\nmode = 2\n");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "mode"));
}

TEST_CASE("profiles.unknown_and_mode_inappropriate_fields_are_diagnosed", "[unit][profiles]") {
    auto parsed =
        parse_profile("[effects.right_trigger]\nmode = 'off'\nfrequency = 120\nextra = true\n");
    REQUIRE(parsed.has_value());
    CHECK(right(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "unexpected field 'frequency'"));
    CHECK(has_diagnostic(*parsed, "unknown field 'extra'"));

    parsed = parse_profile("[effects.left_trigger]\nmode = 'unknown'\n");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "unknown mode"));

    parsed = parse_profile("[effects.left_trigger]\nstart = 0.4\n");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(has_diagnostic(*parsed, "require 'mode'"));

    parsed = parse_profile("[effects]\nrumble = true\n");
    REQUIRE(parsed.has_value());
    CHECK(has_diagnostic(*parsed, "Unknown effect name"));
}

TEST_CASE("profiles.strict_types_ranges_and_array_shape_are_checked", "[unit][profiles]") {
    const std::array<std::string_view, 12U> invalid_documents{
        "[effects.left_trigger]\nmode = 'feedback'\nstart = '0.2'\n",
        "[effects.left_trigger]\nmode = 'feedback'\nstart = 1.1\n",
        "[effects.left_trigger]\nmode = 'vibration'\nfrequency = 120.5\n",
        "[effects.left_trigger]\nmode = 'vibration'\nfrequency = 0\n",
        "[effects.left_trigger]\nmode = 'vibration'\nfrequency = 256\n",
        "[effects.left_trigger]\nmode = 'vibration'\nfrequency = true\n",
        "[effects.left_trigger]\nmode = 'curve'\nstrengths = [0, 1, 2]\n",
        "[effects.left_trigger]\nmode = 'curve'\nstrengths = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n",
        "[effects.left_trigger]\nmode = 'curve'\nstrengths = [0, 1, 2, 3, 4, 5, 6, 7, 8, 1.0]\n",
        "[effects.left_trigger]\nmode = 'feedback'\nend = 0.5\n",
        "[effects.left_trigger]\nmode = 'slope'\nstart = 0.8\nend = 0.2\nstart_strength = "
        "0\nend_strength = 1\n",
        "[effects.left_trigger]\nmode = 'slope'\nstart = 0.2\nend = 0.8\nstart_strength = 0\n",
    };

    for (const auto document : invalid_documents) {
        auto parsed = parse_profile(document);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed->diagnostics.empty());
        CHECK(left(*parsed) == inputnext::output::trigger_value{});
    }
}

TEST_CASE("profiles.full_toml_syntax_and_utf8_outside_trigger_scope_are_accepted",
    "[unit][profiles]") {
    auto parsed = parse_profile(R"(
title = "控制器 # one"
published = 2026-10-28T12:30:00Z
description = """first line
second line"""
matrix = [[1, 2], [3, 4]]
settings = { enabled = true, label = 'literal # value' }

[effects.left_trigger]
mode = 'feedback'
start = 1e-1
strength = 0.5
)");
    REQUIRE(parsed.has_value());
    CHECK(parsed->diagnostics.empty());
    CHECK(left(*parsed).mode == inputnext::output::trigger_mode::feedback);
    CHECK(left(*parsed).start == 0.1F);
}

TEST_CASE("profiles.duplicate_keys_and_malformed_toml_are_document_errors", "[unit][profiles]") {
    auto parsed = parse_profile("[effects.left_trigger]\nmode = 'feedback'\nmode = 'off'\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().line > 0U);
    CHECK(parsed.error().column > 0U);
    CHECK_FALSE(parsed.error().message.empty());

    parsed = parse_profile("[effects.left_trigger]\nmode = [\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().line > 0U);
    CHECK(parsed.error().column > 0U);

    parsed = parse_profile("[effects.left_trigger]\nmode = 'off'\n[effects.left_trigger]\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().line > 0U);
    CHECK(parsed.error().column > 0U);
}

TEST_CASE("profiles.in_memory_documents_obey_the_shared_byte_limit", "[unit][profiles]") {
    std::string maximum_document{"#"};
    maximum_document.resize(maximum_profile_bytes, ' ');
    auto parsed = parse_profile(maximum_document);
    REQUIRE(parsed.has_value());
    CHECK(parsed->diagnostics.empty());

    maximum_document.push_back(' ');
    parsed = parse_profile(maximum_document);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.contains("64 KiB"));
}

TEST_CASE("profiles.non_table_effect_nodes_are_located_and_neutralized", "[unit][profiles]") {
    auto parsed = parse_profile("effects = true\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.size() == 1U);
    CHECK(parsed->settings.left_trigger == inputnext::output::trigger_value{});
    CHECK(parsed->settings.right_trigger == inputnext::output::trigger_value{});
    CHECK(parsed->diagnostics.front().line == 1U);
    CHECK(parsed->diagnostics.front().column > 0U);

    parsed = parse_profile("effects.left_trigger = true\n");
    REQUIRE(parsed.has_value());
    CHECK(left(*parsed) == inputnext::output::trigger_value{});
    CHECK(parsed->diagnostics.front().line == 1U);
}
