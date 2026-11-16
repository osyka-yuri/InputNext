#include "exclusive_native_test_support.hpp"
#include "output/composition/window_acquisition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <future>
#include <memory>

using namespace inputnext;
using namespace inputnext::dinput_native_test_support;

TEST_CASE("dinput.feedback_lease_requires_its_active_exact_foreground_acquisition",
    "[integration][dinput][exclusive][force_feedback]") {
    native_service_fixture fixture;
    const auto request = fixture.request();
    const auto reserved = fixture.reserve(request);
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    CHECK_FALSE(lease->rumble_window(request.target, request.window).has_value());
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(lease->wait_prepared() == DI_OK);
    CHECK_FALSE(lease->rumble_window(request.target, request.window).has_value());
    REQUIRE(lease->try_activate());

    auto other_window = request.window;
    ++other_window.revision;
    CHECK_FALSE(lease->rumble_window(request.target, other_window).has_value());
    CHECK_FALSE(lease->rumble_window({}, request.window).has_value());

    const auto first = lease->rumble_window(request.target, request.window);
    REQUIRE(first.has_value());
    CHECK((*first)->live());
    CHECK((*first)->target() == request.target);
    CHECK((*first)->epoch() == request.window);
    CHECK(lease->rumble_window(request.target, request.window).value() == *first);
    lease->cancel();
    CHECK_FALSE((*first)->live());
    CHECK_FALSE(lease->rumble_window(request.target, request.window).has_value());
    CHECK(SUCCEEDED(lease->wait_closed()));
}

TEST_CASE("dinput.feedback_lease_never_authorizes_background_exclusive_output",
    "[integration][dinput][exclusive][force_feedback]") {
    native_service_fixture fixture;
    const auto request = fixture.request();
    const auto reserved = fixture.reserve_background(nullptr);
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(lease->wait_prepared() == DI_OK);
    REQUIRE(lease->try_activate());
    const auto token = lease->rumble_window(request.target, request.window);
    REQUIRE_FALSE(token.has_value());
    CHECK(token.error() == DIERR_NOTEXCLUSIVEACQUIRED);
}

TEST_CASE("dinput.feedback_lease_cancellation_fences_concurrent_token_creation",
    "[integration][dinput][exclusive][force_feedback][concurrency]") {
    native_service_fixture fixture;
    const auto request = fixture.request();
    for (unsigned iteration = 0U; iteration < 8U; ++iteration) {
        const auto reserved = fixture.reserve(request);
        REQUIRE(reserved.has_value());
        const auto& lease = *reserved;
        REQUIRE(lease->start() == DI_OK);
        REQUIRE(lease->wait_prepared() == DI_OK);
        REQUIRE(lease->try_activate());
        auto capture = std::async(std::launch::async,
            [lease, request] { return lease->rumble_window(request.target, request.window); });
        lease->cancel();
        const auto token = capture.get();
        if (token.has_value()) {
            CHECK_FALSE((*token)->live());
        }
        CHECK(SUCCEEDED(lease->wait_closed()));
    }
}

TEST_CASE("dinput.feedback_lease_shutdown_revokes_retained_tokens",
    "[integration][dinput][exclusive][force_feedback][lifecycle]") {
    native_service_fixture fixture;
    const auto request = fixture.request();
    const auto reserved = fixture.reserve(request);
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(lease->wait_prepared() == DI_OK);
    REQUIRE(lease->try_activate());
    const auto token = lease->rumble_window(request.target, request.window);
    REQUIRE(token.has_value());
    fixture.catalogue.context.access->close_admission();
    CHECK_FALSE((*token)->live());
    fixture.catalogue.context.access->drain();
    CHECK_FALSE(lease->rumble_window(request.target, request.window).has_value());
}
