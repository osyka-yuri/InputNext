#include "audio/audio_service.hpp"
#include "audio/unavailable_native_factory.hpp"
#include "native_session_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "filesystem/temporary_directory.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput_probe/process/executable_path.hpp"
#include "gameinput_probe/process/supervisor.hpp"
#include "profiles/model.hpp"
#include "profiles/parser.hpp"
#include "runtime/session/native_paths.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace inputnext::runtime_session_test_support {
namespace {

class profile_session final {
public:
    explicit profile_session(const profiles::profile& settings) {
        auto port = std::make_unique<fake_gameinput>();
        native_ = port.get();
        native_->device_facts = runtime_fixture::native_facts(u"profile-endpoint");
        native_->device_facts.gameinput_capabilities = {.availability =
                                                            metadata_availability::available,
            .value = {.gamepad = true,
                .low_frequency_rumble = true,
                .high_frequency_rumble = true}};
        reports_ = native_->raw_report_script();

        auto metadata = std::make_unique<fake_metadata_provider>();
        metadata->model = {.availability = metadata_availability::available,
            .value = metadata_device_model::dualsense};
        metadata->transport = {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::native_bus_type}};

        const auto device = handle(device_memory_);
        native_->set_current(device,
            native_reading_lane::gamepad,
            pad_parts(test_pad(static_cast<button_mask>(gamepad_button::south)),
                100U,
                current_qpc()));

        native_session_dependencies dependencies{};
        dependencies.audio_factory = test::audio::make_unavailable_native_port_factory();
        dependencies.audio_launcher = audio_detail::make_std_audio_worker_launcher();
        dependencies.identity = test_runtime_id();
        dependencies.runtime_library = L"C:\\fixture\\GameInputRedist.dll";
        dependencies.native = std::move(port);
        dependencies.metadata = std::move(metadata);
        dependencies.focus_source = std::make_unique<xinput_test_support::focus_source>();
        dependencies.profile_settings = settings;
        session_ = native_session::prepare(std::move(dependencies));
    }

    ~profile_session() noexcept {
        release_maintenance();
        reports_->release_all();
        session_->shutdown();
    }

    profile_session(const profile_session&) = delete;
    profile_session& operator=(const profile_session&) = delete;
    profile_session(profile_session&&) = delete;
    profile_session& operator=(profile_session&&) = delete;

    [[nodiscard]] bool start() {
        if (!session_->start().has_value()) {
            return false;
        }
        native_->emit(handle(device_memory_), native_device_event::connected);
        session_->activate();
        return true;
    }

    [[nodiscard]] bool delivered_profile() const {
        return wait_until([this] {
            return std::ranges::any_of(reports_->sent_payloads(), [](const auto& payload) {
                return payload.size() == 64U && payload[11U] == std::byte{0x25} &&
                       payload[12U] == std::byte{0x84} && payload[14U] == std::byte{0x05} &&
                       payload[22U] == std::byte{0x21} && payload[23U] == std::byte{0xFC} &&
                       payload[24U] == std::byte{0x03};
            });
        });
    }

    void shutdown() const noexcept { session_->shutdown(); }

    [[nodiscard]] bool block_maintenance() {
        maintenance_blocked_ = true;
        native_->block_next_facts();
        native_->emit(handle(device_memory_), native_device_event::haptic_info_updated);
        return native_->wait_for_blocked_facts();
    }

    void release_maintenance() noexcept {
        if (maintenance_blocked_) {
            native_->release_facts();
            maintenance_blocked_ = false;
        }
    }

    [[nodiscard]] bool presentation_closed() const {
        XINPUT_STATE state{};
        return session_->presentation().get_state(xinput::flavor::v1_4, 0U, &state) ==
               ERROR_DEVICE_NOT_CONNECTED;
    }

    [[nodiscard]] std::shared_ptr<raw_report_test_controls> reports() const noexcept {
        return reports_;
    }

private:
    int device_memory_{};
    fake_gameinput* native_{};
    std::shared_ptr<raw_report_test_controls> reports_;
    std::unique_ptr<native_session> session_;
    bool maintenance_blocked_{};
};

