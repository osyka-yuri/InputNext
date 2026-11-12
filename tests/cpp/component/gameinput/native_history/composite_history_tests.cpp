#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/native/native_history_api.hpp"
#include "gameinput/native/native_history_cursor.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <inputnext/gamepad_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

struct composite_record final {
    std::size_t id{};
    std::uint64_t timestamp{};
    gamepad_state pad{};
    input::touch_observation touch{};
    std::size_t references{};
    bool frame_available{true};
};

[[nodiscard]] input::touch_observation touch_reading(const std::uint64_t timestamp,
    const bool pressed,
    const float x_coordinate = 25.0F) noexcept {
    return input::touch_observation{
        .click_status = input::observation_status::observed,
        .click_pressed = pressed,
        .contacts_status = input::observation_status::observed,
        .same_reading = true,
        .contacts_complete = true,
        .surface_width = 100.0F,
        .surface_height = 100.0F,
        .contacts =
            {input::touch_contact{.id = 7U, .x = x_coordinate, .y = 30.0F, .active = pressed},
                input::touch_contact{}},
        .contact_count = pressed ? 1U : 0U,
        .source_timestamp = timestamp,
    };
}

class composite_history_api final : public native_history_api {
public:
    [[nodiscard]] std::size_t
    add(const std::uint64_t timestamp, const gamepad_state pad, input::touch_observation touch) {
        const auto id = readings_.size();
        readings_.push_back(composite_record{
            .id = id,
            .timestamp = timestamp,
            .pad = pad,
            .touch = touch,
        });
        return id;
    }

    void set_current(const std::size_t index) noexcept { current_index_ = index; }

    void retain_session(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++session_retain_count;
    }

    void release_session(native_device_handle device) noexcept override {
        static_cast<void>(device);
        ++session_release_count;
    }

    [[nodiscard]] std::uint64_t current_timestamp() noexcept override { return clock_timestamp; }

    [[nodiscard]] native_result<std::uint64_t> query_qpc() noexcept override { return next_qpc++; }

    [[nodiscard]] native_result<std::uint64_t> query_qpc_frequency() noexcept override {
        return qpc_frequency;
    }

    [[nodiscard]] native_history_read current_pad(native_device_handle device) noexcept override {
        static_cast<void>(device);
        return acquire(current_index_);
    }

    [[nodiscard]] native_history_read next_pad(native_device_handle device,
        void* reference) noexcept override {
        static_cast<void>(device);
        return adjacent(reference, 1);
    }

    [[nodiscard]] native_history_read current_composite(native_device_handle device,
        const native_input_profile& profile) noexcept override {
        static_cast<void>(device);
        if (profile.history != input_history_path::composite) {
            return native_history_read{.kind = native_history_read_kind::failure};
        }
        return acquire(current_index_);
    }

    [[nodiscard]] native_history_read previous_composite(native_device_handle device,
        void* reference,
        const native_input_profile& profile) noexcept override {
        static_cast<void>(device);
        if (profile.history != input_history_path::composite) {
            return native_history_read{.kind = native_history_read_kind::failure};
        }
        const auto* const current = static_cast<composite_record*>(reference);
        if (current != nullptr && disconnect_previous_at_id.has_value() &&
            current->id == *disconnect_previous_at_id) {
            return native_history_read{
                .kind = native_history_read_kind::disconnected,
                .native_code = disconnect_native_code,
            };
        }
        const auto history_lost = history_lost_at_id.has_value() && current != nullptr &&
                                  current->id <= *history_lost_at_id;
        if (history_lost || current == nullptr || current->id == 0U) {
            return native_history_read{
                .kind = history_lost ? native_history_read_kind::reference_too_old
                                     : native_history_read_kind::no_reading,
            };
        }
        return acquire(current->id - 1U);
    }

