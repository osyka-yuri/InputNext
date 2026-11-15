#include "subscription.hpp"

#include "dinput/device_properties.hpp"
#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "dinput/profile.hpp"
#include "event_registration.hpp"
#include "history/reader_observation.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace inputnext::dinput {
namespace {

[[nodiscard]] bool is_supported_stride(DWORD stride) noexcept {
    return stride == sizeof(DIDEVICEOBJECTDATA) || stride == sizeof(DIDEVICEOBJECTDATA_DX3);
}

[[nodiscard]] HRESULT history_error_result(history_error error) noexcept {
    switch (error) {
    case history_error::disconnected:
    case history_error::closed:
    case history_error::invalid_reader:
        return DIERR_INPUTLOST;
    case history_error::busy:
        return S_FALSE;
    case history_error::capacity_exceeded:
        return DIERR_OUTOFMEMORY;
    case history_error::invalid_argument:
        return DIERR_INVALIDPARAM;
    case history_error::no_data:
        return E_PENDING;
    case history_error::identity_exhausted:
    case history_error::sequence_exhausted:
    case history_error::history_lost:
    case history_error::provider_failure:
    case history_error::clock_discontinuity:
        return DIERR_INPUTLOST;
    }
    return DIERR_GENERIC;
}

[[nodiscard]] bool is_guide_object(const data_binding& binding) noexcept {
    const auto descriptors = objects();
    return binding.kind == object_kind::button &&
           binding.profile_object_index < descriptors.size() &&
           descriptors[binding.profile_object_index].source_index == 10U;
}

[[nodiscard]] DWORD value_for(const data_binding& binding, const profile_values& values) noexcept {
    const auto descriptors = objects();
    if (binding.profile_object_index >= descriptors.size()) {
        return 0U;
    }
    const auto& descriptor = descriptors[binding.profile_object_index];
    switch (binding.kind) {
    case object_kind::axis:
        if (descriptor.source_index < values.axes.size()) {
            return static_cast<DWORD>(values.axes[descriptor.source_index]);
        }
        break;
    case object_kind::button:
        if (descriptor.source_index < values.buttons.size()) {
            return values.buttons[descriptor.source_index] == 0U ? 0U : 0x80U;
        }
        break;
    case object_kind::pov:
        return values.pov;
    }
    return 0U;
}

[[nodiscard]] bool valid_sample(const history_sample& sample) noexcept {
    return sample.source_timestamp_us != 0U && is_valid_gamepad_state(sample.state);
}

} // namespace

struct buffered_subscription::implementation final {
    explicit implementation(buffer_configuration supplied)
        : configuration(std::move(supplied)),
          capacity(static_cast<std::size_t>(
              std::min(configuration.requested_size, maximum_buffer_size) == 0U
                  ? 0U
                  : std::min(configuration.requested_size, maximum_buffer_size) - 1U)),
          queue(capacity) {}

    void set_loss(bool continuity_unknown) noexcept {
        overflow_latched = true;
        overflow_resolved = !continuity_unknown && !continuity_lost;
        if (continuity_unknown) {
            continuity_lost = true;
            resync_candidate = false;
        }
        notification_dirty = true;
    }

    void append_event(const DIDEVICEOBJECTDATA& event) noexcept {
        notification_dirty = true;
        if (capacity == 0U) {
            if (configuration.requested_size != 0U) {
                set_loss(false);
            }
            return;
        }
        if (queued == capacity) {
            set_loss(false);
            return;
        }
        const auto tail = (head + queued) % capacity;
        queue[tail] = event;
        ++queued;
    }

    void remember(const history_sample& sample) noexcept {
        last_sample = sample;
        last_values = project(sample.state, configuration.axes);
        has_predecessor = true;
    }

    void emit_changes(const history_sample& sample, DWORD sequence) noexcept {
        const auto next = project(sample.state, configuration.axes);
        for (DWORD index = 0U; index < configuration.format.binding_count; ++index) {
            const auto& binding = configuration.format.bindings[index];
            if (binding.profile_object_index == no_profile_object ||
                binding.profile_object_index >= objects().size() ||
                (is_guide_object(binding) && !sample.guide_known)) {
                continue;
            }
            const auto value = value_for(binding, next);
            if (value == value_for(binding, last_values)) {
                continue;
            }
            const auto elapsed_ms = (sample.source_timestamp_us - cut.clock.timestamp_us) / 1000U;
            DIDEVICEOBJECTDATA event{};
            event.dwOfs = binding.data_offset;
            event.dwData = value;
            event.dwTimeStamp = static_cast<DWORD>(cut.clock.system_time_ms + elapsed_ms);
            event.dwSequence = sequence;
            append_event(event);
        }
    }

