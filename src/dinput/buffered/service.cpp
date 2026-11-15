#include "service.hpp"

#include "core/core.hpp"
#include "dinput/native_types.hpp"
#include "gameinput/runtime/history_access.hpp"
#include "history/reader_observation.hpp"
#include "service_test_access.hpp"
#include "subscription.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace inputnext::dinput {
namespace {

using namespace std::chrono_literals;

inline constexpr std::size_t lane_capacity = 16U;
inline constexpr std::size_t subscriber_capacity = 64U;
static_assert(subscriber_capacity * (static_cast<std::size_t>(maximum_buffer_size) - 1U) *
                  sizeof(DIDEVICEOBJECTDATA) <=
              std::size_t{8U} * 1024U * 1024U);
inline constexpr unsigned unqualified_read_limit = 8U;
inline constexpr auto history_retry_period = 4ms;

[[nodiscard]] HRESULT map_history_failure(const history_failure& failure) noexcept {
    switch (failure.code) {
    case history_error::disconnected:
    case history_error::history_lost:
    case history_error::clock_discontinuity:
        return DIERR_INPUTLOST;
    case history_error::closed:
        return DIERR_NOTINITIALIZED;
    case history_error::capacity_exceeded:
        return DIERR_OUTOFMEMORY;
    case history_error::invalid_argument:
    case history_error::invalid_reader:
    case history_error::busy:
    case history_error::identity_exhausted:
    case history_error::sequence_exhausted:
    case history_error::no_data:
    case history_error::provider_failure:
        return DIERR_GENERIC;
    }
    return DIERR_GENERIC;
}

[[nodiscard]] bool is_busy(const std::optional<history_failure>& failure) noexcept {
    return failure.has_value() && failure->code == history_error::busy;
}

[[nodiscard]] bool fatal_read_failure(const history_failure& failure) noexcept {
    switch (failure.code) {
    case history_error::invalid_argument:
    case history_error::invalid_reader:
    case history_error::closed:
    case history_error::disconnected:
    case history_error::capacity_exceeded:
    case history_error::identity_exhausted:
    case history_error::sequence_exhausted:
        return true;
    case history_error::busy:
    case history_error::no_data:
    case history_error::history_lost:
    case history_error::provider_failure:
    case history_error::clock_discontinuity:
        return false;
    }
    return true;
}

} // namespace

struct buffered_input::implementation final {
    enum class lane_phase : std::uint8_t { starting, running, stopping, done, joining };
    enum class reservation_step : std::uint8_t { ready, reap_completed };

    struct lane final {
        std::size_t index{};
        connection_target target{};
        std::thread worker;
        std::uint64_t membership_revision{};
        lane_phase phase{lane_phase::starting};
    };

    struct subscription_entry final {
        std::shared_ptr<buffered_subscription> value;
        std::size_t lane_index{};
        std::uint64_t generation{};
        bool reserved{};
        bool releasing{};
    };

    struct reservation final {
        std::size_t slot{};
        std::size_t lane_index{};
    };

    struct subscription_snapshot final {
        std::array<std::shared_ptr<buffered_subscription>, subscriber_capacity> values{};
        std::array<std::size_t, subscriber_capacity> slots{};
        std::array<std::uint64_t, subscriber_capacity> generations{};
        std::size_t count{};
        std::uint64_t membership_revision{};
    };

    struct lane_read_state final {
        std::optional<history_domain> latest_domain;
        std::uint64_t latest_sequence{};
        std::optional<history_sample> latest_sample;
        std::array<unsigned, subscriber_capacity> unqualified_reads{};
        std::array<std::uint64_t, subscriber_capacity> attempt_generations{};
    };

    enum class turn_action : std::uint8_t { progress, retry, stop };

    struct turn_result final {
        turn_action action{turn_action::stop};
        std::uint64_t membership_revision{};
    };

    implementation(core::service& core_value,
        gameinput_runtime& runtime_value,
        focus::observer& focus_value) noexcept
        : core(core_value),
          runtime(runtime_value),
          focus(focus_value) {}

