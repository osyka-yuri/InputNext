#include "source_history_test_support.hpp"

#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include "history/store.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>
#include <history_evidence.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

class source_history_provider final : public history_detail::history_provider {
public:
    explicit source_history_provider(cursor_fixture& fixture) noexcept : fixture_(fixture) {}

    [[nodiscard]] history_result<std::unique_ptr<history_detail::history_cursor>> open_cursor(
        connection_target target) override {
        if (!target.valid()) {
            return std::unexpected(history_failure{.code = history_error::invalid_argument});
        }
        auto cursor = std::make_unique<source_history_cursor>(fixture_.native,
            native_device_handle{&fixture_.device},
            *fixture_.capture);
        source_history_cursor_test_access::use_scripted_clock(*cursor);
        return std::unique_ptr<history_detail::history_cursor>{std::move(cursor)};
    }

    [[nodiscard]] bool is_live(connection_target target) noexcept override {
        return target.valid();
    }

private:
    cursor_fixture& fixture_;
};

[[nodiscard]] connection_target source_cursor_target() noexcept {
    runtime_id runtime{};
    runtime.bytes[0U] = std::byte{0x51};
    return connection_target{
        .runtime = runtime,
        .device = device_key{.value = 51U},
        .connection = connection_id{.value = 51U},
    };
}

TEST_CASE("gameinput.history.store_and_evidence_reject_guide_only_native_progress",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.native.history.steps[0U] = provider_failure_step(17U);
    fixture.native.history.step_count = 1U;
    auto provider = source_history_provider{fixture};
    auto store = history_detail::history_store{provider};
    const auto target = source_cursor_target();

    const auto opened = store.open(target, history_time{});
    REQUIRE(opened.has_value());
    const auto baseline = store.read(*opened, history_time{});
    REQUIRE(baseline.count == 1U);
    CHECK(baseline.source == history_source_kind::native_history);
    CHECK(baseline.records[0U].baseline);
    CHECK(baseline.records[0U].origin == history_record_origin::baseline);

    fixture.capture->on_callback(reading_parts{
        .guide = true,
        .source_timestamp_us = 110U,
        .observation_qpc = 110U,
    });
    const auto guide_prefix = store.read(*opened, history_time{});
    REQUIRE(guide_prefix.count == 1U);
    CHECK(guide_prefix.source == history_source_kind::native_history);
    CHECK(guide_prefix.records[0U].origin == history_record_origin::callback_guide);
    REQUIRE(guide_prefix.failure.has_value());
    const auto reported_failure = guide_prefix.failure.value_or(history_failure{});
    CHECK(reported_failure.code == history_error::provider_failure);
    CHECK(reported_failure.native_code == 17U);
    CHECK_FALSE(guide_prefix.caught_up);
    CHECK(guide_prefix.checkpoint_qpc == 0U);

    auto evidence = history_evidence{};
    evidence.observe(baseline);
    evidence.observe(guide_prefix);
    CHECK(evidence.native_baseline);
    CHECK(evidence.readings == 1U);
    CHECK(evidence.native_readings == 0U);
    CHECK_FALSE(evidence.native_progress);
    CHECK_FALSE(evidence.native_history_observed());
}

TEST_CASE("gameinput.history.store_preserves_mixed_origins_and_selected_baselines",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide();
    const auto pad = static_cast<button_mask>(gamepad_button::south);
    fixture.native.history.seed.baseline.state.buttons = pad;
    fixture.native.history.seed.baseline.source_timestamp_us = 100U;
    fixture.native.history.seed.baseline.observation_qpc = 100U;
    fixture.native.history.steps[0U] = reading_step(pad, 100U, 100U);
    fixture.native.history.step_count = 1U;
    fixture.native.history.quiet_checkpoint_qpc = 100U;
    auto provider = source_history_provider{fixture};
    const auto store_storage = std::make_unique<history_detail::history_store>(provider);
    auto& store = *store_storage;
    const auto target = source_cursor_target();

    const auto first_open = store.open(target, history_time{});
    REQUIRE(first_open.has_value());
    const auto baseline = std::make_unique<history_batch>(store.read(*first_open, history_time{}));
    REQUIRE(baseline->count == 1U);
    CHECK(baseline->records[0U].origin == history_record_origin::baseline);

    fixture.capture->on_callback(reading_parts{
        .guide = false,
        .source_timestamp_us = 100U,
        .observation_qpc = 99U,
    });
    const auto mixed = std::make_unique<history_batch>(store.read(*first_open, history_time{}));
    REQUIRE(mixed->count == 2U);
    CHECK(mixed->records[0U].origin == history_record_origin::callback_guide);
    CHECK(mixed->records[1U].origin == history_record_origin::native_pad);
    CHECK(mixed->records[1U].sample.source_timestamp_us ==
          baseline->records[0U].sample.source_timestamp_us);
    CHECK(mixed->records[1U].sample.effective_qpc == baseline->records[0U].sample.effective_qpc);
    CHECK(mixed->records[1U].sample.state.buttons == baseline->records[0U].sample.state.buttons);

    auto first_evidence = history_evidence{};
    first_evidence.observe(*baseline);
    first_evidence.observe(*mixed);
    CHECK(first_evidence.native_readings == 1U);
    CHECK(first_evidence.native_progress);
    CHECK(first_evidence.native_history_observed());

    const auto second_open = store.open(target, history_time{});
    REQUIRE(second_open.has_value());
    const auto selected_baseline =
        std::make_unique<history_batch>(store.read(*second_open, history_time{}));
    REQUIRE(selected_baseline->count == 1U);
    CHECK(selected_baseline->records[0U].baseline);
    CHECK(selected_baseline->records[0U].origin == history_record_origin::baseline);
    CHECK(selected_baseline->records[0U].sample.state.buttons == pad);

    auto selected_evidence = history_evidence{};
    selected_evidence.observe(*selected_baseline);
    CHECK(selected_evidence.baselines == 1U);
    CHECK(selected_evidence.native_readings == 0U);
    CHECK_FALSE(selected_evidence.native_progress);
    CHECK_FALSE(selected_evidence.native_history_observed());
}

} // namespace
} // namespace inputnext::gameinput_detail
