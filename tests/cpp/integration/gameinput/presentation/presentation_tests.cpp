#include "gameinput/native/native_gameinput.hpp"

#include "gameinput/native/native_history.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "gameinput/reading_parts.hpp"
#include "gameinput/runtime/factory.hpp"
#include "metadata/provider.hpp"
#include "support/focus/controlled_source.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t device_count = 5U;

template <typename Value>
[[nodiscard]] Value checked_value(const std::optional<Value>& candidate) {
    if (!candidate.has_value()) {
        throw std::runtime_error("Expected an assigned XInput position.");
    }
    return candidate.value();
}

[[nodiscard]] runtime_id runtime_identity(std::byte value) {
    runtime_id result{};
    result.bytes[0U] = value;
    return result;
}

class narrow_native_port final : public native_gameinput {
public:
    class endpoint final : public native_rumble_endpoint {
    public:
        [[nodiscard]] output::native_result set(output::rumble_value value) noexcept override {
            static_cast<void>(value);
            return output::native_result{.status = output::native_status::accepted};
        }
    };

    [[nodiscard]] native_result<void> initialize(
        const std::filesystem::path& runtime_library) override {
        if (!runtime_library.is_absolute()) {
            return std::unexpected(native_failure{.cause = native_failure_cause::load_failed});
        }
        return {};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        std::scoped_lock lock{mutex_};
        watch_ = &context;
        return native_callback_token{.value = next_token_++};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        std::array<native_device_handle, device_count> live{};
        std::size_t size{};
        native_callback_token token{};
        {
            std::scoped_lock lock{mutex_};
            token = native_callback_token{.value = next_token_++};
            for (std::size_t index = 0U; index < connected_.size(); ++index) {
                if (connected_[index]) {
                    live[size++] = handle(index);
                }
            }
        }
        for (std::size_t index = 0U; index < size; ++index) {
            context.deliver(context.owner, live[index], native_device_event::connected);
        }
        return token;
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) override {
        static_cast<void>(device);
        static_cast<void>(context);
        std::scoped_lock lock{mutex_};
        return native_callback_token{.value = next_token_++};
    }

    [[nodiscard]] bool unregister_callback(native_callback_token token) noexcept override {
        return token.valid();
    }

