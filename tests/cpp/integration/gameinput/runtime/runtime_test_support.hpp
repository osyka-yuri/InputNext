#pragma once

#include "gameinput/reading_parts.hpp"
#include "gameinput/runtime/factory.hpp"
#include "metadata/provider.hpp"
#include "runtime_metadata_test_support.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>

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
#include <string>
#include <utility>

#include <gameinput/native/native_gameinput.hpp>

namespace inputnext::gameinput_test_support {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

constexpr auto query_wait = std::chrono::seconds{2};

[[nodiscard]] inline runtime_id test_runtime_id() {
    runtime_id result{};
    result.bytes[0U] = std::byte{0x51};
    return result;
}

[[nodiscard]] inline gamepad_state test_pad(button_mask buttons, float axis = 0.25F) {
    return gamepad_state{
        .buttons = buttons,
        .left_stick = {.x = axis, .y = -axis},
        .right_stick = {.x = -axis, .y = axis},
        .left_trigger = axis,
        .right_trigger = 1.0F - axis,
    };
}

[[nodiscard]] inline reading_parts
pad_parts(gamepad_state state, std::uint64_t timestamp, std::uint64_t qpc) {
    return reading_parts{
        .pad = state,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
    };
}

[[nodiscard]] inline metadata_field<native_guid> available_container() {
    native_guid value{};
    value.bytes[0U] = std::byte{0x7A};
    return metadata_field<native_guid>{.availability = metadata_availability::available,
        .value = value};
}

class fake_gameinput final : public inputnext::gameinput_detail::native_gameinput {
public:
    enum class registration_kind : std::uint8_t { device_watch, rescan, reading };

    struct registration final {
        native_callback_token token{};
        registration_kind kind{registration_kind::device_watch};
        native_device_handle device{};
        native_device_callback_context* device_context{};
        native_reading_callback_context* reading_context{};
        std::size_t inflight{};
        bool active{};
    };

    struct device_state final {
        native_device_handle handle{};
        bool connected{};
        std::optional<native_device_facts> facts;
        std::optional<reading_parts> gamepad_reading;
        std::optional<reading_parts> system_button_reading;
    };

    static constexpr std::size_t device_capacity = 144U;
    static constexpr std::size_t registration_capacity = 256U;

    [[nodiscard]] native_result<void> initialize(
        const std::filesystem::path& runtime_library) override {
        std::scoped_lock lock{mutex_};
        initialized = runtime_library.is_absolute();
        if (!initialized) {
            return std::unexpected(native_failure{.cause = native_failure_cause::load_failed});
        }
        return {};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        {
            std::scoped_lock lock{mutex_};
            if (watch_registration_partial_failure) {
                watch_registration_partial_failure = false;
                const auto token = add_registration(registration{
                    .kind = registration_kind::device_watch,
                    .device_context = &context,
                    .active = true,
                });
                return std::unexpected(native_failure{
                    .cause = native_failure_cause::registration_failed,
                    .partial_token = token,
                });
            }
        }
        return register_device_callback(registration_kind::device_watch, context);
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        {
            std::scoped_lock lock{mutex_};
            ++rescan_registration_attempts;
            if (rescan_registration_failures > 0U) {
                --rescan_registration_failures;
                return std::unexpected(
                    native_failure{.cause = native_failure_cause::registration_failed});
            }
        }
        auto registered = register_device_callback(registration_kind::rescan, context);
        if (!registered.has_value()) {
            return std::unexpected(registered.error());
        }

        std::array<native_device_handle, device_capacity> connected_devices{};
        std::size_t count{};
        {
            std::scoped_lock lock{mutex_};
            for (const auto& device : devices) {
                if (device.connected) {
                    connected_devices[count++] = device.handle;
                }
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            context.deliver(context.owner,
                connected_devices[index],
                native_device_event::connected);
        }
        return *registered;
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) override {
        std::optional<reading_parts> inline_reading;
        native_callback_token token{};
        {
            std::scoped_lock lock{mutex_};
            ++reading_registration_attempts;
            if (reading_registration_failures > 0U) {
                --reading_registration_failures;
                return std::unexpected(
                    native_failure{.cause = native_failure_cause::registration_failed});
            }
            if (reading_registration_partial_failures > 0U) {
                --reading_registration_partial_failures;
                token = add_registration(registration{
                    .kind = registration_kind::reading,
                    .device = device,
                    .reading_context = &context,
                    .active = true,
                });
                return std::unexpected(native_failure{
                    .cause = native_failure_cause::registration_failed,
                    .partial_token = token,
                });
            }
            token = add_registration(registration{
                .kind = registration_kind::reading,
                .device = device,
                .reading_context = &context,
                .active = true,
            });
            inline_reading = std::exchange(reading_during_registration, std::nullopt);
        }

        if (inline_reading.has_value()) {
            context.deliver(context.owner, *inline_reading);
        }
        return token;
    }

    [[nodiscard]] bool unregister_callback(native_callback_token token) noexcept override {
        std::scoped_lock lock{mutex_};
        ++unregister_attempts;
        if (unregister_failures > 0U) {
            --unregister_failures;
            return false;
        }
        const auto found = std::ranges::find_if(registrations,
            [token](const auto& item) { return item.active && item.token == token; });
        if (found == registrations.end() || found->inflight > 0U) {
            return false;
        }
        found->active = false;
        return true;
    }

    [[nodiscard]] native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) override {
        std::scoped_lock lock{mutex_};
        ++current_calls;
        const auto* state = find_device(devices, device);
        const std::optional<reading_parts>* current{};
        if (state != nullptr) {
            current = lane == native_reading_lane::gamepad ? &state->gamepad_reading
                                                           : &state->system_button_reading;
        }
        if (current == nullptr || !current->has_value()) {
            return std::unexpected(native_failure{.cause = native_failure_cause::no_current_reading,
                .native_code = 0x838A0003U});
        }
        return **current;
    }