    [[nodiscard]] bool eligible(const buffered_subscription& subscription) const noexcept {
        const auto target = subscription.target();
        const auto latest = core.read_latest(target);
        if (latest.has_value()) {
            if (latest->target != target) {
                return false;
            }
        } else if (latest.error() != core::status::no_data) {
            return false;
        }

        const auto window = subscription.window();
        if (!window.has_value()) {
            return true;
        }
        const auto guard = focus.lock_presentation();
        return guard.matches_foreground_window_epoch(*window);
    }

    void revoke_if_ineligible(
        const std::shared_ptr<buffered_subscription>& subscription) const noexcept {
        if (subscription == nullptr || subscription->finished()) {
            return;
        }
        if (!eligible(*subscription)) {
            subscription->lose();
            subscription->signal_pending();
        }
    }

    [[nodiscard]] subscription_snapshot snapshot_subscriptions(std::size_t lane_index) noexcept {
        subscription_snapshot snapshot{};
        std::scoped_lock lock{gate};
        const auto* current_lane = lanes[lane_index].get();
        if (current_lane == nullptr) {
            return snapshot;
        }
        snapshot.membership_revision = current_lane->membership_revision;
        for (std::size_t slot = 0U; slot < subscriptions.size(); ++slot) {
            const auto& entry = subscriptions[slot];
            if (entry.value != nullptr && !entry.releasing && entry.lane_index == lane_index) {
                snapshot.values[snapshot.count] = entry.value;
                snapshot.slots[snapshot.count] = slot;
                snapshot.generations[snapshot.count] = entry.generation;
                ++snapshot.count;
            }
        }
        return snapshot;
    }

    [[nodiscard]] bool stop_if_empty(lane& current_lane, std::uint64_t observed_revision) noexcept {
        {
            std::scoped_lock lock{gate};
            if (lanes[current_lane.index].get() != &current_lane ||
                current_lane.membership_revision != observed_revision) {
                return false;
            }
            if (has_reservations_locked(current_lane.index)) {
                return false;
            }
            current_lane.phase = lane_phase::stopping;
        }
        changed.notify_all();
        return true;
    }

    [[nodiscard]] bool wait_for_retry(lane& current_lane,
        std::uint64_t observed_revision) noexcept {
        std::unique_lock lock{gate};
        changed.wait_for(lock, history_retry_period, [&] {
            return admission_closed || current_lane.phase == lane_phase::stopping ||
                   lanes[current_lane.index].get() != &current_lane ||
                   current_lane.membership_revision != observed_revision;
        });
        return !admission_closed && current_lane.phase != lane_phase::stopping &&
               lanes[current_lane.index].get() == &current_lane;
    }

    [[nodiscard]] static bool valid_target_observation(
        const history_detail::observed_read& observation,
        const connection_target target) noexcept {
        return observation.batch.domain.target == target;
    }

    void fail_lane_subscriptions(lane& current_lane, HRESULT error) noexcept {
        const auto snapshot = snapshot_subscriptions(current_lane.index);
        for (std::size_t index = 0U; index < snapshot.count; ++index) {
            const auto& subscription = snapshot.values[index];
            if (subscription == nullptr || subscription->finished()) {
                continue;
            }
            if (error == DIERR_INPUTLOST) {
                subscription->lose();
            } else {
                subscription->fail(error);
            }
            subscription->signal_pending();
        }
    }

    void track_observation(lane& current_lane,
        const history_detail::observed_read& observation,
        lane_read_state& state,
        std::array<DWORD, history_batch_capacity>& sequences,
        std::size_t& record_count) noexcept {
        const auto& batch = observation.batch;
        record_count = std::min(batch.count, history_batch_capacity);
        for (std::size_t index = 0U; index < record_count; ++index) {
            sequences[index] = next_sequence.fetch_add(1U, std::memory_order_relaxed);
        }
        if (is_busy(batch.failure) && record_count == 0U) {
            return;
        }
        if (!valid_target_observation(observation, current_lane.target)) {
            fail_lane_subscriptions(current_lane, DIERR_INPUTLOST);
            record_count = 0U;
            return;
        }

        if (state.latest_domain.has_value() && *state.latest_domain != batch.domain) {
            state.latest_sequence = 0U;
            state.latest_sample.reset();
        }
        state.latest_domain = batch.domain;
        for (std::size_t index = 0U; index < record_count; ++index) {
            state.latest_sequence = batch.records[index].sequence;
            state.latest_sample = batch.records[index].sample;
        }
    }

