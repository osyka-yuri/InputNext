#include <GameInput.h>
#include <Windows.h>
#include <objbase.h>

#include "native_capture.hpp"

#include "capture_report.hpp"
#include "gameinput_observation.hpp"

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace inputnext::input_qualification::diagnostic {
namespace {

constexpr std::uint32_t max_native_batch_records = 32U;

} // namespace

std::uintptr_t canonical_identity(gameinput::IGameInputReading& reading) noexcept {
    IUnknown* unknown{};
    if (FAILED(reading.QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unknown))) ||
        unknown == nullptr) {
        return 0U;
    }
    const auto identity = reinterpret_cast<std::uintptr_t>(unknown);
    unknown->Release();
    return identity;
}

void native_capture::release_reading::operator()(
    gameinput::IGameInputReading* reading) const noexcept {
    reading->Release();
}

void native_capture::append(reading_owner reading, const device_report& descriptor, bool reverse) {
    const auto identity = canonical_identity(*reading);
    const auto sample = inspect_reading(*reading, descriptor, reverse ? nullptr : &resolver_);
    if (identity == 0U) {
        ++order_.identity_failures;
    }
    if (!frames_.empty()) {
        const auto& previous = frames_.back();
        if (reverse ? previous.sample.timestamp < sample.timestamp
                    : previous.sample.timestamp > sample.timestamp) {
            ++order_.timestamp_order_violations;
        }
        if (previous.sample.timestamp == sample.timestamp && previous.identity != identity) {
            ++order_.same_timestamp_distinct_readings;
        }
    }
    frames_.push_back(
        record{.sample = sample, .identity = identity, .reading = std::move(reading)});
}

void native_capture::seed(gameinput::IGameInput& input,
    gameinput::IGameInputDevice& device,
    const device_report& descriptor) {
    if (state_ != walk_state::not_started) {
        return;
    }
    frames_.reserve(max_native_capture_records);
    gameinput::IGameInputReading* current{};
    const auto result = input.GetCurrentReading(observation_kinds, &device, &current);
    seed_hresult_ = static_cast<std::int32_t>(result);
    last_hresult_ = seed_hresult_;
    reading_owner initial{current};
    if (FAILED(result) || initial == nullptr) {
        state_ = walk_state::seed_unavailable;
        return;
    }
    append(std::move(initial), descriptor, false);
    state_ = walk_state::walking;
}

bool native_capture::pump(gameinput::IGameInput& input,
    gameinput::IGameInputDevice& device,
    const device_report& descriptor) {
    if (state_ != walk_state::walking) {
        return false;
    }
    const auto before = frames_.size();
    for (std::uint32_t count = 0U; count < max_native_batch_records; ++count) {
        if (frames_.size() >= max_native_capture_records) {
            state_ = walk_state::capacity_exhausted;
            break;
        }
        gameinput::IGameInputReading* next{};
        const auto result =
            input.GetNextReading(frames_.back().reading.get(), observation_kinds, &device, &next);
        last_hresult_ = static_cast<std::int32_t>(result);
        reading_owner next_reading{next};
        if (result == gameinput::GAMEINPUT_E_READING_NOT_FOUND && next_reading == nullptr) {
            ++no_next_polls_;
            break;
        }
        if (FAILED(result) || next_reading == nullptr) {
            state_ = result == gameinput::GAMEINPUT_E_REFERENCE_READING_TOO_OLD
                         ? walk_state::reference_too_old
                         : walk_state::next_failed;
            break;
        }
        append(std::move(next_reading), descriptor, false);
        ++transitions_;
    }
    return frames_.size() != before;
}

void native_capture::advance(gameinput::IGameInput& input,
    gameinput::IGameInputDevice& device,
    const device_report& descriptor) {
    seed(input, device, descriptor);
    static_cast<void>(pump(input, device, descriptor));
}

void native_capture::finish(gameinput::IGameInput& input,
    gameinput::IGameInputDevice& device,
    const device_report& descriptor) {
    while (pump(input, device, descriptor)) {
    }
    if (state_ == walk_state::walking) {
        state_ = walk_state::complete;
    }
}

native_capture native_capture::read_tail(gameinput::IGameInput& input,
    gameinput::IGameInputDevice& device,
    const device_report& descriptor) {
    native_capture tail;
    tail.frames_.reserve(max_native_tail_records);
    gameinput::IGameInputReading* current{};
    const auto seed_result = input.GetCurrentReading(observation_kinds, &device, &current);
    tail.seed_hresult_ = static_cast<std::int32_t>(seed_result);
    tail.last_hresult_ = tail.seed_hresult_;
    reading_owner reading{current};
    if (FAILED(seed_result) || reading == nullptr) {
        tail.state_ = walk_state::tail_seed_unavailable;
        return tail;
    }
    tail.state_ = walk_state::tail_budget_exhausted;
    for (std::uint32_t count = 0U; count < max_native_tail_records; ++count) {
        tail.append(std::move(reading), descriptor, true);
        gameinput::IGameInputReading* previous{};
        const auto result = input.GetPreviousReading(tail.frames_.back().reading.get(),
            observation_kinds,
            &device,
            &previous);
        tail.last_hresult_ = static_cast<std::int32_t>(result);
        reading.reset(previous);
        if (FAILED(result) || reading == nullptr) {
            tail.state_ = result == gameinput::GAMEINPUT_E_READING_NOT_FOUND
                              ? walk_state::tail_window_end
                              : walk_state::tail_previous_failed;
            break;
        }
        ++tail.transitions_;
    }
    return tail;
}

native_capture_view native_capture::view(std::vector<reading_view>& records) const {
    records.clear();
    records.reserve(frames_.size());
    for (const auto& frame : frames_) {
        records.push_back(
            reading_view{.data = std::cref(frame.sample), .canonical_identity = frame.identity});
    }
    return {.frames = records,
        .truncated =
            state_ == walk_state::capacity_exhausted || state_ == walk_state::tail_budget_exhausted,
        .stop_reason = stop_reason(),
        .seed_hresult = seed_hresult_,
        .last_hresult = last_hresult_,
        .no_next_polls = no_next_polls_,
        .transitions = transitions_,
        .order = order_,
        .forward_complete = state_ == walk_state::complete};
}

std::string_view native_capture::stop_reason() const noexcept {
    switch (state_) {
    case walk_state::not_started:
        return "not_started";
    case walk_state::seed_unavailable:
        return "initial_current_unavailable";
    case walk_state::walking:
        return "tracking_forward_from_initial_current";
    case walk_state::complete:
        return "capture_completed_without_observed_history_loss";
    case walk_state::capacity_exhausted:
        return "record_capacity_exhausted";
    case walk_state::reference_too_old:
        return "history_lost_reference_too_old";
    case walk_state::next_failed:
        return "next_reading_failed";
    case walk_state::tail_seed_unavailable:
        return "post_capture_current_unavailable";
    case walk_state::tail_window_end:
        return "post_capture_tail_window_end";
    case walk_state::tail_previous_failed:
        return "post_capture_previous_failed";
    case walk_state::tail_budget_exhausted:
        return "post_capture_tail_budget_exhausted";
    }
    return "not_started";
}

} // namespace inputnext::input_qualification::diagnostic