    [[nodiscard]] native_result<native_device_facts> facts(native_device_handle device) override {
        native_device_facts result{};
        {
            std::scoped_lock lock{mutex_};
            ++facts_calls;
            const auto* state = find_device(devices, device);
            if (state == nullptr) {
                return std::unexpected(
                    native_failure{.cause = native_failure_cause::device_disconnected});
            }
            if (facts_failures > 0U) {
                --facts_failures;
                return std::unexpected(
                    native_failure{.cause = native_failure_cause::native_failure});
            }
            result = state->facts.has_value() ? *state->facts : device_facts;
        }
        if (block_next_facts_.exchange(false, std::memory_order_acq_rel)) {
            facts_blocked_.release();
            facts_release_.acquire();
        }
        return result;
    }

    [[nodiscard]] bool connected(native_device_handle device) noexcept override {
        std::scoped_lock lock{mutex_};
        const auto* state = find_device(devices, device);
        return state != nullptr && state->connected;
    }

    void retain(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++retained_devices;
    }

    void release(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++released_devices;
    }

    void set_connected(native_device_handle handle, bool value) {
        std::scoped_lock lock{mutex_};
        auto* state = ensure_device(handle);
        state->connected = value;
    }

    void set_current(native_device_handle handle, native_reading_lane lane, reading_parts parts) {
        std::scoped_lock lock{mutex_};
        auto* state = ensure_device(handle);
        if (state == nullptr) {
            return;
        }
        auto& current = lane == native_reading_lane::gamepad ? state->gamepad_reading
                                                             : state->system_button_reading;
        current = parts;
    }

    void update_facts(native_device_facts facts) {
        std::scoped_lock lock{mutex_};
        device_facts = std::move(facts);
    }

    void update_facts(native_device_handle device, native_device_facts facts) {
        std::scoped_lock lock{mutex_};
        if (auto* state = ensure_device(device); state != nullptr) {
            state->facts = std::move(facts);
        }
    }