    [[nodiscard]] native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) override {
        const auto index = device_index(device);
        if (index >= device_count) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::device_disconnected});
        }
        const auto sequence = next_sequence_.fetch_add(1U, std::memory_order_relaxed);
        if (lane == native_reading_lane::gamepad) {
            gamepad_state state{};
            state.buttons = static_cast<button_mask>(gamepad_button::south);
            state.left_stick.x = 0.5F;
            state.left_stick.y = -0.25F;
            state.right_stick.x = -0.75F;
            state.right_stick.y = 0.125F;
            state.left_trigger = 0.5F;
            state.right_trigger = 0.25F;
            return reading_parts{.pad = state,
                .source_timestamp_us = sequence,
                .observation_qpc = sequence};
        }
        return reading_parts{.guide = false,
            .source_timestamp_us = sequence,
            .observation_qpc = sequence};
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device) override {
        static_cast<void>(device);
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    }

    [[nodiscard]] native_result<std::uint64_t> performance_frequency() noexcept override {
        return 10'000'000U;
    }

    [[nodiscard]] native_result<native_device_facts> facts(native_device_handle device) override {
        const auto index = device_index(device);
        if (index >= device_count) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::device_disconnected});
        }
        native_device_facts result{};
        result.vendor_id = {.availability = metadata_availability::available, .value = 0x054CU};
        result.product_id = {.availability = metadata_availability::available,
            .value = static_cast<std::uint16_t>(0x0CE6U + index)};
        result.gameinput_capabilities = {.availability = metadata_availability::available,
            .value = {.gamepad = true,
                .low_frequency_rumble = true,
                .high_frequency_rumble = true}};
        return result;
    }

    [[nodiscard]] bool connected(native_device_handle device) noexcept override {
        const auto index = device_index(device);
        if (index >= device_count) {
            return false;
        }
        std::scoped_lock lock{mutex_};
        return connected_[index];
    }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>> create_rumble_endpoint(
        native_device_handle device) override {
        if (!device.valid() || !connected(device)) {
            return std::unexpected(
                native_failure{.cause = native_failure_cause::device_disconnected});
        }
        return std::unique_ptr<native_rumble_endpoint>{std::make_unique<endpoint>()};
    }

    void retain(native_device_handle device) noexcept override { static_cast<void>(device); }

    void release(native_device_handle device) noexcept override {
        const auto index = device_index(device);
        if (index == release_block_index_.exchange(device_count, std::memory_order_acq_rel)) {
            release_entered_.release();
            release_continue_.acquire();
        }
        if (index < device_count) {
            release_counts_[index].fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void disconnect(std::size_t index) {
        native_device_callback_context const* watch{};
        {
            std::scoped_lock lock{mutex_};
            connected_[index] = false;
            watch = watch_;
        }
        if (watch != nullptr) {
            watch->deliver(watch->owner, handle(index), native_device_event::disconnected);
        }
    }

    void reconnect(std::size_t index) {
        native_device_callback_context const* watch{};
        {
            std::scoped_lock lock{mutex_};
            connected_[index] = true;
            watch = watch_;
        }
        if (watch != nullptr) {
            watch->deliver(watch->owner, handle(index), native_device_event::connected);
        }
    }

    void block_next_release(std::size_t index) noexcept {
        release_block_index_.store(index, std::memory_order_release);
    }

    [[nodiscard]] bool wait_for_release() { return release_entered_.try_acquire_for(2s); }

    void resume_release() noexcept { release_continue_.release(); }

    [[nodiscard]] std::size_t release_count(std::size_t index) const noexcept {
        return release_counts_[index].load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] native_device_handle handle(std::size_t index) noexcept {
        return native_device_handle{.value = &identities_[index]};
    }

    [[nodiscard]] std::size_t device_index(native_device_handle device) const noexcept {
        for (std::size_t index = 0U; index < identities_.size(); ++index) {
            if (device.value == &identities_[index]) {
                return index;
            }
        }
        return device_count;
    }

    mutable std::mutex mutex_;
    std::array<std::byte, device_count> identities_{};
    std::array<bool, device_count> connected_{true, true, true, true, true};
    native_device_callback_context* watch_{};
    std::uint64_t next_token_{1U};
    std::atomic<std::uint64_t> next_sequence_{1U};
    std::array<std::atomic<std::size_t>, device_count> release_counts_{};
    std::atomic<std::size_t> release_block_index_{device_count};
    std::binary_semaphore release_entered_{0};
    std::binary_semaphore release_continue_{0};
};

class narrow_metadata_provider final : public metadata_provider {
public:
    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const metadata_request& request) override {
        query_count_.fetch_add(1U, std::memory_order_relaxed);
        if (block_next_.exchange(false, std::memory_order_acq_rel)) {
            query_entered_.release();
            query_continue_.acquire();
        }
        metadata_snapshot result{};
        result.target = request.target;
        result.native = request.facts;
        result.capabilities = {.availability = metadata_availability::available,
            .value = {.gamepad = true,
                .low_frequency_rumble = true,
                .high_frequency_rumble = true}};
        result.transport = {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::native_enumerator}};
        return result;
    }

    void block_next_query() noexcept { block_next_.store(true, std::memory_order_release); }

    [[nodiscard]] bool wait_for_query() { return query_entered_.try_acquire_for(2s); }

    void resume_query() noexcept { query_continue_.release(); }

    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> block_next_;
    std::atomic<std::size_t> query_count_;
    std::binary_semaphore query_entered_{0};
    std::binary_semaphore query_continue_{0};
};

class presentation_fixture final {
public:
    explicit presentation_fixture(std::size_t device_total = device_count)
        : registry_(runtime_identity(std::byte{0x11})),
          device_total_(std::min(device_total, device_count)) {
        auto native = std::make_unique<narrow_native_port>();
        native_ = native.get();
        auto metadata = std::make_unique<narrow_metadata_provider>();
        metadata_ = metadata.get();
        runtime_ = runtime_factory::create(registry_,
            gameinput_options{.runtime_library = std::filesystem::path{L"C:\\GameInputRedist.dll"}},
            std::move(native),
            std::move(metadata));
    }

    ~presentation_fixture() {
        if (presentation_) {
            presentation_->close();
        }
        if (runtime_) {
            static_cast<void>(runtime_->stop(maintenance_time::clock::now()));
        }
        if (focus_) {
            focus_->stop();
        }
        presentation_.reset();
        focus_.reset();
        runtime_.reset();
        registry_.shutdown();
    }

    presentation_fixture(const presentation_fixture&) = delete;
    presentation_fixture& operator=(const presentation_fixture&) = delete;
    presentation_fixture(presentation_fixture&&) = delete;
    presentation_fixture& operator=(presentation_fixture&&) = delete;

