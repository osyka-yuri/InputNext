#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/property_abi.h"
#include "gameinput/reading_parts.hpp"
#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <dinput.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] DIPROPDWORD buffer_size_property() noexcept {
    DIPROPDWORD property{};
    property.diph.dwSize = data_size<DIPROPDWORD>();
    property.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    property.diph.dwObj = 0U;
    property.diph.dwHow = DIPH_DEVICE;
    return property;
}

[[nodiscard]] DWORD button_zero_offset(const bool standard_format) noexcept {
    return standard_format ? static_cast<DWORD>(offsetof(DIJOYSTATE, rgbButtons))
                           : static_cast<DWORD>(offsetof(compact_state, buttons));
}

} // namespace

TEST_CASE("dinput.buffered_input_preserves_prefix_and_acknowledges_overflow_on_consumption",
    "[integration][dinput][input][buffered][overflow]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x71U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());

    REQUIRE(set_buffer_size(*device.get(), 4U) == DI_OK);
    auto property = buffer_size_property();
    REQUIRE(
        inputnext_dinput_get_property_w(device.get(), &DIPROP_BUFFERSIZE, &property.diph) == DI_OK);
    CHECK(property.dwData == 4U);
    REQUIRE(device->Acquire() == DI_OK);

    DWORD initial_count{};
    REQUIRE(peek_buffer_count(*device.get(), initial_count) == DI_OK);
    CHECK(initial_count == 0U);

    DIDEVICEOBJECTDATA untouched{};
    std::memset(&untouched, 0xA5, sizeof(untouched));
    auto invalid_output = untouched;
    DWORD invalid_count = 7U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              &invalid_output,
              &invalid_count,
              0x80000000U) == DIERR_INVALIDPARAM);
    CHECK(invalid_count == 7U);
    CHECK(std::memcmp(&invalid_output, &untouched, sizeof(untouched)) == 0);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 2U, .buttons = buffered_button});
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 3U, .buttons = 0U});
    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 4U, .buttons = buffered_button});
    enqueue_checkpoint(script, buffered_cut_timestamp + 4U);
    REQUIRE(wait_for_buffer_count(*device.get(), 3U));
    REQUIRE(wait_for_overflow(*device.get()));

    std::array<DIDEVICEOBJECTDATA, 8U> peeked{};
    auto peeked_count = static_cast<DWORD>(peeked.size());
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              peeked.data(),
              &peeked_count,
              DIGDD_PEEK) == DI_BUFFEROVERFLOW);
    REQUIRE(peeked_count == 3U);
    constexpr auto button_offset = static_cast<DWORD>(offsetof(compact_state, buttons));
    CHECK(peeked[0U].dwOfs == button_offset);
    CHECK(peeked[0U].dwData == 0U);
    CHECK(peeked[1U].dwOfs == button_offset);
    CHECK(peeked[1U].dwData == 0x80U);
    CHECK(peeked[2U].dwOfs == button_offset);
    CHECK(peeked[2U].dwData == 0U);
    CHECK(peeked[0U].dwSequence < peeked[1U].dwSequence);
    CHECK(peeked[1U].dwSequence < peeked[2U].dwSequence);

    DWORD overflow_probe{};
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &overflow_probe, 0U) ==
          DI_BUFFEROVERFLOW);

    peeked_count = static_cast<DWORD>(peeked.size());
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              peeked.data(),
              &peeked_count,
              DIGDD_PEEK) == DI_BUFFEROVERFLOW);
    CHECK(peeked_count == 3U);

    std::array<DIDEVICEOBJECTDATA, 8U> consumed{};
    auto consumed_count = static_cast<DWORD>(consumed.size());
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              consumed.data(),
              &consumed_count,
              0U) == DI_BUFFEROVERFLOW);
    REQUIRE(consumed_count == 3U);
    CHECK(consumed[0U].dwSequence == peeked[0U].dwSequence);
    CHECK(consumed[1U].dwSequence == peeked[1U].dwSequence);
    CHECK(consumed[2U].dwSequence == peeked[2U].dwSequence);

    overflow_probe = 0U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &overflow_probe, 0U) ==
          DI_OK);
    CHECK(overflow_probe == 0U);

    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 5U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 5U);
    REQUIRE(wait_for_buffer_count(*device.get(), 1U));
    DWORD flushed_count = INFINITE;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &flushed_count, 0U) ==
          DI_OK);
    CHECK(flushed_count == 1U);

    DWORD after_flush = INFINITE;
    CHECK(
        device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &after_flush, DIGDD_PEEK) ==
        DI_OK);
    CHECK(after_flush == 0U);
}