    [[nodiscard]] native_history_read next_composite(native_device_handle device,
        void* reference,
        const native_input_profile& profile) noexcept override {
        static_cast<void>(device);
        if (profile.history != input_history_path::composite) {
            return native_history_read{.kind = native_history_read_kind::failure};
        }
        const auto* const current = static_cast<composite_record*>(reference);
        if (current != nullptr && disconnect_next_at_id.has_value() &&
            current->id == *disconnect_next_at_id) {
            return native_history_read{
                .kind = native_history_read_kind::disconnected,
                .native_code = disconnect_native_code,
            };
        }
        if (current == nullptr ||
            (stop_forward_after.has_value() && current->id == *stop_forward_after) ||
            current->id + 1U >= readings_.size()) {
            return native_history_read{.kind = native_history_read_kind::no_reading};
        }
        if (forward_edge_count < forward_edges.size()) {
            forward_edges[forward_edge_count++] = current->id;
        }
        return acquire(current->id + 1U);
    }

    [[nodiscard]] bool same_reading(void* first, void* second) noexcept override {
        return first != nullptr && first == second;
    }

    [[nodiscard]] std::uint64_t reading_timestamp(void* reading) noexcept override {
        return static_cast<const composite_record*>(reading)->timestamp;
    }

    [[nodiscard]] native_result<gamepad_state> read_gamepad(void* reading) noexcept override {
        if (reading == nullptr) {
            return std::unexpected(native_failure{});
        }
        return static_cast<const composite_record*>(reading)->pad;
    }

    [[nodiscard]] native_result<native_history_frame> read_composite(void* reading,
        const native_input_profile& profile) noexcept override {
        if (profile.history != input_history_path::composite) {
            return std::unexpected(native_failure{});
        }
        const auto* const record = static_cast<composite_record*>(reading);
        if (record == nullptr || !record->frame_available) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
            });
        }
        return native_history_frame{.pad = record->pad, .touch = record->touch};
    }

    void release_reading(void* reading) noexcept override {
        auto* const record = static_cast<composite_record*>(reading);
        if (record == nullptr || record->references == 0U) {
            ++invalid_release_count;
            return;
        }
        --record->references;
    }

    [[nodiscard]] const composite_record& at(const std::size_t index) const noexcept {
        return readings_[index];
    }

    [[nodiscard]] std::size_t size() const noexcept { return readings_.size(); }

    std::uint64_t clock_timestamp{20U};
    std::uint64_t next_qpc{100U};
    std::uint64_t qpc_frequency{10'000'000U};
    std::size_t current_index_{};
    std::size_t session_retain_count{};
    std::size_t session_release_count{};
    std::size_t invalid_release_count{};
    std::optional<std::size_t> history_lost_at_id;
    std::optional<std::size_t> disconnect_previous_at_id;
    std::optional<std::size_t> disconnect_next_at_id;
    std::uint32_t disconnect_native_code{0xD15CU};
    std::optional<std::size_t> stop_forward_after;
    std::array<std::size_t, 256U> forward_edges{};
    std::size_t forward_edge_count{};

private:
    [[nodiscard]] native_history_read adjacent(void* reference, const int direction) noexcept {
        const auto* const current = static_cast<composite_record*>(reference);
        if (current == nullptr) {
            return native_history_read{.kind = native_history_read_kind::failure};
        }
        const auto index = static_cast<std::int64_t>(current->id) + direction;
        if (index < 0 || static_cast<std::size_t>(index) >= readings_.size()) {
            return native_history_read{.kind = native_history_read_kind::no_reading};
        }
        return acquire(static_cast<std::size_t>(index));
    }

    [[nodiscard]] native_history_read acquire(const std::size_t index) noexcept {
        if (index >= readings_.size()) {
            return native_history_read{.kind = native_history_read_kind::no_reading};
        }
        auto& reading = readings_[index];
        ++reading.references;
        return native_history_read{
            .kind = native_history_read_kind::reading,
            .reading = &reading,
        };
    }

    std::deque<composite_record> readings_;
};

[[nodiscard]] native_input_profile composite_profile() noexcept {
    return native_input_profile{
        .history = input_history_path::composite,
        .reason = input_fallback_reason::none,
        .callback_coherent = true,
        .coordinates = touch_coordinate_source::surface,
        .button_count = 1U,
        .click_button = 0U,
        .surface_index = 0U,
        .surface_width = 100U,
        .surface_height = 100U,
    };
}

[[nodiscard]] input::touch_snapshot held_context(const std::uint64_t first_timestamp) noexcept {
    return input::touch_snapshot{
        .source = input::touch_source::left,
        .click_known_pressed = true,
        .epoch = 1U,
        .first_accepted_timestamp = first_timestamp,
    };
}

