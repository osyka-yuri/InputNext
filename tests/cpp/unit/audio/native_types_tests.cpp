#include "audio/native_types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <inputnext/device_metadata.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {

using inputnext::audio_detail::fixed_haptic_endpoint_id;
using inputnext::audio_detail::fixed_haptic_endpoint_id_error;

TEST_CASE("audio.endpoint_id_is_bounded_and_complete", "[unit][audio]") {
    const auto empty = fixed_haptic_endpoint_id::create({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == fixed_haptic_endpoint_id_error::empty);

    constexpr std::array embedded_null{u'a', u'\0', u'b'};
    const auto malformed = fixed_haptic_endpoint_id::create(
        std::u16string_view{embedded_null.data(), embedded_null.size()});
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error() == fixed_haptic_endpoint_id_error::embedded_null);

    const std::u16string maximum(inputnext::haptic_endpoint_content_code_units, u'x');
    const auto accepted = fixed_haptic_endpoint_id::create(maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted->size() == maximum.size());
    CHECK(accepted->view() == maximum);
    CHECK(accepted->terminated_data()[accepted->size()] == u'\0');

    const std::u16string oversized(inputnext::haptic_endpoint_buffer_code_units, u'x');
    const auto rejected = fixed_haptic_endpoint_id::create(oversized);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == fixed_haptic_endpoint_id_error::too_long);
}

} // namespace