TEST_CASE("dinput.buffered_data_arguments_preserve_native_discard_and_all_event_reads",
    "[integration][dinput][input][buffered][arguments]") {
    core_test_support::fixture owners;
    const auto target = owners.connect();
    prepare_buffered_history(owners);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x82U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 2U, .buttons = buffered_button});
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 3U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 3U);
    REQUIRE(wait_for_buffer_count(*device.get(), 3U));

    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    std::memset(events.data(), 0xA5, sizeof(events));
    const auto sentinel = events;
    DWORD count{};
    CHECK(
        device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) == DI_OK);
    CHECK(count == 0U);
    CHECK(std::memcmp(events.data(), sentinel.data(), sizeof(events)) == 0);
    count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>() - 1U, events.data(), &count, 0U) ==
          DIERR_INVALIDPARAM);
    CHECK(count == 1U);
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              events.data(),
              &count,
              0x80000000U) == DIERR_INVALIDPARAM);
    CHECK(count == 1U);
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), nullptr, 0U) ==
          DIERR_INVALIDPARAM);
    CHECK(std::memcmp(events.data(), sentinel.data(), sizeof(events)) == 0);

    count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, DIGDD_PEEK) ==
          DI_OK);
    CHECK(count == 1U);
    CHECK(peek_buffer_count(*device.get(), count) == DI_OK);
    CHECK(count == 3U);
    count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, 0U) == DI_OK);
    CHECK(count == 1U);

    count = INFINITE;
    CHECK(
        device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, DIGDD_PEEK) ==
        DI_OK);
    REQUIRE(count == 2U);
    CHECK(events.front().dwData == 0x80U);
    CHECK(events[1U].dwData == 0U);
    CHECK(std::memcmp(&events[2U], &sentinel[2U], sizeof(events) - (2U * sizeof(events[0U]))) == 0);
    count = INFINITE;
    CHECK(
        device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) == DI_OK);
    CHECK(count == 2U);
    CHECK(peek_buffer_count(*device.get(), count) == DI_OK);
    CHECK(count == 0U);
}

TEST_CASE("dinput.buffered_subscribers_consume_independently_with_their_own_formats",
    "[integration][dinput][input][buffered][subscribers]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    auto standard = create_device(target, access, parent, 0x72U);
    auto custom = create_device(target, access, parent, 0x73U);
    REQUIRE(standard->SetDataFormat(&c_dfDIJoystick) == DI_OK);
    REQUIRE(standard->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) ==
            DI_OK);
    configure_compact_device(*custom.get(), window.get());
    REQUIRE(set_buffer_size(*standard.get(), 8U) == DI_OK);
    REQUIRE(set_buffer_size(*custom.get(), 8U) == DI_OK);
    REQUIRE(standard->Acquire() == DI_OK);
    REQUIRE(custom->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 2U, .buttons = buffered_button});
    enqueue_checkpoint(script, buffered_cut_timestamp + 2U);
    REQUIRE(wait_for_buffer_count(*standard.get(), 2U));
    REQUIRE(wait_for_buffer_count(*custom.get(), 2U));

    std::array<DIDEVICEOBJECTDATA, 4U> standard_events{};
    auto standard_count = static_cast<DWORD>(standard_events.size());
    REQUIRE(standard->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
                standard_events.data(),
                &standard_count,
                0U) == DI_OK);
    REQUIRE(standard_count == 2U);
    CHECK(standard_events[0U].dwOfs == button_zero_offset(true));
    CHECK(standard_events[0U].dwData == 0U);
    CHECK(standard_events[1U].dwOfs == button_zero_offset(true));
    CHECK(standard_events[1U].dwData == 0x80U);
    CHECK(standard_events[0U].dwSequence < standard_events[1U].dwSequence);

    std::array<DIDEVICEOBJECTDATA, 4U> custom_peek{};
    auto custom_peek_count = static_cast<DWORD>(custom_peek.size());
    REQUIRE(custom->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
                custom_peek.data(),
                &custom_peek_count,
                DIGDD_PEEK) == DI_OK);
    REQUIRE(custom_peek_count == 2U);
    CHECK(custom_peek[0U].dwOfs == button_zero_offset(false));
    CHECK(custom_peek[0U].dwData == 0U);
    CHECK(custom_peek[1U].dwOfs == button_zero_offset(false));
    CHECK(custom_peek[1U].dwData == 0x80U);
    CHECK(custom_peek[0U].dwSequence == standard_events[0U].dwSequence);
    CHECK(custom_peek[1U].dwSequence == standard_events[1U].dwSequence);

    std::array<DIDEVICEOBJECTDATA_DX3, 4U> dx3_events{};
    auto dx3_count = static_cast<DWORD>(dx3_events.size());
    REQUIRE(custom->GetDeviceData(data_size<DIDEVICEOBJECTDATA_DX3>(),
                reinterpret_cast<DIDEVICEOBJECTDATA*>(dx3_events.data()),
                &dx3_count,
                0U) == DI_OK);
    REQUIRE(dx3_count == 2U);
    CHECK(dx3_events[0U].dwOfs == button_zero_offset(false));
    CHECK(dx3_events[0U].dwData == 0U);
    CHECK(dx3_events[1U].dwOfs == button_zero_offset(false));
    CHECK(dx3_events[1U].dwData == 0x80U);
    CHECK(dx3_events[0U].dwSequence == standard_events[0U].dwSequence);
    CHECK(dx3_events[1U].dwSequence == standard_events[1U].dwSequence);

    DWORD empty_count{};
    CHECK(peek_buffer_count(*standard.get(), empty_count) == DI_OK);
    CHECK(empty_count == 0U);
    CHECK(peek_buffer_count(*custom.get(), empty_count) == DI_OK);
    CHECK(empty_count == 0U);
}