[[nodiscard]] std::unique_ptr<native_history_cursor_impl> make_cursor(composite_history_api*& api,
    native_device_handle device,
    input::touch_snapshot context = {}) {
    auto scripted = std::make_unique<composite_history_api>();
    api = scripted.get();
    return std::make_unique<native_history_cursor_impl>(std::move(scripted),
        device,
        composite_profile(),
        context);
}

TEST_CASE("gameinput.native.composite_bootstrap_replays_exact_readings",
    "[component][gameinput][native][history][touch]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(15U, gamepad_state{.buttons = 1U}, touch_reading(15U, false)));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    static_cast<void>(api->add(17U, gamepad_state{.buttons = 1U}, touch_reading(17U, true, 25.0F)));
    static_cast<void>(api->add(17U, gamepad_state{.buttons = 2U}, touch_reading(17U, true, 75.0F)));
    api->set_current(3U);
    api->clock_timestamp = 17U;

    const auto seed = cursor->start();

    REQUIRE(seed.has_value());
    REQUIRE(seed->baseline.state.buttons == 2U);
    // The same contact crosses the midpoint while held; the original left-side latch survives.
    REQUIRE(seed->touch == input::touch_source::left);
    REQUIRE(seed->resolved_touch.click_known_pressed);
    REQUIRE(seed->bootstrap_transitions == 4U);
    REQUIRE(cursor->bootstrap_transitions() == seed->bootstrap_transitions);
    REQUIRE(api->forward_edge_count == 2U);
    REQUIRE(api->forward_edges[0U] == 1U);
    REQUIRE(api->forward_edges[1U] == 2U);
    REQUIRE(api->at(2U).timestamp == api->at(3U).timestamp);
    REQUIRE(&api->at(2U) != &api->at(3U));
    REQUIRE(api->at(3U).references == 1U);
    REQUIRE(api->at(1U).references == 0U);
    REQUIRE(api->at(2U).references == 0U);
    REQUIRE(api->invalid_release_count == 0U);
    REQUIRE(api->session_retain_count == 1U);
    REQUIRE(api->session_release_count == 0U);

    api->set_current(1U);
    api->clock_timestamp = 16U;
    const auto reset_seed = cursor->start();

    REQUIRE(reset_seed.has_value());
    REQUIRE(reset_seed->bootstrap_transitions == 0U);
    REQUIRE(cursor->bootstrap_transitions() == 0U);
    REQUIRE(api->at(3U).references == 0U);
    REQUIRE(api->at(1U).references == 1U);
}

TEST_CASE("gameinput.native.composite_bootstrap_requires_reset_strictly_after_t0",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(15U, gamepad_state{}, touch_reading(15U, false)));
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(1U);
    api->clock_timestamp = 20U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::history_lost);
    REQUIRE(api->at(0U).references == 0U);
    REQUIRE(api->at(1U).references == 0U);
    REQUIRE(api->invalid_release_count == 0U);
}

TEST_CASE("gameinput.native.composite_bootstrap_rejects_held_baseline_without_t0",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object});
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(0U);
    api->clock_timestamp = 20U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::incomplete_replay);
    REQUIRE(api->at(0U).references == 0U);
}

TEST_CASE("gameinput.native.composite_bootstrap_fails_closed_when_window_is_lost",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, true)));
    api->set_current(0U);
    api->clock_timestamp = 16U;
    api->history_lost_at_id = 0U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::history_lost);
    REQUIRE(cursor->bootstrap_transitions() == 0U);
    REQUIRE(api->at(0U).references == 0U);
}

TEST_CASE("gameinput.native.composite_bootstrap_reports_transitions_before_window_loss",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    static_cast<void>(api->add(18U, gamepad_state{}, touch_reading(18U, true)));
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(2U);
    api->clock_timestamp = 20U;
    api->history_lost_at_id = 1U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::history_lost);
    REQUIRE(cursor->bootstrap_transitions() == 1U);
    for (std::size_t index = 0U; index < api->size(); ++index) {
        REQUIRE(api->at(index).references == 0U);
    }
}

