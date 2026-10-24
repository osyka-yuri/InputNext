#include "profiles/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

namespace {

[[nodiscard]] std::string dotted_key(const std::size_t tables) {
    std::string text;
    for (std::size_t index = 0U; index < tables; ++index) {
        text += "a.";
    }
    return text + "value = 1\n";
}

} // namespace

TEST_CASE("profiles.parser_limits_implicit_tables_before_building_a_deep_tree",
    "[profiles][unit][limits]") {
    REQUIRE(inputnext::profiles::parse_profile(dotted_key(127U)).has_value());
    const auto rejected = inputnext::profiles::parse_profile(dotted_key(12'000U));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.contains("table count of 128"));
    CHECK(rejected.error().line == 1U);
}

TEST_CASE("profiles.parser_table_budget_counts_explicit_and_array_tables",
    "[profiles][unit][limits]") {
    std::string tables;
    for (std::size_t index = 0U; index < 127U; ++index) {
        tables += "[table" + std::to_string(index) + "]\n";
    }
    REQUIRE(inputnext::profiles::parse_profile(tables).has_value());
    tables += "[one_more]\n";
    REQUIRE_FALSE(inputnext::profiles::parse_profile(tables).has_value());

    std::string arrays;
    for (std::size_t index = 0U; index < 128U; ++index) {
        arrays += "[[entries]]\n";
    }
    REQUIRE_FALSE(inputnext::profiles::parse_profile(arrays).has_value());
}

TEST_CASE("profiles.parser_limits_recursive_arrays_and_inline_tables", "[profiles][unit][limits]") {
    const std::string nested = "unused=" + std::string(64U, '[') + "0" + std::string(64U, ']');
    const auto rejected = inputnext::profiles::parse_profile(nested);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.contains("nested value depth"));

    std::string inline_tables{"unused="};
    for (std::size_t index = 0U; index < 64U; ++index) {
        inline_tables += "{value=";
    }
    inline_tables += "0" + std::string(64U, '}');
    REQUIRE_FALSE(inputnext::profiles::parse_profile(inline_tables).has_value());
}
