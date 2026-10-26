#include "native_session_test_support.hpp"

#include "audio/audio_service.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include "support/xinput/test_ports.hpp"

#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace inputnext::runtime_session_test_support {
namespace {

class audio_session_fixture final {
public:
    audio_session_fixture() {
        auto factory = std::make_unique<test::audio::controlled_native_port_factory>();
        audio = factory->state_for(0U);
        auto native_port = std::make_unique<fake_gameinput>();
        native = native_port.get();
        native->device_facts = runtime_fixture::native_facts(u"session-haptic-endpoint");
        native->device_facts.gameinput_capabilities = {.availability =
                                                           metadata_availability::available,
            .value = {.gamepad = true,
                .low_frequency_rumble = true,
                .high_frequency_rumble = true}};

        audio->endpoint = {.id = *audio_detail::fixed_haptic_endpoint_id::create(
                               native->device_facts.haptic_endpoint_id.value),
            .state = audio_detail::native_endpoint_state::active,
            .flow = audio_detail::native_endpoint_flow::render,
            .container_id = native->device_facts.container_id.value};
        audio->block_on(audio_detail::native_operation::get_device);

        auto metadata = std::make_unique<fake_metadata_provider>();
        metadata->model = {.availability = metadata_availability::available,
            .value = metadata_device_model::dualsense};
        metadata->transport = {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::native_bus_type}};

        device = handle(device_memory);
        native->set_current(device,
            native_reading_lane::gamepad,
            pad_parts(test_pad(static_cast<button_mask>(gamepad_button::south)),
                100U,
                current_qpc()));

        native_session_dependencies dependencies{};
        dependencies.identity = test_runtime_id();
        dependencies.runtime_library = L"C:\\fixture\\GameInputRedist.dll";
        dependencies.native = std::move(native_port);
        dependencies.metadata = std::move(metadata);
        dependencies.focus_source = std::make_unique<xinput_test_support::focus_source>();
        dependencies.audio_factory = std::move(factory);
        dependencies.audio_launcher = audio_detail::make_std_audio_worker_launcher();
        session = native_session::prepare(std::move(dependencies));
    }

    audio_session_fixture(const audio_session_fixture&) = delete;
    audio_session_fixture& operator=(const audio_session_fixture&) = delete;
    audio_session_fixture(audio_session_fixture&&) = delete;
    audio_session_fixture& operator=(audio_session_fixture&&) = delete;

    ~audio_session_fixture() noexcept {
        audio->release_block();
        if (session != nullptr) {
            session->shutdown();
        }
    }

    int device_memory{};
    native_device_handle device{};
    fake_gameinput* native{};
    std::shared_ptr<test::audio::controlled_native_port_state> audio;
    std::unique_ptr<native_session> session;
};

class audio_release_guard final {
public:
    explicit audio_release_guard(test::audio::controlled_native_port_state& state) noexcept
        : state_(state) {}

    ~audio_release_guard() noexcept { state_.release_block(); }

    audio_release_guard(const audio_release_guard&) = delete;
    audio_release_guard& operator=(const audio_release_guard&) = delete;
    audio_release_guard(audio_release_guard&&) = delete;
    audio_release_guard& operator=(audio_release_guard&&) = delete;

private:
    test::audio::controlled_native_port_state& state_;
};

TEST_CASE("runtime.session.audio_setup_isolated_from_input_and_drained_before_native_shutdown",
    "[integration][runtime][session][audio][concurrency]") {
    audio_session_fixture fixture;
    std::future<DWORD> reading;
    std::future<void> stopping;
    const audio_release_guard release{*fixture.audio};
    REQUIRE(fixture.session->start().has_value());
    fixture.native->emit(fixture.device, native_device_event::connected);
    fixture.session->activate();
    REQUIRE(fixture.audio->wait_until_blocked(audio_detail::native_operation::get_device));

    reading = std::async(std::launch::async, [&] {
        XINPUT_STATE state{};
        return fixture.session->presentation().get_state(xinput::flavor::v1_4, 0U, &state);
    });
    REQUIRE(reading.wait_for(500ms) == std::future_status::ready);
    CHECK(reading.get() == ERROR_SUCCESS);

    stopping = std::async(std::launch::async, [&] { fixture.session->shutdown(); });
    REQUIRE(wait_until([&] {
        return fixture.session->drain_progress().phase ==
               native_session_drain_phase::stopping_audio;
    }));
    CHECK(stopping.wait_for(20ms) == std::future_status::timeout);
    CHECK(fixture.session->drain_progress().resident_audio_slots == 1U);

    fixture.audio->release_block();
    REQUIRE(stopping.wait_for(2s) == std::future_status::ready);
    stopping.get();
    const auto progress = fixture.session->drain_progress();
    CHECK(progress.phase == native_session_drain_phase::drained);
    CHECK(progress.resident_audio_slots == 0U);
    CHECK(fixture.audio->count_calls(audio_detail::native_operation::finish_worker_sta) == 1U);
    CHECK(fixture.audio->all_native_calls_on_worker());
}

} // namespace
} // namespace inputnext::runtime_session_test_support