    void check_record_sequence(const history_record& record) noexcept {
        if (last_history_sequence.has_value() &&
            (*last_history_sequence == std::numeric_limits<std::uint64_t>::max() ||
                record.sequence != *last_history_sequence + 1U)) {
            set_loss(true);
        }
        last_history_sequence = record.sequence;
    }

    void remember_pre_cut(const history_sample& sample) noexcept {
        if (!sample.guide_known) {
            set_loss(true);
            return;
        }
        remember(sample);
        if (continuity_lost) {
            resync_candidate = true;
        }
    }

    void accept_at_cut(const history_record& record) noexcept {
        const auto& sample = record.sample;
        if (sample.source_timestamp_us == cut.clock.timestamp_us) {
            set_loss(true);
            if (sample.guide_known) {
                remember(sample);
                resync_candidate = true;
            }
            return;
        }
        if (sample.source_timestamp_us > cut.clock.timestamp_us) {
            return;
        }
        if (record.origin == history_record_origin::callback_guide) {
            set_loss(true);
            if (sample.guide_known) {
                remember(sample);
                resync_candidate = true;
            }
            return;
        }
        remember_pre_cut(sample);
    }

    void accept_after_cutoff(const history_record& record) noexcept {
        const auto& sample = record.sample;
        if (sample.source_timestamp_us > cut.clock.timestamp_us) {
            return;
        }
        if (sample.source_timestamp_us == cut.clock.timestamp_us || crossed_cut ||
            record.origin == history_record_origin::callback_guide) {
            set_loss(true);
        }
        if (sample.guide_known && continuity_lost) {
            remember(sample);
            resync_candidate = true;
        } else if (sample.guide_known && !crossed_cut) {
            remember(sample);
        }
    }

    void accept_record(const history_record& record, DWORD sequence) noexcept {
        const auto& sample = record.sample;
        if (!valid_sample(sample)) {
            set_loss(true);
            return;
        }
        check_record_sequence(record);
        if (last_sample.has_value() &&
            sample.source_timestamp_us < last_sample->source_timestamp_us) {
            set_loss(true);
        }

        if (current_domain == cut.domain && record.sequence <= cut.sequence && !crossed_cut) {
            accept_at_cut(record);
            return;
        }
        if (sample.source_timestamp_us <= cut.clock.timestamp_us) {
            accept_after_cutoff(record);
            return;
        }
        crossed_cut = true;
        if (!sample.guide_known) {
            set_loss(true);
            return;
        }
        if (record.baseline || !has_predecessor || continuity_lost) {
            if (!has_predecessor && !record.baseline) {
                set_loss(true);
            }
            remember(sample);
            if (continuity_lost) {
                resync_candidate = true;
            }
            return;
        }
        emit_changes(sample, sequence);
        remember(sample);
    }

    void finish_read(const history_detail::observed_read& read, bool busy) noexcept {
        if (read.batch.failure.has_value() && !busy) {
            const auto error = read.batch.failure->code;
            const bool has_known_predecessor =
                has_predecessor && last_sample.has_value() && last_sample->guide_known;
            set_loss(true);
            resync_candidate = has_known_predecessor;
            if (error == history_error::disconnected || error == history_error::closed ||
                error == history_error::invalid_reader ||
                error == history_error::sequence_exhausted ||
                error == history_error::identity_exhausted) {
                terminal = true;
                terminal_result = history_error_result(error);
            }
        }
        if (read.fresh_checkpoint && read.batch.caught_up && read.batch.checkpoint_qpc != 0U &&
            continuity_lost && resync_candidate && last_history_sequence.has_value() &&
            read.batch.checkpoint_sequence == *last_history_sequence) {
            continuity_lost = false;
            resync_candidate = false;
            overflow_resolved = true;
        }
        if (terminal && acquired) {
            notification_dirty = true;
        }
    }

