#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

class scoped_file {
public:
    explicit scoped_file(std::filesystem::path path) : path_(std::move(path)) {
        std::ofstream stream{path_};
        stream << "fixture resource\n";
        stream.close();
        if (!stream) {
            throw std::runtime_error{"Cannot create fixture resource"};
        }
    }

    ~scoped_file() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    scoped_file(const scoped_file&) = delete;
    scoped_file& operator=(const scoped_file&) = delete;
    scoped_file(scoped_file&&) = delete;
    scoped_file& operator=(scoped_file&&) = delete;

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("harness.cpp23_is_enabled", "[harness][integration]") {
    const std::expected<int, int> value{42};
    REQUIRE(value.has_value());
    CHECK(*value == 42);

    const std::expected<int, int> error{std::unexpected{7}};
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error() == 7);
}

TEST_CASE("harness.assertion_failure_probe", "[harness][.failure-probe]") {
    // The parent check supplies a fresh working directory for this probe.
    std::ofstream entered{"entered.txt"};
    entered << "probe entered\n";
    entered.close();
    REQUIRE(entered.good());

    const scoped_file resource{"fixture.tmp"};
    REQUIRE(std::filesystem::exists("fixture.tmp"));
    FAIL("Intentional assertion failure for the runner self-check");
}