    void emit(native_device_handle handle, native_device_event event) {
        std::array<native_device_callback_context*, registration_capacity> callbacks{};
        std::size_t count{};
        {
            std::scoped_lock lock{mutex_};
            if (event == native_device_event::connected) {
                if (auto* state = ensure_device(handle); state != nullptr) {
                    state->connected = true;
                }
            } else if (event == native_device_event::disconnected) {
                if (auto* state = find_device(devices, handle); state != nullptr) {
                    state->connected = false;
                }
            }
            for (const auto& item : registrations) {
                if (item.active && item.kind != registration_kind::reading &&
                    item.device_context != nullptr) {
                    callbacks[count++] = item.device_context;
                }
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            callbacks[index]->deliver(callbacks[index]->owner, handle, event);
        }
    }

    void publish(native_device_handle device, const reading_parts& parts) {
        std::array<native_reading_callback_context*, registration_capacity> callbacks{};
        std::size_t count{};
        {
            std::scoped_lock lock{mutex_};
            for (const auto& item : registrations) {
                if (item.active && item.kind == registration_kind::reading &&
                    item.device == device && item.reading_context != nullptr) {
                    callbacks[count++] = item.reading_context;
                }
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            callbacks[index]->deliver(callbacks[index]->owner, parts);
        }
    }

    [[nodiscard]] bool wait_for_blocked_reading_dispatch() {
        return reading_dispatch_entered_.try_acquire_for(query_wait);
    }

    void release_reading_dispatch() noexcept { reading_dispatch_release_.release(); }

    void publish_blocked(native_device_handle device, const reading_parts& parts) {
        registration* callback{};
        {
            std::scoped_lock lock{mutex_};
            const auto found = std::ranges::find_if(registrations, [device](const auto& item) {
                return item.active && item.kind == registration_kind::reading &&
                       item.device == device;
            });
            if (found == registrations.end()) {
                return;
            }
            callback = &*found;
            ++callback->inflight;
        }

        reading_dispatch_entered_.release();
        reading_dispatch_release_.acquire();
        callback->reading_context->deliver(callback->reading_context->owner, parts);

        std::scoped_lock lock{mutex_};
        --callback->inflight;
    }

    void fail_next_unregisters(std::size_t count) {
        std::scoped_lock lock{mutex_};
        unregister_failures = count;
    }

    void fail_next_reading_registrations(std::size_t count) {
        std::scoped_lock lock{mutex_};
        reading_registration_failures = count;
    }

    void fail_next_reading_registration_with_partial_token() {
        std::scoped_lock lock{mutex_};
        ++reading_registration_partial_failures;
    }

    void fail_next_facts(std::size_t count) {
        std::scoped_lock lock{mutex_};
        facts_failures = count;
    }

    void fail_next_rescan_registrations(std::size_t count) {
        std::scoped_lock lock{mutex_};
        rescan_registration_failures = count;
    }

    void fail_next_watch_registration_with_partial_token() {
        std::scoped_lock lock{mutex_};
        watch_registration_partial_failure = true;
    }

    void callback_on_next_reading_registration(reading_parts parts) {
        std::scoped_lock lock{mutex_};
        reading_during_registration = parts;
    }

    void block_next_facts() noexcept { block_next_facts_.store(true, std::memory_order_release); }

    [[nodiscard]] bool wait_for_blocked_facts() {
        return facts_blocked_.try_acquire_for(query_wait);
    }

    void release_facts() noexcept { facts_release_.release(); }

    [[nodiscard]] std::size_t active_registrations(registration_kind kind) {
        std::scoped_lock lock{mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(registrations,
            [kind](const auto& item) { return item.active && item.kind == kind; }));
    }

    [[nodiscard]] bool has_active_reading(native_device_handle device) {
        std::scoped_lock lock{mutex_};
        return std::ranges::any_of(registrations, [device](const auto& item) {
            return item.active && item.kind == registration_kind::reading && item.device == device;
        });
    }

    bool initialized{};
    std::size_t reading_registration_attempts{};
    std::size_t current_calls{};
    std::size_t facts_calls{};
    std::size_t rescan_registration_attempts{};
    std::size_t unregister_attempts{};
    std::size_t reading_registration_partial_failures{};
    bool watch_registration_partial_failure{};
    std::atomic<std::size_t> retained_devices;
    std::atomic<std::size_t> released_devices;
    native_device_facts device_facts{};

private:
    [[nodiscard]] native_result<native_callback_token>
    register_device_callback(registration_kind kind, native_device_callback_context& context) {
        std::scoped_lock lock{mutex_};
        return add_registration(registration{
            .kind = kind,
            .device_context = &context,
            .active = true,
        });
    }

    [[nodiscard]] native_callback_token add_registration(registration value) {
        const auto free =
            std::ranges::find_if(registrations, [](const auto& item) { return !item.active; });
        if (free == registrations.end()) {
            return {};
        }
        value.token = native_callback_token{next_token++};
        *free = value;
        return free->token;
    }

    [[nodiscard]] device_state* ensure_device(native_device_handle handle) {
        if (auto* found = find_device(devices, handle); found != nullptr) {
            return found;
        }
        const auto free =
            std::ranges::find_if(devices, [](const auto& item) { return !item.handle.valid(); });
        if (free == devices.end()) {
            return nullptr;
        }
        free->handle = handle;
        return &*free;
    }

    [[nodiscard]] static device_state* find_device(std::span<device_state> values,
        native_device_handle handle) {
        const auto found = std::ranges::find_if(values,
            [handle](const auto& item) { return item.handle == handle; });
        return found == values.end() ? nullptr : &*found;
    }

    std::mutex mutex_;
    std::array<device_state, device_capacity> devices{};
    std::array<registration, registration_capacity> registrations{};
    std::uint64_t next_token{1U};
    std::size_t unregister_failures{};
    std::size_t reading_registration_failures{};
    std::size_t facts_failures{};
    std::size_t rescan_registration_failures{};
    std::optional<reading_parts> reading_during_registration;
    std::atomic<bool> block_next_facts_;
    std::counting_semaphore<1> facts_blocked_{0};
    std::counting_semaphore<1> facts_release_{0};
    std::counting_semaphore<1> reading_dispatch_entered_{0};
    std::counting_semaphore<1> reading_dispatch_release_{0};
};

class scoped_facts_release final {
public:
    explicit scoped_facts_release(fake_gameinput& native) noexcept : native_(native) {}

    ~scoped_facts_release() { release(); }

    scoped_facts_release(const scoped_facts_release&) = delete;
    scoped_facts_release& operator=(const scoped_facts_release&) = delete;
    scoped_facts_release(scoped_facts_release&&) = delete;
    scoped_facts_release& operator=(scoped_facts_release&&) = delete;

    void release() noexcept {
        if (active_) {
            native_.release_facts();
            active_ = false;
        }
    }

private:
    fake_gameinput& native_;
    bool active_{true};
};

class scoped_reading_dispatch_release final {
public:
    explicit scoped_reading_dispatch_release(fake_gameinput& native) noexcept : native_(native) {}

    ~scoped_reading_dispatch_release() { release(); }

    scoped_reading_dispatch_release(const scoped_reading_dispatch_release&) = delete;
    scoped_reading_dispatch_release& operator=(const scoped_reading_dispatch_release&) = delete;
    scoped_reading_dispatch_release(scoped_reading_dispatch_release&&) = delete;
    scoped_reading_dispatch_release& operator=(scoped_reading_dispatch_release&&) = delete;

    void release() noexcept {
        if (active_) {
            native_.release_reading_dispatch();
            active_ = false;
        }
    }

private:
    fake_gameinput& native_;
    bool active_{true};
};

class runtime_fixture final {
public:
    runtime_fixture() : registry(test_runtime_id()) {
        auto fake_native = std::make_unique<fake_gameinput>();
        native = fake_native.get();
        native->device_facts = native_facts(u"old-endpoint");
        std::unique_ptr<inputnext::gameinput_detail::native_gameinput> native_port =
            std::move(fake_native);

        auto fake_metadata = std::make_unique<fake_metadata_provider>();
        metadata = fake_metadata.get();
        std::unique_ptr<inputnext::metadata_provider> metadata_port = std::move(fake_metadata);
        runtime = runtime_factory::create(registry,
            gameinput_options{.runtime_library = L"C:\\fixture\\GameInputRedist.dll"},
            std::move(native_port),
            std::move(metadata_port));
    }

    [[nodiscard]] static native_device_facts native_facts(std::u16string endpoint) {
        native_device_facts result{};
        result.vendor_id =
            metadata_field<std::uint16_t>{.availability = metadata_availability::available,
                .value = 0x054CU};
        result.product_id =
            metadata_field<std::uint16_t>{.availability = metadata_availability::available,
                .value = 0x0CE6U};
        result.container_id = available_container();
        result.haptic_endpoint_id = metadata_field<std::u16string>{
            .availability = metadata_availability::available,
            .value = std::move(endpoint),
        };
        return result;
    }

    [[nodiscard]] gameinput_result<void> start() const { return runtime->start(now); }

    void connect(native_device_handle device) const {
        native->emit(device, native_device_event::connected);
    }

    void maintain() {
        runtime->maintain(now);
        now += std::chrono::milliseconds{4};
    }

    [[nodiscard]] std::size_t connection_count() const { return runtime->connections().size; }

    [[nodiscard]] gameinput_connection connection(std::size_t index = 0U) const {
        return runtime->connections().entries[index];
    }

    std::array<int, 129U> device_memory{};
    device_registry registry;
    std::unique_ptr<gameinput_runtime> runtime;
    fake_gameinput* native{};
    fake_metadata_provider* metadata{};
    maintenance_time now{std::chrono::steady_clock::now()};
};

[[nodiscard]] inline native_device_handle handle(int& object) noexcept {
    return native_device_handle{&object};
}

} // namespace inputnext::gameinput_test_support
