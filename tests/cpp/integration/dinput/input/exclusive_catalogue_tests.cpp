#include "catalogue_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/catalogue/devices.hpp"
#include "dinput/native_types.hpp"
#include "gameinput/native/native_gameinput.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;

namespace {

constexpr std::string_view controller_path{R"(\\?\HID#VID_054C&PID_0CE6#Exclusive)"};

class release_enumeration final {
public:
    explicit release_enumeration(std::shared_ptr<native_control_script> controls) noexcept
        : controls_(std::move(controls)) {}

    ~release_enumeration() { controls_->release_block(native_operation::enum_devices); }

    release_enumeration(const release_enumeration&) = delete;
    release_enumeration& operator=(const release_enumeration&) = delete;
    release_enumeration(release_enumeration&&) = delete;
    release_enumeration& operator=(release_enumeration&&) = delete;

private:
    const std::shared_ptr<native_control_script> controls_;
};

[[nodiscard]] com_owner<IDirectInput8W> native_root(environment& fixture) {
    void* result{};
    const auto status = create_system(&fixture.context,
        GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        &result,
        nullptr);
    REQUIRE(status == DI_OK);
    return com_owner<IDirectInput8W>{static_cast<IDirectInput8W*>(result)};
}

[[nodiscard]] DIPROPGUIDANDPATH expected_path() {
    DIPROPGUIDANDPATH result{};
    result.guidClass = GUID_Joystick;
    const std::wstring value{controller_path.begin(), controller_path.end()};
    wcscpy_s(result.wszPath, value.c_str());
    return result;
}

[[nodiscard]] connection_target connect_matched(environment& fixture, bool publish_sample = true) {
    fixture.add_system_gamepad(guid(1U),
        std::wstring{controller_path.begin(), controller_path.end()});
    return fixture.connect(0U, std::string{controller_path}, true, publish_sample);
}

[[nodiscard]] HRESULT
qualify(environment& fixture, IDirectInput8W& system, connection_target target) {
    return dinput::qualify_exclusive_device(*fixture.context.access,
        system,
        target,
        guid(1U),
        expected_path());
}

[[nodiscard]] std::optional<std::uint64_t> wait_for_facts_refresh(environment& fixture,
    connection_target target,
    std::uint64_t original_generation) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        fixture.owners->gameinput.maintain();
        const auto current = fixture.owners->gameinput.runtime->query_metadata(target);
        if (current.has_value() && current->catalogue_generation != original_generation) {
            return current->catalogue_generation;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("dinput.exclusive_catalogue.requires_complete_current_correlation",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    REQUIRE(qualify(fixture, *native, target) == DI_OK);

    auto wrong_class = expected_path();
    wrong_class.guidClass = GUID_SysMouse;
    CHECK(dinput::qualify_exclusive_device(*fixture.context.access,
              *native,
              target,
              guid(1U),
              wrong_class) == DIERR_UNSUPPORTED);
    CHECK(dinput::qualify_exclusive_device(*fixture.context.access,
              *native,
              target,
              guid(2U),
              expected_path()) == DIERR_UNSUPPORTED);

    auto malformed = expected_path();
    for (auto& character : malformed.wszPath) {
        character = L'x';
    }
    CHECK(dinput::qualify_exclusive_device(*fixture.context.access,
              *native,
              target,
              guid(1U),
              malformed) == DIERR_UNSUPPORTED);
}

TEST_CASE("dinput.exclusive_catalogue_rejects_unknown_competitor_paths_without_hiding_discovery",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    static_cast<void>(fixture.connect(1U));
    REQUIRE(fixture.wait_for_catalogue(2U));
    const auto native = native_root(fixture);
    CHECK(qualify(fixture, *native, target) == DIERR_UNSUPPORTED);

    const auto root = fixture.create_root();
    CHECK(enumerate(*root).size() == 2U);
}

TEST_CASE("dinput.exclusive_catalogue_accepts_a_live_target_before_the_first_sample",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture, false);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    CHECK(qualify(fixture, *native, target) == DI_OK);
}

TEST_CASE("dinput.exclusive_catalogue_rejects_unknown_native_competitor_paths",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    fixture.add_system_gamepad(guid(2U), L"");
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    CHECK(qualify(fixture, *native, target) == DIERR_UNSUPPORTED);
}

TEST_CASE("dinput.exclusive_catalogue_propagates_incomplete_native_evidence",
    "[integration][dinput][exclusive][catalogue][failure]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    fixture.add_system_gamepad(guid(2U), L"Different competitor path");
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    HRESULT expected{};
    SECTION("native enumeration failure") {
        std::scoped_lock lock{fixture.system->gate};
        expected = E_ABORT;
        fixture.system->enum_devices_result = expected;
    }
    SECTION("competitor creation failure") {
        std::scoped_lock lock{fixture.system->gate};
        expected = E_ACCESSDENIED;
        fixture.system->devices.back().create_device_result = expected;
    }
    SECTION("competitor path inspection failure") {
        std::scoped_lock lock{fixture.system->gate};
        expected = E_FAIL;
        fixture.system->devices.back().guid_path_result = expected;
    }
    CHECK(qualify(fixture, *native, target) == expected);
}

TEST_CASE("dinput.exclusive_catalogue_rejects_duplicate_paths_and_native_guids",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    SECTION("native path") {
        fixture.add_system_gamepad(guid(2U),
            std::wstring{controller_path.begin(), controller_path.end()});
    }
    SECTION("native GUID") {
        fixture.add_system_gamepad(guid(1U), L"Other native path");
    }
    SECTION("GameInput path") {
        static_cast<void>(fixture.connect(1U, std::string{controller_path}));
        REQUIRE(fixture.wait_for_catalogue(2U));
    }
    CHECK(qualify(fixture, *native, target) == DIERR_UNSUPPORTED);
}

TEST_CASE("dinput.exclusive_catalogue_ignores_detached_native_phantoms",
    "[integration][dinput][exclusive][catalogue]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    {
        std::scoped_lock lock{fixture.system->gate};
        fixture.system->devices.push_back(system_device_record{
            .instance = make_system_instance(guid(2U), L"Detached native device"),
            .path = {},
            .attached = false});
    }
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    CHECK(qualify(fixture, *native, target) == DI_OK);
}