constexpr std::string_view sample_profile = R"toml(
[effects.left_trigger]
mode = "feedback"
start = 0.20
strength = 0.60
[effects.right_trigger]
mode = "weapon"
start = 0.25
end = 0.75
strength = 0.80
)toml";

} // namespace

TEST_CASE("runtime.session.parsed_profile_reaches_the_native_usb_sink",
    "[integration][runtime][session][profiles][dualsense]") {
    const auto parsed = profiles::parse_profile(sample_profile);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());
    profile_session fixture{parsed->settings};
    REQUIRE(fixture.start());
    REQUIRE(fixture.delivered_profile());
    fixture.shutdown();
    CHECK(fixture.reports()->report_releases() == fixture.reports()->report_creations());
}

TEST_CASE("runtime.session.profile_file_child", "[.owned-child]") {
    const auto settings = load_process_profile();
    REQUIRE(settings.left_trigger.has_value());
    REQUIRE(settings.right_trigger.has_value());
    profile_session fixture{settings};
    REQUIRE(fixture.start());
    REQUIRE(fixture.delivered_profile());
}

TEST_CASE("runtime.session.active_profile_shutdown_closes_before_blocked_maintenance_finishes",
    "[integration][runtime][session][profiles][concurrency][shutdown]") {
    const auto parsed = profiles::parse_profile(sample_profile);
    REQUIRE(parsed.has_value());
    profile_session fixture{parsed->settings};
    REQUIRE(fixture.start());
    REQUIRE(fixture.delivered_profile());
    REQUIRE(fixture.block_maintenance());
    std::future<void> stopped;

    struct maintenance_release final {
        profile_session& fixture;

        explicit maintenance_release(profile_session& value) noexcept : fixture(value) {}

        ~maintenance_release() { fixture.release_maintenance(); }

        maintenance_release(const maintenance_release&) = delete;
        maintenance_release& operator=(const maintenance_release&) = delete;
        maintenance_release(maintenance_release&&) = delete;
        maintenance_release& operator=(maintenance_release&&) = delete;
    };

    const maintenance_release release{fixture};

    stopped = std::async(std::launch::async, [&fixture] { fixture.shutdown(); });
    REQUIRE(wait_until([&fixture] { return fixture.presentation_closed(); }));
    CHECK(stopped.wait_for(0s) == std::future_status::timeout);
    fixture.release_maintenance();
    REQUIRE(stopped.wait_for(2s) == std::future_status::ready);
    stopped.get();
    CHECK(fixture.reports()->report_releases() == fixture.reports()->report_creations());
}

TEST_CASE("runtime.session.process_profile_is_loaded_beside_executable_in_an_unrelated_cwd",
    "[integration][runtime][session][profiles][process]") {
    const test::temporary_directory sandbox;
    const auto executable = sandbox.path() / L"profile-session.exe";
    std::filesystem::copy_file(gameinput_detail::current_executable_path(), executable);
    const auto cwd = sandbox.path() / L"unrelated-cwd";
    std::filesystem::create_directory(cwd);
    {
        std::ofstream file{sandbox.path() / L"inputnext.toml", std::ios::binary};
        REQUIRE(file.is_open());
        file << sample_profile;
        REQUIRE(file.good());
    }
    std::ostringstream output;
    std::ostringstream errors;
    const auto result = gameinput_detail::run_owned_process(
        {.executable = executable,
            .working_directory = cwd,
            .arguments = {L"runtime.session.profile_file_child"},
            .timeout = 10s},
        {.output = output, .errors = errors});
    INFO(output.str());
    INFO(errors.str());
    REQUIRE_FALSE(result.timed_out);
    CHECK(result.cleanup_confirmed);
    CHECK(result.exit_code == 0U);
}

} // namespace inputnext::runtime_session_test_support