    void publish_to_existing(const history_detail::observed_read& observation,
        const std::array<DWORD, history_batch_capacity>& sequences,
        const subscription_snapshot& snapshot) const noexcept {
        for (std::size_t index = 0U; index < snapshot.count; ++index) {
            const auto& subscription = snapshot.values[index];
            if (subscription == nullptr || subscription->finished() || subscription->pending()) {
                continue;
            }
            revoke_if_ineligible(subscription);
            if (subscription->finished()) {
                continue;
            }
            subscription->accept(observation, sequences);
            subscription->signal_pending();
        }
    }

    [[nodiscard]] static bool usable_boundary(const history_detail::reader_boundary& boundary,
        const connection_target target) noexcept {
        return boundary.domain.target == target && boundary.domain.stream != 0U &&
               boundary.clock.timestamp_us != 0U;
    }

    void prepare_pending(lane& current_lane,
        const history_detail::observed_read& observation,
        const subscription_snapshot& snapshot,
        lane_read_state& state) const noexcept {
        const auto* boundary = observation.boundary.has_value() ? &*observation.boundary : nullptr;
        const bool has_cut = boundary != nullptr && usable_boundary(*boundary, current_lane.target);

        for (std::size_t index = 0U; index < snapshot.count; ++index) {
            const auto& subscription = snapshot.values[index];
            if (subscription == nullptr || subscription->finished() || !subscription->pending()) {
                continue;
            }
            revoke_if_ineligible(subscription);
            if (subscription->finished()) {
                continue;
            }

            const auto slot = snapshot.slots[index];
            if (state.attempt_generations[slot] != snapshot.generations[index]) {
                state.attempt_generations[slot] = snapshot.generations[index];
                state.unqualified_reads[slot] = 0U;
            }
            auto& attempts = state.unqualified_reads[slot];
            const bool exact_seed = has_cut && state.latest_domain.has_value() &&
                                    *state.latest_domain == boundary->domain &&
                                    state.latest_sequence == boundary->sequence &&
                                    state.latest_sample.has_value();
            const bool qualified = has_cut && observation.fresh_checkpoint &&
                                   observation.batch.caught_up && boundary->checkpoint_qpc != 0U &&
                                   exact_seed;
            if (qualified) {
                subscription->prepare(*boundary, state.latest_sample);
                subscription->signal_pending();
                attempts = 0U;
                continue;
            }

            if (attempts < unqualified_read_limit) {
                ++attempts;
            }
            if (attempts < unqualified_read_limit) {
                continue;
            }
            if (has_cut) {
                // An exact committed sequence and real source clock set the cutoff. Without a
                // seed or checkpoint, the subscription enters unknown-loss resynchronization.
                subscription->prepare(*boundary, std::nullopt);
            } else {
                subscription->fail(DIERR_GENERIC);
            }
            subscription->signal_pending();
        }
    }

    void process_read(lane& current_lane,
        const history_detail::observed_read& observation,
        const subscription_snapshot& snapshot,
        lane_read_state& state) noexcept {
        std::array<DWORD, history_batch_capacity> sequences{};
        std::size_t record_count{};
        track_observation(current_lane, observation, state, sequences, record_count);
        if (is_busy(observation.batch.failure) && record_count == 0U) {
            return;
        }
        publish_to_existing(observation, sequences, snapshot);
        if (!is_busy(observation.batch.failure)) {
            prepare_pending(current_lane, observation, snapshot, state);
        }
    }