    mutable std::mutex gate;
    std::condition_variable ready;
    std::mutex signal_gate;
    buffer_configuration configuration;
    std::size_t capacity{};
    std::vector<DIDEVICEOBJECTDATA> queue;
    std::size_t head{};
    std::size_t queued{};
    history_detail::reader_boundary cut{};
    history_domain current_domain{};
    std::optional<history_sample> last_sample;
    profile_values last_values{};
    std::optional<std::uint64_t> last_history_sequence;
    HRESULT terminal_result{DIERR_INPUTLOST};
    bool prepared{};
    bool acquired{};
    bool terminal{};
    bool canceled{};
    bool has_predecessor{};
    bool continuity_lost{};
    bool resync_candidate{};
    bool crossed_cut{};
    bool overflow_latched{};
    bool overflow_resolved{};
    bool notification_dirty{};
};

buffered_subscription::buffered_subscription(buffer_configuration configuration)
    : implementation_(std::make_unique<implementation>(std::move(configuration))) {
}

buffered_subscription::~buffered_subscription() {
    cancel();
    synchronize_signals();
}

connection_target buffered_subscription::target() const noexcept {
    return implementation_->configuration.target;
}

std::optional<focus::window_epoch> buffered_subscription::window() const noexcept {
    return implementation_->configuration.window;
}

bool buffered_subscription::pending() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return !implementation_->prepared && !implementation_->terminal;
}

bool buffered_subscription::active() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return implementation_->acquired && !implementation_->terminal;
}

bool buffered_subscription::finished() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return implementation_->terminal;
}

HRESULT buffered_subscription::wait_ready() {
    auto& state = *implementation_;
    std::unique_lock lock{state.gate};
    state.ready.wait(lock, [&state] { return state.prepared || state.terminal; });
    if (state.prepared && !state.terminal) {
        return DI_OK;
    }
    return state.terminal_result;
}

void buffered_subscription::prepare(const history_detail::reader_boundary& boundary,
    std::optional<history_sample> baseline) noexcept {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.gate};
        if (state.terminal || state.prepared) {
            return;
        }
        state.cut = boundary;
        state.current_domain = boundary.domain;
        state.prepared = true;
        state.last_history_sequence = boundary.sequence;
        if (boundary.domain.target != state.configuration.target || boundary.domain.stream == 0U ||
            boundary.clock.timestamp_us == 0U) {
            state.terminal = true;
            state.terminal_result = DIERR_INPUTLOST;
            state.set_loss(true);
            state.ready.notify_all();
            return;
        }
        if (boundary.checkpoint_qpc == 0U) {
            state.set_loss(true);
        }
        if (baseline.has_value()) {
            if (!valid_sample(*baseline) || !baseline->guide_known ||
                baseline->source_timestamp_us >= boundary.clock.timestamp_us) {
                state.set_loss(true);
                if (valid_sample(*baseline)) {
                    state.remember(*baseline);
                    state.resync_candidate = baseline->guide_known;
                }
            } else {
                state.remember(*baseline);
                state.resync_candidate = state.continuity_lost;
            }
        } else {
            state.set_loss(true);
        }
    }
    state.ready.notify_all();
}

bool buffered_subscription::activate() noexcept {
    auto& state = *implementation_;
    std::scoped_lock lock{state.gate};
    if (!state.prepared || state.terminal || state.canceled) {
        return false;
    }
    if (state.acquired) {
        return false;
    }
    state.acquired = true;
    if (state.queued != 0U || state.overflow_latched) {
        state.notification_dirty = true;
    }
    return true;
}