    void start() {
        const auto now = maintenance_time::clock::now();
        started_at_ = now;
        REQUIRE(runtime_->start(now).has_value());
        for (std::size_t index = 0U; index < device_total_; ++index) {
            native_->reconnect(index);
        }
        for (std::size_t step = 0U; step < 16U; ++step) {
            runtime_->maintain(now + std::chrono::milliseconds{4 * step});
            const auto inventory = runtime_->connections();
            const auto ready = static_cast<std::size_t>(
                std::ranges::count_if(std::span{inventory.entries}.first(inventory.size),
                    [](const auto& connection) {
                        return connection.pad_known &&
                               connection.health == connection_health::callback_driven;
                    }));
            if (ready >= device_total_) {
                break;
            }
        }
        const auto inventory = runtime_->connections();
        REQUIRE(inventory.size >= device_total_);
        const auto process = focus::process_instance{.pid = 321U, .creation_time_100ns = 987654U};
        auto source = std::make_unique<test::focus::controlled_source>(process,
            std::vector<focus::reading>{
                {.process = process, .state = focus::evidence::foreground}});
        focus_ =
            std::make_unique<focus::observer>(runtime_identity(std::byte{0x22}), std::move(source));
        presentation_ = std::make_unique<gameinput_xinput>(*runtime_, *focus_);
        const auto synchronized = presentation_->synchronize(inventory);
        REQUIRE(synchronized.has_value());
    }

    [[nodiscard]] gameinput_runtime& runtime() noexcept { return *runtime_; }

    [[nodiscard]] narrow_native_port& native() noexcept { return *native_; }

    [[nodiscard]] narrow_metadata_provider& metadata() noexcept { return *metadata_; }

    [[nodiscard]] focus::observer& focus_observer() noexcept { return *focus_; }

    [[nodiscard]] gameinput_xinput& presentation() noexcept { return *presentation_; }

    [[nodiscard]] std::size_t device_total() const noexcept { return device_total_; }

    [[nodiscard]] maintenance_time now_after(std::chrono::milliseconds delta) const noexcept {
        return started_at_ + delta;
    }

private:
    device_registry registry_;
    narrow_native_port* native_{};
    narrow_metadata_provider* metadata_{};
    std::unique_ptr<gameinput_runtime> runtime_;
    std::unique_ptr<focus::observer> focus_;
    std::unique_ptr<gameinput_xinput> presentation_;
    std::size_t device_total_{device_count};
    maintenance_time started_at_;
};

[[nodiscard]] bool same_capabilities_ex(const xinput::capabilities_ex& left,
    const xinput::capabilities_ex& right) noexcept {
    return left.capabilities.Type == right.capabilities.Type &&
           left.capabilities.SubType == right.capabilities.SubType &&
           left.capabilities.Flags == right.capabilities.Flags &&
           left.capabilities.Gamepad.wButtons == right.capabilities.Gamepad.wButtons &&
           left.capabilities.Gamepad.bLeftTrigger == right.capabilities.Gamepad.bLeftTrigger &&
           left.capabilities.Gamepad.bRightTrigger == right.capabilities.Gamepad.bRightTrigger &&
           left.capabilities.Gamepad.sThumbLX == right.capabilities.Gamepad.sThumbLX &&
           left.capabilities.Gamepad.sThumbLY == right.capabilities.Gamepad.sThumbLY &&
           left.capabilities.Gamepad.sThumbRX == right.capabilities.Gamepad.sThumbRX &&
           left.capabilities.Gamepad.sThumbRY == right.capabilities.Gamepad.sThumbRY &&
           left.capabilities.Vibration.wLeftMotorSpeed ==
               right.capabilities.Vibration.wLeftMotorSpeed &&
           left.capabilities.Vibration.wRightMotorSpeed ==
               right.capabilities.Vibration.wRightMotorSpeed &&
           left.vendor_id == right.vendor_id && left.product_id == right.product_id &&
           left.product_version == right.product_version && left.unknown1 == right.unknown1;
}