    [[nodiscard]] static bool any_live(const subscription_snapshot& snapshot) noexcept {
        for (std::size_t index = 0U; index < snapshot.count; ++index) {
            if (snapshot.values[index] != nullptr && !snapshot.values[index]->finished()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool open_lane_reader(lane& current_lane,
        history_reader_handle& reader) noexcept {
        for (;;) {
            const auto snapshot = snapshot_subscriptions(current_lane.index);
            if (!any_live(snapshot)) {
                if (stop_if_empty(current_lane, snapshot.membership_revision) ||
                    !wait_for_retry(current_lane, snapshot.membership_revision)) {
                    return false;
                }
                continue;
            }

            auto opened = gameinput_detail::runtime_history_access::open_directinput(runtime,
                current_lane.target,
                current_lane.index,
                history_time::clock::now());
            if (opened.has_value()) {
                reader = *opened;
                if (reader.target == current_lane.target && reader.value != 0U) {
                    return true;
                }
                fail_lane_subscriptions(current_lane, DIERR_INPUTLOST);
                return false;
            }
            if (opened.error().code != history_error::busy) {
                fail_lane_subscriptions(current_lane, map_history_failure(opened.error()));
                return false;
            }
            if (!wait_for_retry(current_lane, snapshot.membership_revision)) {
                return false;
            }
        }
    }

    [[nodiscard]] turn_result read_lane_turn(lane& current_lane,
        history_reader_handle reader,
        lane_read_state& state) noexcept {
        const auto snapshot = snapshot_subscriptions(current_lane.index);
        if (!any_live(snapshot)) {
            if (stop_if_empty(current_lane, snapshot.membership_revision)) {
                return {.action = turn_action::stop};
            }
            return {.action = turn_action::retry,
                .membership_revision = snapshot.membership_revision};
        }

        const auto capture_boundary = std::ranges::any_of(snapshot.values.begin(),
            snapshot.values.begin() + static_cast<std::ptrdiff_t>(snapshot.count),
            [](const auto& subscription) {
                return subscription != nullptr && subscription->pending();
            });
        const auto observation = gameinput_detail::runtime_history_access::read_directinput(runtime,
            reader,
            history_time::clock::now(),
            capture_boundary);
        process_read(current_lane, observation, snapshot, state);

        if (!observation.batch.failure.has_value()) {
            return {.action =
                        observation.batch.count == 0U ? turn_action::retry : turn_action::progress,
                .membership_revision = snapshot.membership_revision};
        }
        if (is_busy(observation.batch.failure)) {
            for (std::size_t index = 0U; index < snapshot.count; ++index) {
                if (snapshot.values[index] != nullptr) {
                    snapshot.values[index]->signal_pending();
                }
            }
            return {.action = turn_action::retry,
                .membership_revision = snapshot.membership_revision};
        }
        if (fatal_read_failure(*observation.batch.failure)) {
            fail_lane_subscriptions(current_lane, map_history_failure(*observation.batch.failure));
            return {.action = turn_action::stop};
        }
        return {.action = turn_action::retry, .membership_revision = snapshot.membership_revision};
    }

    void read_lane(lane& current_lane, history_reader_handle reader) noexcept {
        lane_read_state state{};
        for (;;) {
            const auto result = read_lane_turn(current_lane, reader, state);
            if (result.action == turn_action::stop) {
                return;
            }
            if (result.action == turn_action::retry &&
                !wait_for_retry(current_lane, result.membership_revision)) {
                return;
            }
        }
    }

    [[nodiscard]] bool has_reservations_locked(std::size_t lane_index) const noexcept {
        return std::ranges::any_of(subscriptions, [lane_index](const auto& entry) {
            return entry.reserved && entry.lane_index == lane_index;
        });
    }

    [[nodiscard]] detail::buffered_input_test_hooks snapshot_test_hooks() noexcept {
        std::scoped_lock lock{gate};
        return test_hooks;
    }

    [[nodiscard]] bool continue_after_close(lane& current_lane) noexcept {
        for (;;) {
            const auto snapshot = snapshot_subscriptions(current_lane.index);
            const bool live = any_live(snapshot);
            bool restart{};
            {
                std::unique_lock lock{gate};
                if (current_lane.membership_revision != snapshot.membership_revision) {
                    continue;
                }
                const bool reserved = has_reservations_locked(current_lane.index);
                if (admission_closed && reserved) {
                    changed.wait(lock,
                        [&] { return !has_reservations_locked(current_lane.index); });
                    continue;
                }
                restart = !admission_closed && (live || reserved);
                current_lane.phase = restart ? lane_phase::running : lane_phase::done;
            }
            changed.notify_all();
            return restart;
        }
    }

    [[nodiscard]] bool finish_lane(lane& current_lane, history_reader_handle reader) noexcept {
        {
            std::scoped_lock lock{gate};
            current_lane.phase = lane_phase::stopping;
        }
        const auto hooks = snapshot_test_hooks();
        if (hooks.before_close != nullptr) {
            hooks.before_close(hooks.context);
        }
        if (reader.value != 0U && reader.target.valid()) {
            const auto close_result = runtime.close_history(reader);
            if (!close_result.has_value()) {
                fail_lane_subscriptions(current_lane, map_history_failure(close_result.error()));
            }
        }
        return continue_after_close(current_lane);
    }

    void run_lane(lane& current_lane) noexcept {
        for (;;) {
            history_reader_handle reader{};
            if (open_lane_reader(current_lane, reader)) {
                read_lane(current_lane, reader);
            }
            if (!finish_lane(current_lane, reader)) {
                return;
            }
        }
    }

    void reap_subscriptions() noexcept {
        for (auto& entry : subscriptions) {
            const buffered_subscription* candidate{};
            {
                std::scoped_lock lock{gate};
                if (entry.value == nullptr || entry.reserved || entry.releasing ||
                    entry.value.use_count() != 1L) {
                    continue;
                }
                entry.releasing = true;
                candidate = entry.value.get();
            }

            const bool finished = candidate->finished();
            std::shared_ptr<buffered_subscription> released;
            std::size_t released_lane{};
            {
                std::scoped_lock lock{gate};
                if (entry.value.get() != candidate || !entry.releasing) {
                    continue;
                }
                if (!finished || entry.value.use_count() != 1L) {
                    entry.releasing = false;
                    continue;
                }
                released = std::move(entry.value);
                released_lane = entry.lane_index;
            }

            released.reset();
            {
                std::scoped_lock lock{gate};
                entry = subscription_entry{};
                if (lanes[released_lane] != nullptr &&
                    lanes[released_lane]->membership_revision != UINT64_MAX) {
                    ++lanes[released_lane]->membership_revision;
                }
            }
            changed.notify_all();
        }
    }

    // Requires lane_reaper_gate, held through the caller's reservation selection.
    void reap_completed_lanes_locked() noexcept {
        for (const auto& owned_lane : lanes) {
            lane* candidate{};
            {
                std::scoped_lock lock{gate};
                if (owned_lane != nullptr && owned_lane->phase == lane_phase::done) {
                    owned_lane->phase = lane_phase::joining;
                    candidate = owned_lane.get();
                }
            }
            if (candidate == nullptr) {
                continue;
            }
            if (candidate->worker.joinable()) {
                candidate->worker.join();
            }
            std::unique_ptr<lane> released;
            {
                std::scoped_lock lock{gate};
                if (lanes[candidate->index].get() == candidate) {
                    released = std::move(lanes[candidate->index]);
                }
            }
            released.reset();
        }
    }

    void rollback_reservation(const reservation& reserved) noexcept {
        std::unique_ptr<lane> released;
        {
            std::scoped_lock lock{gate};
            subscriptions[reserved.slot] = subscription_entry{};
            auto& current_lane = lanes[reserved.lane_index];
            // A starting lane has no installed members: its first installer launches it.
            if (current_lane != nullptr && current_lane->phase == lane_phase::starting &&
                !has_reservations_locked(reserved.lane_index)) {
                released = std::move(current_lane);
            }
        }
        released.reset();
        changed.notify_all();
    }

    [[nodiscard]] std::expected<reservation_step, HRESULT>
    reserve_subscription(connection_target target, reservation& reserved) noexcept {
        {
            std::scoped_lock lock{gate};
            if (admission_closed) {
                return std::unexpected(DIERR_NOTINITIALIZED);
            }
            const auto free_entry =
                std::ranges::find_if(subscriptions, [](const subscription_entry& entry) {
                    return entry.value == nullptr && !entry.reserved && !entry.releasing;
                });
            if (free_entry == subscriptions.end() || next_generation == 0U) {
                return std::unexpected(DIERR_OUTOFMEMORY);
            }
            reserved.slot =
                static_cast<std::size_t>(std::distance(subscriptions.begin(), free_entry));

            bool matching_lane{};
            for (std::size_t index = 0U; index < lanes.size(); ++index) {
                if (lanes[index] == nullptr || lanes[index]->target != target) {
                    continue;
                }
                matching_lane = true;
                if (lanes[index]->phase == lane_phase::done ||
                    lanes[index]->phase == lane_phase::joining) {
                    return reservation_step::reap_completed;
                }
                if (lanes[index]->membership_revision == UINT64_MAX) {
                    return std::unexpected(DIERR_OUTOFMEMORY);
                }
                reserved.lane_index = index;
                break;
            }
            if (!matching_lane) {
                const auto free_lane = std::ranges::find_if(lanes,
                    [](const auto& current_lane) { return current_lane == nullptr; });
                if (free_lane == lanes.end()) {
                    return std::unexpected(DIERR_OUTOFMEMORY);
                }
                reserved.lane_index =
                    static_cast<std::size_t>(std::distance(lanes.begin(), free_lane));
                try {
                    *free_lane = std::make_unique<lane>();
                } catch (...) {
                    return std::unexpected(DIERR_OUTOFMEMORY);
                }
                (*free_lane)->index = reserved.lane_index;
                (*free_lane)->target = target;
            }

            auto& entry = subscriptions[reserved.slot];
            entry.lane_index = reserved.lane_index;
            entry.generation = next_generation++;
            entry.reserved = true;
        }
        return reservation_step::ready;
    }

    [[nodiscard]] HRESULT install_subscription(const reservation& reserved,
        const std::shared_ptr<buffered_subscription>& candidate) noexcept {
        {
            std::scoped_lock lock{gate};
            if (admission_closed) {
                return DIERR_NOTINITIALIZED;
            }
            auto& current_lane = *lanes[reserved.lane_index];
            if (current_lane.membership_revision == UINT64_MAX) {
                return DIERR_OUTOFMEMORY;
            }
            if (current_lane.phase == lane_phase::starting) {
                try {
                    auto* lane_value = &current_lane;
                    current_lane.worker =
                        std::thread([state = this, lane_value] { state->run_lane(*lane_value); });
                    current_lane.phase = lane_phase::running;
                } catch (...) {
                    return DIERR_OUTOFMEMORY;
                }
            }
            auto& entry = subscriptions[reserved.slot];
            entry.value = candidate;
            entry.reserved = false;
            ++current_lane.membership_revision;
        }
        changed.notify_all();
        return S_OK;
    }

    [[nodiscard]] HRESULT select_reservation(connection_target target,
        reservation& reserved) noexcept {
        std::scoped_lock reaper_lock{lane_reaper_gate};
        reap_completed_lanes_locked();
        auto selected = reserve_subscription(target, reserved);
        if (selected.has_value() && *selected == reservation_step::reap_completed) {
            reap_completed_lanes_locked();
            selected = reserve_subscription(target, reserved);
        }
        if (!selected.has_value()) {
            return selected.error();
        }
        return *selected == reservation_step::ready ? S_OK : DIERR_GENERIC;
    }

    [[nodiscard]] std::expected<std::shared_ptr<buffered_subscription>, HRESULT> subscribe(
        buffer_configuration configuration) noexcept {
        reap_subscriptions();

        reservation reserved{};
        const auto reserved_result = select_reservation(configuration.target, reserved);
        if (FAILED(reserved_result)) {
            return std::unexpected(reserved_result);
        }

        std::shared_ptr<buffered_subscription> candidate;
        try {
            const auto hooks = snapshot_test_hooks();
            if (hooks.before_allocation != nullptr) {
                hooks.before_allocation(hooks.context);
            }
            candidate = std::make_shared<buffered_subscription>(std::move(configuration));
        } catch (const std::bad_alloc&) {
            rollback_reservation(reserved);
            return std::unexpected(DIERR_OUTOFMEMORY);
        } catch (...) {
            rollback_reservation(reserved);
            return std::unexpected(DIERR_GENERIC);
        }

        const auto installed = install_subscription(reserved, candidate);
        if (FAILED(installed)) {
            candidate.reset();
            rollback_reservation(reserved);
            return std::unexpected(installed);
        }
        return candidate;
    }

    void close_admission() noexcept {
        std::array<std::shared_ptr<buffered_subscription>, subscriber_capacity> values{};
        std::size_t count{};
        {
            std::scoped_lock lock{gate};
            if (admission_closed) {
                return;
            }
            admission_closed = true;
            for (const auto& entry : subscriptions) {
                if (entry.value != nullptr) {
                    values[count++] = entry.value;
                }
            }
        }
        changed.notify_all();
        for (std::size_t index = 0U; index < count; ++index) {
            values[index]->lose();
            values[index]->signal_pending();
        }
    }

    void drain() noexcept {
        close_admission();
        std::scoped_lock drain_lock{drain_gate, lane_reaper_gate};
        if (drained) {
            return;
        }

        std::array<lane*, lane_capacity> joining{};
        {
            std::scoped_lock lock{gate};
            for (std::size_t index = 0U; index < lanes.size(); ++index) {
                if (lanes[index] != nullptr) {
                    lanes[index]->phase = lane_phase::stopping;
                    joining[index] = lanes[index].get();
                }
            }
        }
        changed.notify_all();
        for (auto* current_lane : joining) {
            if (current_lane != nullptr && current_lane->worker.joinable()) {
                current_lane->worker.join();
            }
        }

        std::array<std::unique_ptr<lane>, lane_capacity> released_lanes{};
        std::array<std::shared_ptr<buffered_subscription>, subscriber_capacity> released{};
        {
            std::scoped_lock lock{gate};
            for (std::size_t index = 0U; index < lanes.size(); ++index) {
                released_lanes[index] = std::move(lanes[index]);
            }
            for (std::size_t index = 0U; index < subscriptions.size(); ++index) {
                released[index] = std::move(subscriptions[index].value);
                subscriptions[index] = subscription_entry{};
            }
            drained = true;
        }
        for (const auto& subscription : released) {
            if (subscription != nullptr) {
                subscription->synchronize_signals();
            }
        }
    }

    void refresh_eligibility() noexcept {
        std::array<std::shared_ptr<buffered_subscription>, subscriber_capacity> values{};
        std::size_t count{};
        {
            std::scoped_lock lock{gate};
            for (const auto& entry : subscriptions) {
                if (entry.value != nullptr) {
                    values[count++] = entry.value;
                }
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            if (!values[index]->finished()) {
                revoke_if_ineligible(values[index]);
            }
            values[index]->signal_pending();
        }
    }

    core::service& core;
    gameinput_runtime& runtime;
    focus::observer& focus;
    std::mutex gate;
    std::condition_variable changed;
    std::mutex lane_reaper_gate;
    std::mutex drain_gate;
    std::array<std::unique_ptr<lane>, lane_capacity> lanes{};
    std::array<subscription_entry, subscriber_capacity> subscriptions{};
    detail::buffered_input_test_hooks test_hooks{};
    std::uint64_t next_generation{1U};
    std::atomic<DWORD> next_sequence{1U};
    bool admission_closed{};
    bool drained{};
};

buffered_input::buffered_input(core::service& core,
    gameinput_runtime& runtime,
    focus::observer& focus)
    : implementation_(std::make_unique<implementation>(core, runtime, focus)) {
}

buffered_input::~buffered_input() {
    close_admission();
    drain();
}

std::expected<std::shared_ptr<buffered_subscription>, HRESULT> buffered_input::subscribe(
    buffer_configuration configuration) noexcept {
    return implementation_->subscribe(std::move(configuration));
}

void buffered_input::close_admission() noexcept {
    implementation_->close_admission();
}

void buffered_input::drain() noexcept {
    implementation_->drain();
}

void buffered_input::refresh_eligibility() noexcept {
    implementation_->refresh_eligibility();
}

void detail::buffered_input_test_access::set_hooks(buffered_input& input,
    buffered_input_test_hooks hooks) noexcept {
    std::scoped_lock lock{input.implementation_->gate};
    input.implementation_->test_hooks = hooks;
}

} // namespace inputnext::dinput