void buffered_subscription::accept(const history_detail::observed_read& read,
    const std::array<DWORD, history_batch_capacity>& sequences) noexcept {
    auto& state = *implementation_;
    std::scoped_lock lock{state.gate};
    if (!state.prepared || state.terminal || state.canceled) {
        return;
    }
    const bool busy =
        read.batch.failure.has_value() && read.batch.failure->code == history_error::busy;
    if (busy && read.batch.count == 0U) {
        return;
    }

    const auto domain = read.batch.domain;
    if (domain.target != state.configuration.target) {
        state.terminal = true;
        state.terminal_result = DIERR_INPUTLOST;
        state.set_loss(true);
        state.ready.notify_all();
        return;
    }
    if (domain.stream == 0U) {
        state.set_loss(true);
        state.ready.notify_all();
        return;
    }
    if (domain != state.current_domain) {
        state.set_loss(true);
        state.current_domain = domain;
        state.last_history_sequence.reset();
        state.has_predecessor = false;
        state.last_sample.reset();
    }
    if (read.batch.reset != history_reset::none || read.batch.loss != history_loss::none) {
        state.set_loss(true);
    }

    const auto count = std::min(read.batch.count, history_batch_capacity);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& record = read.batch.records[index];
        if (state.current_domain != state.cut.domain && record.baseline) {
            state.last_history_sequence.reset();
        }
        if (state.last_history_sequence.has_value() &&
            record.sequence <= *state.last_history_sequence) {
            state.set_loss(true);
            state.last_history_sequence = record.sequence;
            state.accept_record(record, sequences[index]);
            continue;
        }
        state.accept_record(record, sequences[index]);
    }

    state.finish_read(read, busy);
    state.ready.notify_all();
}

HRESULT buffered_subscription::read(DWORD stride,
    DIDEVICEOBJECTDATA* destination,
    DWORD requested,
    DWORD& returned,
    bool peek) noexcept {
    returned = 0U;
    if (!is_supported_stride(stride)) {
        return DIERR_INVALIDPARAM;
    }

    auto& state = *implementation_;
    std::scoped_lock lock{state.gate};
    if (!state.acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (state.terminal) {
        return state.terminal_result;
    }
    if (state.configuration.requested_size == 0U) {
        return DIERR_NOTBUFFERED;
    }

    const auto available = static_cast<DWORD>(state.queued);
    const auto count = requested == INFINITE ? available : std::min(requested, available);
    returned = count;
    for (DWORD index = 0U; destination != nullptr && index < count; ++index) {
        const auto queue_index = (state.head + index) % state.capacity;
        std::memcpy(reinterpret_cast<std::byte*>(destination) +
                        (static_cast<std::size_t>(index) * stride),
            &state.queue[queue_index],
            stride);
    }

    if (!peek && count != 0U) {
        state.head = (state.head + count) % state.capacity;
        state.queued -= count;
    }

    const bool overflow = state.overflow_latched;
    if (overflow && !peek && requested != 0U && state.overflow_resolved) {
        state.overflow_latched = false;
        state.overflow_resolved = false;
    }
    return overflow ? DI_BUFFEROVERFLOW : DI_OK;
}

void buffered_subscription::cancel() noexcept {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.gate};
        if (state.terminal && state.canceled) {
            return;
        }
        state.canceled = true;
        state.terminal = true;
        state.terminal_result = DIERR_INPUTLOST;
        state.notification_dirty = false;
    }
    state.ready.notify_all();
}

void buffered_subscription::lose() noexcept {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.gate};
        if (state.canceled) {
            return;
        }
        state.terminal = true;
        state.terminal_result = DIERR_INPUTLOST;
        state.set_loss(true);
        state.notification_dirty = state.acquired;
    }
    state.ready.notify_all();
}

void buffered_subscription::fail(HRESULT error) noexcept {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.gate};
        if (state.canceled) {
            return;
        }
        state.terminal = true;
        state.terminal_result = FAILED(error) ? error : DIERR_GENERIC;
        if (error == DIERR_INPUTLOST) {
            state.set_loss(true);
            state.notification_dirty = state.acquired;
        }
    }
    state.ready.notify_all();
}

void buffered_subscription::signal_pending() noexcept {
    auto& state = *implementation_;
    std::scoped_lock signal_lock{state.signal_gate};
    std::shared_ptr<event_registration> notification;
    {
        std::scoped_lock lock{state.gate};
        if (state.canceled || !state.acquired || !state.notification_dirty) {
            return;
        }
        state.notification_dirty = false;
        notification = state.configuration.notification;
    }
    if (notification != nullptr) {
        notification->signal();
    }
}

void buffered_subscription::synchronize_signals() noexcept {
    std::scoped_lock lock{implementation_->signal_gate};
}

} // namespace inputnext::dinput
