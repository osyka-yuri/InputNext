#include "gameinput/native/native_history_cursor.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/native/native_history_api.hpp"
#include "gameinput/native/windows_history_cursor.hpp"
#include <inputnext/gamepad_state.hpp>

#include <GameInput.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

struct scripted_reading final {
    std::uint64_t timestamp_us{};
    gamepad_state state{};
    std::size_t references{};
};

struct scripted_next_action final {
    native_history_read_kind kind{native_history_read_kind::no_reading};
    std::size_t reading_index{};
    std::uint32_t native_code{};
};

struct scripted_history_data final {
    static constexpr std::size_t reading_capacity = 8U;
    static constexpr std::size_t action_capacity = 16U;

    std::array<scripted_reading, reading_capacity> readings{};
    std::array<scripted_next_action, action_capacity> actions{};
    std::array<std::size_t, action_capacity> next_reference_ids{};
    std::size_t reading_count{};
    std::size_t next_reading_calls{};
    std::size_t current_reading_calls{};
    std::size_t action_count{};
    std::size_t action_offset{};
    std::size_t session_retain_count{};
    std::size_t session_release_count{};
    std::size_t invalid_release_count{};
};

class scripted_native_history_api final : public native_history_api {
public:
    explicit scripted_native_history_api(scripted_history_data& data) : data_(data) {
        static_cast<void>(add_reading(1'000U, gamepad_state{.buttons = 1U}));
    }

    [[nodiscard]] std::size_t add_reading(std::uint64_t timestamp_us, gamepad_state state) {
        const auto index = data_.reading_count++;
        data_.readings[index] = scripted_reading{.timestamp_us = timestamp_us, .state = state};
        return index;
    }

    void queue_next(scripted_next_action action) { data_.actions[data_.action_count++] = action; }

    void retain_session(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++data_.session_retain_count;
    }

    void release_session(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++data_.session_release_count;
    }

    [[nodiscard]] std::uint64_t current_timestamp() noexcept override {
        return current_timestamp_us;
    }

    [[nodiscard]] native_result<std::uint64_t> query_qpc() noexcept override {
        if (qpc_error.has_value()) {
            return std::unexpected(*qpc_error);
        }
        return next_qpc++;
    }

    [[nodiscard]] native_result<std::uint64_t> query_qpc_frequency() noexcept override {
        if (qpc_frequency_error.has_value()) {
            return std::unexpected(*qpc_frequency_error);
        }
        return qpc_frequency;
    }

    [[nodiscard]] native_history_read current_pad(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++data_.current_reading_calls;
        if (!current_reading_available) {
            return native_history_read{
                .kind = native_history_read_kind::no_reading,
                .native_code = no_reading_code,
            };
        }
        return acquired_reading(native_history_read_kind::reading, current_reading_index);
    }

    [[nodiscard]] native_history_read next_pad(native_device_handle device,
        void* reference) noexcept override {
        static_cast<void>(device);
        const auto* const prior = static_cast<scripted_reading*>(reference);
        data_.next_reference_ids[data_.next_reading_calls] = reading_id(prior);
        ++data_.next_reading_calls;
        if (data_.action_offset == data_.action_count) {
            return native_history_read{.kind = native_history_read_kind::no_reading};
        }

        const auto action = data_.actions[data_.action_offset++];
        if (action.kind != native_history_read_kind::reading) {
            return native_history_read{
                .kind = action.kind,
                .native_code = action.native_code,
            };
        }
        return acquired_reading(action.kind, action.reading_index, action.native_code);
    }

    [[nodiscard]] std::uint64_t reading_timestamp(void* reading) noexcept override {
        return static_cast<scripted_reading*>(reading)->timestamp_us;
    }

    [[nodiscard]] native_result<gamepad_state> read_gamepad(void* reading) noexcept override {
        return static_cast<scripted_reading*>(reading)->state;
    }

    void release_reading(void* reading) noexcept override {
        auto* const value = static_cast<scripted_reading*>(reading);
        if (value->references == 0U) {
            ++data_.invalid_release_count;
            return;
        }
        --value->references;
    }

    [[nodiscard]] std::size_t reading_id(const scripted_reading* reading) const noexcept {
        return static_cast<std::size_t>(reading - data_.readings.data());
    }

    std::size_t current_reading_index{};
    std::uint64_t current_timestamp_us{1'000U};
    std::uint64_t next_qpc{100U};
    std::uint64_t qpc_frequency{10'000'000U};
    std::uint32_t no_reading_code{};
    bool current_reading_available{true};
    std::optional<native_failure> qpc_error;
    std::optional<native_failure> qpc_frequency_error;

private:
    [[nodiscard]] native_history_read acquired_reading(native_history_read_kind kind,
        std::size_t index,
        std::uint32_t native_code = 0U) noexcept {
        ++data_.readings[index].references;
        return native_history_read{
            .kind = kind,
            .reading = &data_.readings[index],
            .native_code = native_code,
        };
    }

    scripted_history_data& data_;
};

[[nodiscard]] std::unique_ptr<native_history_cursor_impl> make_cursor(
    scripted_native_history_api*& api,
    scripted_history_data& data,
    native_device_handle device) {
    auto scripted = std::make_unique<scripted_native_history_api>(data);
    api = scripted.get();
    return std::make_unique<native_history_cursor_impl>(std::move(scripted), device);
}

TEST_CASE("gameinput.history.native_cursor_keeps_equal_time_readings_ordered",
    "[component][gameinput][native][history]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});

    const auto baseline = cursor->start();
    REQUIRE(baseline.has_value());
    REQUIRE(baseline->baseline.state.buttons == 1U);
    REQUIRE(baseline->baseline.observation_qpc == 102U);

    const auto first_index = api->add_reading(1'000U, gamepad_state{.buttons = 2U});
    const auto second_index = api->add_reading(1'000U, gamepad_state{.buttons = 4U});
    api->queue_next({.kind = native_history_read_kind::reading, .reading_index = first_index});
    api->queue_next({.kind = native_history_read_kind::reading, .reading_index = second_index});

    const auto first = cursor->next();
    const auto second = cursor->next();

    REQUIRE(first.kind == native_history_step_kind::reading);
    REQUIRE(second.kind == native_history_step_kind::reading);
    REQUIRE(first.reading.state.buttons == 2U);
    REQUIRE(second.reading.state.buttons == 4U);
    REQUIRE(first.reading.observation_qpc == 102U);
    REQUIRE(second.reading.observation_qpc == 102U);
    REQUIRE(data.next_reference_ids[0U] == 0U);
    REQUIRE(data.next_reference_ids[1U] == first_index);
}

TEST_CASE("gameinput.history.native_cursor_retries_prefix_failure_from_same_reference",
    "[component][gameinput][native][history][failure]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    REQUIRE(cursor->start().has_value());

    constexpr std::uint32_t scripted_error = 0x80004005U;
    const auto next_index = api->add_reading(1'001U, gamepad_state{.buttons = 8U});
    api->queue_next({
        .kind = native_history_read_kind::failure,
        .native_code = scripted_error,
    });
    api->queue_next({.kind = native_history_read_kind::reading, .reading_index = next_index});

    const auto failed = cursor->next();
    const auto retried = cursor->next();

    REQUIRE(failed.kind == native_history_step_kind::provider_failure);
    REQUIRE(failed.native_code == scripted_error);
    REQUIRE(retried.kind == native_history_step_kind::reading);
    REQUIRE(retried.reading.state.buttons == 8U);
    REQUIRE(data.next_reference_ids[0U] == 0U);
    REQUIRE(data.next_reference_ids[1U] == 0U);
    REQUIRE(data.readings[0U].references == 0U);
    REQUIRE(data.readings[next_index].references == 1U);
}

TEST_CASE("gameinput.history.native_cursor_maps_empty_and_old_reference_results",
    "[component][gameinput][native][history]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    REQUIRE(cursor->start().has_value());

    const auto empty = cursor->next();
    constexpr std::uint32_t scripted_error = 0x838A0004U;
    api->queue_next({
        .kind = native_history_read_kind::reference_too_old,
        .native_code = scripted_error,
    });
    const auto lost = cursor->next();

    REQUIRE(empty.kind == native_history_step_kind::caught_up);
    REQUIRE(empty.checkpoint_qpc == 102U);
    REQUIRE(lost.kind == native_history_step_kind::history_lost);
    REQUIRE(lost.native_code == scripted_error);
    REQUIRE(data.readings[0U].references == 1U);
}

TEST_CASE("gameinput.history.native_cursor_keeps_reference_after_invalid_reading",
    "[component][gameinput][native][history][failure]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    REQUIRE(cursor->start().has_value());

    const auto invalid_index = api->add_reading(1'001U, gamepad_state{.left_trigger = 2.0F});
    const auto valid_index = api->add_reading(1'002U, gamepad_state{.buttons = 16U});
    api->queue_next({.kind = native_history_read_kind::reading, .reading_index = invalid_index});
    api->queue_next({.kind = native_history_read_kind::reading, .reading_index = valid_index});

    const auto invalid = cursor->next();
    const auto recovered = cursor->next();

    REQUIRE(invalid.kind == native_history_step_kind::provider_failure);
    REQUIRE(recovered.kind == native_history_step_kind::reading);
    REQUIRE(recovered.reading.state.buttons == 16U);
    REQUIRE(data.next_reference_ids[0U] == 0U);
    REQUIRE(data.next_reference_ids[1U] == 0U);
    REQUIRE(data.invalid_release_count == 0U);
}

TEST_CASE("gameinput.history.native_cursor_rejects_clock_rollback_without_advancing",
    "[component][gameinput][native][history][failure]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    REQUIRE(cursor->start().has_value());

    api->current_timestamp_us = 999U;
    const auto discontinuity = cursor->next();
    api->current_timestamp_us = 1'001U;
    const auto caught_up = cursor->next();

    REQUIRE(discontinuity.kind == native_history_step_kind::clock_discontinuity);
    REQUIRE(caught_up.kind == native_history_step_kind::caught_up);
    REQUIRE(data.next_reading_calls == 1U);
    REQUIRE(data.next_reference_ids[0U] == 0U);
}

TEST_CASE("gameinput.history.native_cursor_releases_native_references_with_session",
    "[component][gameinput][native][history][lifecycle]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    REQUIRE(cursor->start().has_value());
    REQUIRE(data.session_retain_count == 1U);
    REQUIRE(data.readings[0U].references == 1U);

    cursor.reset();

    REQUIRE(data.session_release_count == 1U);
    REQUIRE(data.readings[0U].references == 0U);
    REQUIRE(data.invalid_release_count == 0U);
}

TEST_CASE("gameinput.history.native_cursor_preserves_clock_errors_on_start",
    "[component][gameinput][native][history][failure]") {
    int device_object{};
    scripted_history_data data{};
    scripted_native_history_api* api{};
    const auto cursor = make_cursor(api, data, native_device_handle{&device_object});
    api->qpc_error = native_failure{
        .cause = native_failure_cause::clock_discontinuity,
        .native_code = 0x80004005U,
    };

    const auto failed = cursor->start();

    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().cause == native_failure_cause::clock_discontinuity);
    REQUIRE(failed.error().native_code == 0x80004005U);
    REQUIRE(data.session_retain_count == 1U);
    REQUIRE(data.session_release_count == 0U);
    REQUIRE(data.readings[0U].references == 0U);
}

TEST_CASE("gameinput.history.native_adapter_classifies_sdk_end_and_loss_codes",
    "[component][gameinput][native][history]") {
    namespace gameinput = GameInput::v4;

    REQUIRE(classify_history_failure(static_cast<std::uint32_t>(
                gameinput::GAMEINPUT_E_READING_NOT_FOUND)) == native_history_read_kind::no_reading);
    REQUIRE(classify_history_failure(
                static_cast<std::uint32_t>(gameinput::GAMEINPUT_E_REFERENCE_READING_TOO_OLD)) ==
            native_history_read_kind::reference_too_old);
    REQUIRE(classify_history_failure(
                static_cast<std::uint32_t>(gameinput::GAMEINPUT_E_DEVICE_DISCONNECTED)) ==
            native_history_read_kind::disconnected);
    REQUIRE(classify_history_failure(static_cast<std::uint32_t>(E_FAIL)) ==
            native_history_read_kind::failure);
}

} // namespace
} // namespace inputnext::gameinput_detail