[[nodiscard]] bool wait_for_focus_progress(focus::observer const& observer, std::uint64_t before) {
    const auto deadline = maintenance_time::clock::now() + 2s;
    while (maintenance_time::clock::now() < deadline) {
        if (observer.published().completed_observations > before) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

TEST_CASE("gameinput.presentation.sync_retains_eligible_assignments_and_fills_lowest_free",
    "[integration][gameinput][presentation]") {
    presentation_fixture fixture;
    fixture.start();

    const auto inventory = fixture.runtime().connections();
    const auto first = fixture.presentation().presentation().positions();
    const auto first0 = checked_value(first[0U]);
    const auto first1 = checked_value(first[1U]);
    const auto first2 = checked_value(first[2U]);
    const auto first3 = checked_value(first[3U]);
    CHECK(first0.target == inventory.entries[0U].target);
    CHECK(first1.target == inventory.entries[1U].target);
    CHECK(first2.target == inventory.entries[2U].target);
    CHECK(first3.target == inventory.entries[3U].target);
    xinput::capabilities_ex capabilities{};
    CHECK(fixture.presentation().presentation().get_capabilities_ex(xinput::flavor::v1_4,
              0U,
              0U,
              &capabilities) == ERROR_SUCCESS);
    CHECK(capabilities.vendor_id == 0x054CU);
    CHECK(capabilities.product_id == 0x0CE6U);

    auto unchanged = inventory;
    unchanged.entries[1U].health = connection_health::polling_fallback;
    REQUIRE(fixture.presentation().synchronize(unchanged).has_value());
    CHECK(fixture.presentation().presentation().positions() == first);
    REQUIRE(fixture.presentation().synchronize(unchanged).has_value());
    CHECK(fixture.presentation().presentation().positions() == first);

    unchanged.entries[0U].pad_known = false;
    REQUIRE(fixture.presentation().synchronize(unchanged).has_value());
    const auto replacement = fixture.presentation().presentation().positions();
    const auto replacement0 = checked_value(replacement[0U]);
    CHECK(replacement0.target == inventory.entries[4U].target);
    CHECK(replacement0.id != first0.id);
    CHECK(replacement[1U] == first[1U]);
    CHECK(replacement[2U] == first[2U]);
    CHECK(replacement[3U] == first[3U]);
}

TEST_CASE("gameinput.presentation.blocked_metadata_keeps_focus_and_other_position_ready",
    "[integration][gameinput][presentation][concurrency]") {
    presentation_fixture fixture{2U};
    fixture.start();
    const auto baseline = fixture.focus_observer().published().completed_observations;
    fixture.metadata().block_next_query();

    xinput::capabilities_ex output{};
    output.capabilities.Type = 0xA5U;
    output.capabilities.SubType = 0x5AU;
    output.capabilities.Flags = 0xA55AU;
    output.capabilities.Gamepad.wButtons = 0x5AA5U;
    output.capabilities.Gamepad.bLeftTrigger = 0xA5U;
    output.capabilities.Gamepad.bRightTrigger = 0x5AU;
    output.capabilities.Gamepad.sThumbLX = 0x1234;
    output.capabilities.Gamepad.sThumbLY = -0x1234;
    output.capabilities.Gamepad.sThumbRX = 0x2345;
    output.capabilities.Gamepad.sThumbRY = -0x2345;
    output.capabilities.Vibration.wLeftMotorSpeed = 0xA55AU;
    output.capabilities.Vibration.wRightMotorSpeed = 0x5AA5U;
    output.vendor_id = 0xA55AU;
    output.product_id = 0x5AA5U;
    output.product_version = 0x1357U;
    output.unknown1 = 0x2468U;
    const auto output_before = output;
    const auto original_positions = fixture.presentation().presentation().positions();
    const auto original_assignment = checked_value(original_positions[0U]);
    const auto retained_assignment = checked_value(original_positions[1U]);
    std::atomic<DWORD> query_result{0xFFFF'FFFFUL};
    std::binary_semaphore query_done{0};
    std::jthread query([&] {
        query_result.store(
            fixture.presentation().presentation().get_capabilities_ex(xinput::flavor::v1_4,
                0U,
                0U,
                &output),
            std::memory_order_release);
        query_done.release();
    });
    const bool query_blocked = fixture.metadata().wait_for_query();

    std::atomic<DWORD> ready_result{0xFFFF'FFFFUL};
    std::binary_semaphore ready_done{0};
    std::jthread ready_read([&] {
        XINPUT_STATE state{};
        ready_result.store(
            fixture.presentation().presentation().get_state(xinput::flavor::v1_4, 1U, &state),
            std::memory_order_release);
        ready_done.release();
    });
    const bool ready_while_blocked = query_blocked && ready_done.try_acquire_for(500ms);
    const bool focus_advanced =
        query_blocked && wait_for_focus_progress(fixture.focus_observer(), baseline);

    std::binary_semaphore reassignment_done{0};
    bool original_target_retired{};
    bool replacement_admitted{};
    bool reassignment_synchronized{};
    std::jthread reassignment([&] {
        fixture.native().disconnect(0U);
        const auto maintenance_time = fixture.now_after(1s);
        for (std::size_t attempt = 0U; attempt < 16U; ++attempt) {
            fixture.runtime().maintain(
                maintenance_time + std::chrono::milliseconds{
                                       static_cast<std::chrono::milliseconds::rep>(4U * attempt)});
            const auto inventory = fixture.runtime().connections();
            original_target_retired =
                std::ranges::none_of(std::span{inventory.entries}.first(inventory.size),
                    [&](const auto& connection) {
                        return connection.target == original_assignment.target;
                    });
            if (original_target_retired && fixture.native().release_count(0U) != 0U) {
                break;
            }
        }

        if (original_target_retired && fixture.native().release_count(0U) != 0U) {
            auto remaining = fixture.runtime().connections();
            reassignment_synchronized = fixture.presentation().synchronize(remaining).has_value();
            fixture.native().reconnect(0U);
            for (std::size_t attempt = 0U; attempt < 16U; ++attempt) {
                fixture.runtime().maintain(
                    maintenance_time + 2s +
                    std::chrono::milliseconds{
                        static_cast<std::chrono::milliseconds::rep>(4U * attempt)});
                remaining = fixture.runtime().connections();
                replacement_admitted =
                    std::ranges::any_of(std::span{remaining.entries}.first(remaining.size),
                        [&](const auto& connection) {
                            return connection.pad_known &&
                                   connection.target != original_assignment.target &&
                                   connection.target != retained_assignment.target;
                        });
                if (replacement_admitted) {
                    break;
                }
            }
            if (replacement_admitted) {
                reassignment_synchronized =
                    reassignment_synchronized &&
                    fixture.presentation().synchronize(remaining).has_value();
            }
        }
        reassignment_done.release();
    });
    const bool reassignment_while_blocked =
        query_blocked && reassignment_done.try_acquire_for(500ms);

    fixture.metadata().resume_query();
    static_cast<void>(query_done.try_acquire_for(2s));
    query.join();
    ready_read.join();
    reassignment.join();
    const auto replacement_positions = fixture.presentation().presentation().positions();
    const auto replacement0 = checked_value(replacement_positions[0U]);

    CHECK(query_blocked);
    CHECK(ready_while_blocked);
    CHECK(focus_advanced);
    CHECK(reassignment_while_blocked);
    CHECK(original_target_retired);
    CHECK(replacement_admitted);
    CHECK(reassignment_synchronized);
    CHECK(replacement0.target != original_assignment.target);
    CHECK(replacement0.id != original_assignment.id);
    CHECK(ready_result.load(std::memory_order_acquire) == ERROR_SUCCESS);
    CHECK(query_result.load(std::memory_order_acquire) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(same_capabilities_ex(output, output_before));
    CHECK(fixture.metadata().query_count() == 1U);
    CHECK(fixture.runtime().query_metadata(original_assignment.target).has_value() == false);
}

TEST_CASE("gameinput.presentation.blocked_native_release_keeps_focus_and_other_position_ready",
    "[integration][gameinput][presentation][concurrency]") {
    presentation_fixture fixture{2U};
    fixture.start();
    const auto baseline = fixture.focus_observer().published().completed_observations;
    fixture.native().block_next_release(0U);
    fixture.native().disconnect(0U);

    std::binary_semaphore maintenance_done{0};
    std::jthread maintenance([&] {
        fixture.runtime().maintain(fixture.now_after(1s));
        maintenance_done.release();
    });
    const bool release_blocked = fixture.native().wait_for_release();

    std::atomic<DWORD> ready_result{0xFFFF'FFFFUL};
    std::binary_semaphore ready_done{0};
    std::jthread ready_read([&] {
        XINPUT_STATE state{};
        ready_result.store(
            fixture.presentation().presentation().get_state(xinput::flavor::v1_4, 1U, &state),
            std::memory_order_release);
        ready_done.release();
    });
    const bool ready_while_blocked = release_blocked && ready_done.try_acquire_for(500ms);
    const bool focus_advanced =
        release_blocked && wait_for_focus_progress(fixture.focus_observer(), baseline);

    fixture.native().resume_release();
    const bool maintenance_finished = maintenance_done.try_acquire_for(2s);
    maintenance.join();
    ready_read.join();
    const auto inventory = fixture.runtime().connections();
    const auto synchronized = fixture.presentation().synchronize(inventory);

    CHECK(release_blocked);
    CHECK(ready_while_blocked);
    CHECK(focus_advanced);
    CHECK(maintenance_finished);
    CHECK(synchronized.has_value());
    CHECK(ready_result.load(std::memory_order_acquire) == ERROR_SUCCESS);
}

} // namespace
} // namespace inputnext::gameinput_detail
