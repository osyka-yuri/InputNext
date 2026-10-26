#include "audio/native_port.hpp"
#include "audio/native_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <utility>

namespace {

using inputnext::audio_detail::fixed_haptic_endpoint_id;

TEST_CASE("audio.windows_port_sta_impossible_endpoint_cleanup_has_no_stream_start",
    "[integration][audio][windows]") {
    using inputnext::audio_detail::make_windows_native_port_factory;
    using inputnext::audio_detail::native_error;
    using inputnext::audio_detail::native_operation;

    auto factory = make_windows_native_port_factory();
    REQUIRE(factory != nullptr);
    auto created = factory->create();
    REQUIRE(created.has_value());
    auto port = std::move(*created);

    struct smoke_result final {
        bool apartment_started{};
        bool impossible_endpoint_rejected{};
        native_error endpoint_error{};
        bool cleanup_finished{};
        bool buffer_event_closed{};
        bool stop_was_not_attempted{};
    } result{};

    const auto impossible_id = fixed_haptic_endpoint_id::create(
        u"\\\\?\\inputnext-audio-test\\impossible\\{3f6e4a21-90ac-4a1f-89d2-5d2ac6ccdb12}");
    REQUIRE(impossible_id.has_value());

    std::thread worker([&port, &result, &impossible_id] {
        const auto apartment = port->begin_worker_sta();
        if (!apartment.has_value()) {
            result.endpoint_error = apartment.error();
        } else {
            result.apartment_started = true;
            const auto endpoint = port->open_exact_endpoint(*impossible_id);
            result.impossible_endpoint_rejected = !endpoint.has_value();
            if (!endpoint.has_value()) {
                result.endpoint_error = endpoint.error();
            }
        }

        const auto cleanup = port->finish_worker_sta();
        result.cleanup_finished = cleanup.has_value();
        if (cleanup.has_value()) {
            result.buffer_event_closed =
                cleanup->buffer_event_close.attempted && cleanup->buffer_event_close.succeeded;
            result.stop_was_not_attempted = !cleanup->stop.attempted;
        }
    });
    worker.join();

    CHECK(result.apartment_started);
    CHECK(result.impossible_endpoint_rejected);
    CHECK((result.endpoint_error.operation == native_operation::get_device ||
           result.endpoint_error.operation == native_operation::create_device_enumerator));
    CHECK(result.endpoint_error.native_code != 0U);
    CHECK(result.cleanup_finished);
    CHECK(result.buffer_event_closed);
    CHECK(result.stop_was_not_attempted);
}

} // namespace