TEST_CASE("dinput.exclusive_catalogue_does_not_transfer_a_saved_match_after_reconnect",
    "[integration][dinput][exclusive][catalogue][reconnect]") {
    environment fixture;
    const auto original = connect_matched(fixture);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    REQUIRE(qualify(fixture, *native, original) == DI_OK);
    REQUIRE(fixture.disconnect(0U, original));
    const auto replacement = fixture.connect(0U, std::string{controller_path});
    REQUIRE(replacement != original);
    REQUIRE(fixture.wait_for_catalogue(1U));
    CHECK(qualify(fixture, *native, original) == DIERR_INPUTLOST);
    CHECK(qualify(fixture, *native, replacement) == DI_OK);
}

TEST_CASE("dinput.exclusive_catalogue_rejects_facts_refresh_during_native_inspection",
    "[integration][dinput][exclusive][catalogue][concurrency]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    constexpr std::string_view competitor_path{R"(\\?\HID#Competitor)"};
    fixture.add_system_gamepad(guid(2U),
        std::wstring{competitor_path.begin(), competitor_path.end()});
    const auto competitor = fixture.connect(1U, std::string{competitor_path});
    REQUIRE(fixture.wait_for_catalogue(2U));
    const auto native = native_root(fixture);

    std::size_t changed_index{};
    auto changed_target = target;
    HRESULT expected = HRESULT_FROM_WIN32(ERROR_BUSY);
    bool restore_original_path{};
    metadata_field<std::string> changed_path{};
    SECTION("selected path changes") {
        changed_path = {.availability = metadata_availability::available,
            .value = R"(\\?\HID#Replacement)"};
    }
    SECTION("competitor becomes ambiguous") {
        changed_index = 1U;
        changed_target = competitor;
        changed_path = {.availability = metadata_availability::available,
            .value = std::string{controller_path}};
    }
    SECTION("competitor path becomes unknown") {
        changed_index = 1U;
        changed_target = competitor;
        expected = DIERR_UNSUPPORTED;
    }
    SECTION("same path receives a new facts generation") {
        changed_path = {.availability = metadata_availability::available,
            .value = std::string{controller_path}};
    }
    SECTION("path changes away and back") {
        changed_path = {.availability = metadata_availability::available,
            .value = R"(\\?\HID#Temporary)"};
        restore_original_path = true;
    }
    const auto original = fixture.owners->gameinput.runtime->query_metadata(changed_target);
    REQUIRE(original.has_value());
    const auto before = fixture.owners->core->enumerate();
    REQUIRE(before.has_value());
    const auto controls = fixture.system->controls;
    controls->block_next(native_operation::enum_devices);
    auto pending = std::async(std::launch::async,
        [&fixture, &native, target] { return qualify(fixture, *native, target); });
    const release_enumeration cleanup{controls};
    REQUIRE(controls->wait_for_entry(native_operation::enum_devices));

    auto facts = original->native;
    facts.pnp_path_utf8 = std::move(changed_path);
    const auto device =
        gameinput_test_support::handle(fixture.owners->gameinput.device_memory[changed_index]);
    fixture.owners->gameinput.native->update_facts(device, std::move(facts));
    fixture.owners->gameinput.native->emit(device,
        gameinput_detail::native_device_event::haptic_info_updated);
    const auto refreshed_generation =
        wait_for_facts_refresh(fixture, changed_target, original->catalogue_generation)
            .value_or(0U);
    REQUIRE(refreshed_generation != 0U);
    if (restore_original_path) {
        fixture.owners->gameinput.native->update_facts(device, original->native);
        fixture.owners->gameinput.native->emit(device,
            gameinput_detail::native_device_event::haptic_info_updated);
        REQUIRE(wait_for_facts_refresh(fixture, changed_target, refreshed_generation).has_value());
    }
    const auto after = fixture.owners->core->enumerate();
    REQUIRE(after.has_value());
    REQUIRE(after->revision == before->revision);
    controls->release_block(native_operation::enum_devices);
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    const HRESULT status = pending.get();
    CHECK(status == expected);
}

TEST_CASE("dinput.exclusive_catalogue_rejects_incomplete_facts_recollection",
    "[integration][dinput][exclusive][catalogue][concurrency]") {
    environment fixture;
    const auto target = connect_matched(fixture);
    REQUIRE(fixture.wait_for_catalogue(1U));
    const auto native = native_root(fixture);
    const auto controls = fixture.system->controls;
    controls->block_next(native_operation::enum_devices);
    auto pending = std::async(std::launch::async,
        [&fixture, &native, target] { return qualify(fixture, *native, target); });
    const release_enumeration cleanup{controls};
    REQUIRE(controls->wait_for_entry(native_operation::enum_devices));

    HRESULT expected{};
    SECTION("pending facts installation") {
        fixture.owners->gameinput.native->emit(
            gameinput_test_support::handle(fixture.owners->gameinput.device_memory[0U]),
            gameinput_detail::native_device_event::haptic_info_updated);
        expected = HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    SECTION("metadata query fails") {
        fixture.owners->gameinput.metadata->fail_next_query(metadata_error::resource_exhausted);
        expected = DIERR_OUTOFMEMORY;
    }
    SECTION("target membership changes") {
        static_cast<void>(fixture.connect(1U, std::string{controller_path}));
        REQUIRE(fixture.wait_for_catalogue(2U));
        expected = HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    controls->release_block(native_operation::enum_devices);
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    const HRESULT status = pending.get();
    CHECK(status == expected);
}