TEST_CASE("dinput.buffered_requested_capacity_is_reported_while_actual_storage_is_capped",
    "[integration][dinput][input][buffered][capacity]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x77U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    constexpr DWORD requested_capacity = 4'097U;
    REQUIRE(set_buffer_size(*device.get(), requested_capacity) == DI_OK);
    auto property = buffer_size_property();
    REQUIRE(
        inputnext_dinput_get_property_w(device.get(), &DIPROP_BUFFERSIZE, &property.diph) == DI_OK);
    CHECK(property.dwData == requested_capacity);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    constexpr std::uint64_t transition_count = 4'097U;
    for (std::uint64_t index = 1U; index <= transition_count; ++index) {
        enqueue_pad_reading(script,
            {.timestamp = buffered_cut_timestamp + index,
                .buttons = (index & 1U) == 0U ? button_mask{0U} : buffered_button});
    }
    enqueue_checkpoint(script, buffered_cut_timestamp + transition_count);
    constexpr DWORD expected_capacity = 4'095U;
    REQUIRE(wait_for_buffer_count(*device.get(), expected_capacity, 5s));
    REQUIRE(wait_for_overflow(*device.get(), 5s));

    std::vector<DIDEVICEOBJECTDATA> events(expected_capacity);
    auto count = expected_capacity;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) ==
          DI_BUFFEROVERFLOW);
    REQUIRE(count == expected_capacity);
    CHECK(events.front().dwOfs == offsetof(compact_state, buttons));
    CHECK(events.front().dwData == 0U);
    CHECK(events.back().dwOfs == offsetof(compact_state, buttons));
    CHECK(events.back().dwData == 0U);
    CHECK(events.front().dwSequence < events.back().dwSequence);

    DWORD empty = INFINITE;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &empty, DIGDD_PEEK) ==
          DI_OK);
    CHECK(empty == 0U);
}

TEST_CASE("dinput.buffered_equal_cut_timestamp_latches_loss_until_a_consuming_read",
    "[integration][dinput][input][buffered][loss]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x78U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp);
    REQUIRE(wait_for_overflow(*device.get()));

    DWORD query_count{};
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &query_count, 0U) ==
          DI_BUFFEROVERFLOW);
    CHECK(query_count == 0U);

    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 1U, .buttons = buffered_button});
    enqueue_checkpoint(script, buffered_cut_timestamp + 1U);
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 2U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 2U);
    REQUIRE(wait_for_buffer_count(*device.get(), 1U));

    std::array<DIDEVICEOBJECTDATA, 4U> events{};
    auto count = static_cast<DWORD>(events.size());
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) ==
          DI_BUFFEROVERFLOW);
    REQUIRE(count > 0U);
    CHECK(count <= events.size());
    for (DWORD index = 0U; index < count; ++index) {
        CHECK(events[index].dwOfs == offsetof(compact_state, buttons));
        CHECK(((events[index].dwData == 0U) || (events[index].dwData == 0x80U)));
        if (index > 0U) {
            CHECK(events[index - 1U].dwSequence < events[index].dwSequence);
        }
    }

    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 3U, .buttons = buffered_button});
    enqueue_checkpoint(script, buffered_cut_timestamp + 3U);
    REQUIRE(wait_for_buffer_count(*device.get(), 1U));
    count = static_cast<DWORD>(events.size());
    CHECK(
        device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) == DI_OK);
    CHECK(count == 1U);
    CHECK(((events[0U].dwData == 0x80U) || (events[0U].dwData == 0U)));
}

TEST_CASE("dinput.buffered_late_pre_cut_guide_is_loss_without_a_synthetic_transition",
    "[integration][dinput][input][buffered][loss][guide]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x7CU);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto qpc = owners.scripts[0U]->seed.baseline.observation_qpc + 1U;
    owners.gameinput.native->publish(
        gameinput_test_support::handle(owners.gameinput.device_memory[0U]),
        gameinput_detail::reading_parts{.guide = true,
            .source_timestamp_us = buffered_cut_timestamp - 1U,
            .observation_qpc = qpc});
    enqueue_checkpoint(owners.scripts[0U], buffered_cut_timestamp + 1U);
    REQUIRE(wait_for_overflow(*device.get()));

    std::array<DIDEVICEOBJECTDATA, 4U> events{};
    DWORD peeked = INFINITE;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &peeked, DIGDD_PEEK) ==
          DI_BUFFEROVERFLOW);
    CHECK(peeked == 0U);
    auto delivered = static_cast<DWORD>(events.size());
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
              events.data(),
              &delivered,
              DIGDD_PEEK) == DI_BUFFEROVERFLOW);
    CHECK(delivered == 0U);
}
