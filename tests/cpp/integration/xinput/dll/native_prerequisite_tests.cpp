#include "native_prerequisites.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("xinput.dll.manual_native_prerequisite_decision", "[integration][xinput][dll]") {
    using inputnext::xinput_dll_tests::decide_native_prerequisites;
    using inputnext::xinput_dll_tests::native_prerequisite_state;

    CHECK(decide_native_prerequisites({.foreground = false,
              .production_bundle_matches = true,
              .controller_connected = true}) == native_prerequisite_state::foreground_missing);
    CHECK(decide_native_prerequisites({.foreground = true,
              .production_bundle_matches = false,
              .controller_connected = true}) ==
          native_prerequisite_state::production_bundle_mismatch);
    CHECK(
        decide_native_prerequisites({.foreground = true,
            .production_bundle_matches = true,
            .controller_connected = false}) == native_prerequisite_state::controller_disconnected);
    CHECK(decide_native_prerequisites({.foreground = true,
              .production_bundle_matches = true,
              .controller_connected = true}) == native_prerequisite_state::ready);
}
