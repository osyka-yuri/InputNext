#include "profiles/detail/profile_stream.hpp"

#include "profiles/loader.hpp"
#include "profiles/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <istream>
#include <sstream>
#include <streambuf>
#include <string>

using namespace inputnext::profiles;
using namespace inputnext::profiles::detail;

namespace {

class failing_stream_buffer final : public std::streambuf {
public:
    [[nodiscard]] std::size_t read_calls() const noexcept { return read_calls_; }

protected:
    std::streamsize xsgetn([[maybe_unused]] char* destination,
        [[maybe_unused]] const std::streamsize count) override {
        ++read_calls_;
        throw std::ios_base::failure{"injected profile read failure"};
    }

private:
    std::size_t read_calls_{};
};

} // namespace

TEST_CASE("profiles.stream_accepts_exactly_the_byte_limit", "[unit][profiles]") {
    const std::string bytes(maximum_profile_bytes, 'x');
    std::istringstream stream(bytes);

    const auto loaded = read_profile_stream(stream, static_cast<std::uintmax_t>(bytes.size()));

    REQUIRE(loaded.has_value());
    CHECK(loaded->size() == maximum_profile_bytes);
}

TEST_CASE("profiles.stream_rejects_growth_past_the_byte_limit", "[unit][profiles]") {
    const std::string bytes(maximum_profile_bytes + 1U, 'x');
    std::istringstream stream(bytes);

    const auto loaded =
        read_profile_stream(stream, static_cast<std::uintmax_t>(maximum_profile_bytes));

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::too_large);
}

TEST_CASE("profiles.stream_rejects_a_file_shorter_than_its_initial_size", "[unit][profiles]") {
    std::istringstream stream("short");

    const auto loaded = read_profile_stream(stream, 6U);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::io_error);
}

TEST_CASE("profiles.stream_stops_after_a_bad_read_without_retrying", "[unit][profiles]") {
    failing_stream_buffer buffer;
    std::istream stream(&buffer);

    const auto loaded = read_profile_stream(stream, 1U);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::io_error);
    CHECK(stream.bad());
    CHECK(buffer.read_calls() == 1U);
}

TEST_CASE("profiles.stream_rejects_initial_sizes_over_the_byte_limit_without_reading",
    "[unit][profiles]") {
    failing_stream_buffer buffer;
    std::istream stream(&buffer);

    const auto loaded = read_profile_stream(stream, maximum_profile_bytes + 1U);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::too_large);
    CHECK(buffer.read_calls() == 0U);
}
