#include <inputnext/focus.hpp>

#include <inputnext/device_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::focus;

TEST_CASE("focus.current_process_source_uses_passive_os_observation",
    "[integration][focus][windows]") {
    auto source = make_current_process_source();
    REQUIRE(source != nullptr);
    const auto process = source->process();
    REQUIRE(process.pid != 0U);
    REQUIRE(process.creation_time_100ns != 0U);

    const auto reading = source->observe();
    CHECK(reading.process == process);
    CHECK(static_cast<std::uint8_t>(reading.state) <=
          static_cast<std::uint8_t>(evidence::foreground));

    runtime_id incarnation{};
    incarnation.bytes[0U] = std::byte{7U};
    observer value{incarnation, std::move(source)};
    CHECK(value.published().process == process);
    value.stop();
    CHECK(!value.published().open);
}

} // namespace