TEST_CASE("gameinput.native.composite_bootstrap_preserves_reverse_disconnect",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    static_cast<void>(api->add(18U, gamepad_state{}, touch_reading(18U, true)));
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(2U);
    api->clock_timestamp = 20U;
    api->disconnect_previous_at_id = 1U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().cause == native_failure_cause::device_disconnected);
    REQUIRE(seed.error().native_code == api->disconnect_native_code);
    REQUIRE(cursor->bootstrap_transitions() == 1U);
    for (std::size_t index = 0U; index < api->size(); ++index) {
        REQUIRE(api->at(index).references == 0U);
    }
}

TEST_CASE("gameinput.native.composite_bootstrap_preserves_forward_disconnect",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    static_cast<void>(api->add(18U, gamepad_state{}, touch_reading(18U, true)));
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(2U);
    api->clock_timestamp = 20U;
    api->disconnect_next_at_id = 0U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().cause == native_failure_cause::device_disconnected);
    REQUIRE(seed.error().native_code == api->disconnect_native_code);
    REQUIRE(cursor->bootstrap_transitions() == 2U);
    for (std::size_t index = 0U; index < api->size(); ++index) {
        REQUIRE(api->at(index).references == 0U);
    }
}

TEST_CASE("gameinput.native.composite_bootstrap_requires_complete_forward_replay",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    static_cast<void>(api->add(20U, gamepad_state{}, touch_reading(20U, true)));
    api->set_current(1U);
    api->clock_timestamp = 20U;
    api->stop_forward_after = 0U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::incomplete_replay);
    REQUIRE(api->at(0U).references == 0U);
    REQUIRE(api->at(1U).references == 0U);
}

TEST_CASE("gameinput.native.composite_bootstrap_counts_reverse_and_forward_budget_together",
    "[component][gameinput][native][history][touch][failure]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object}, held_context(15U));
    static_cast<void>(api->add(16U, gamepad_state{}, touch_reading(16U, false)));
    for (std::size_t index = 0U; index < 129U; ++index) {
        static_cast<void>(api->add(17U,
            gamepad_state{},
            touch_reading(17U, true, (index % 2U) == 0U ? 25.0F : 75.0F)));
    }
    api->set_current(api->size() - 1U);
    api->clock_timestamp = 17U;

    const auto seed = cursor->start();

    REQUIRE_FALSE(seed.has_value());
    REQUIRE(seed.error().input_reason == input_fallback_reason::bootstrap_budget);
    REQUIRE(cursor->bootstrap_transitions() == 256U);
    for (std::size_t index = 0U; index < api->size(); ++index) {
        REQUIRE(api->at(index).references == 0U);
    }
    REQUIRE(api->invalid_release_count == 0U);
}

TEST_CASE("gameinput.native.composite_next_rejects_missing_click_but_keeps_optional_coordinates",
    "[component][gameinput][native][history][touch]") {
    int device_object{};
    composite_history_api* api{};
    const auto cursor = make_cursor(api, native_device_handle{&device_object});
    static_cast<void>(api->add(10U, gamepad_state{}, touch_reading(10U, false)));
    auto press = touch_reading(11U, true);
    press.contacts_status = input::observation_status::unavailable;
    press.contacts_complete = false;
    static_cast<void>(api->add(11U, gamepad_state{.buttons = 4U}, press));
    api->set_current(0U);
    api->clock_timestamp = 11U;

    const auto seed = cursor->start();
    REQUIRE(seed.has_value());
    const auto step = cursor->next();
    REQUIRE(step.kind == native_history_step_kind::reading);
    REQUIRE(step.reading.state.buttons == 4U);
    REQUIRE(step.touch == input::touch_source::fallback);

    auto unavailable_click = touch_reading(12U, true);
    unavailable_click.click_status = input::observation_status::unavailable;
    static_cast<void>(api->add(12U, gamepad_state{.buttons = 8U}, unavailable_click));
    api->clock_timestamp = 12U;
    const auto missing_click = cursor->next();
    REQUIRE(missing_click.kind == native_history_step_kind::incomplete_replay);
    REQUIRE(missing_click.fallback_reason == input_fallback_reason::missing_click);
    REQUIRE(api->at(1U).references == 1U);
}

} // namespace
} // namespace inputnext::gameinput_detail
