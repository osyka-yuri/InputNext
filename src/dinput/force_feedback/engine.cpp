#include "state.hpp"

#include "contribution.hpp"
#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/playback.hpp"
#include "dinput/native_types.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/window_acquisition.hpp"
#include "service.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace inputnext::dinput::force_feedback {

bool service::implementation::eligible(const eligibility& acquisition) noexcept {
    return SUCCEEDED(acquisition.status) && acquisition.token != nullptr &&
           acquisition.token->live() && acquisition.token->target().valid();
}

HRESULT service::implementation::eligibility_error(const eligibility& acquisition) noexcept {
    if (FAILED(acquisition.status)) {
        return acquisition.status;
    }
    return DIERR_INPUTLOST;
}

bool service::implementation::same_token(
    const std::shared_ptr<const output::detail::window_acquisition>& left,
    const std::shared_ptr<const output::detail::window_acquisition>& right) noexcept {
    return left.get() == right.get();
}

bool service::implementation::nonzero(const output::rumble_value& value) noexcept {
    return value.low_frequency > 0.0F || value.high_frequency > 0.0F;
}

service::implementation::implementation(output::detail::composition& output_composition,
    output::executor& output_executor,
    void* query_context,
    const motor_query query) noexcept
    : composition(output_composition),
      executor(output_executor),
      capability_context(query_context),
      query_motors(query) {
}

motors service::implementation::capabilities(const connection_target target) const noexcept {
    if (!target.valid() || query_motors == nullptr) {
        return {};
    }
    const auto supported = query_motors(capability_context, target);
    return (supported.low || supported.high) && supported.backend.valid() &&
                   supported.backend.target == target
               ? supported
               : motors{};
}

service::implementation::observed_motors service::implementation::observe_capabilities(
    const eligibility& acquisition) noexcept {
    observed_motors observed{};
    {
        std::scoped_lock lock{gate};
        observed.generation = wake_generation;
    }
    if (acquisition.token != nullptr && SUCCEEDED(acquisition.status)) {
        observed.supported = capabilities(acquisition.token->target());
    }
    return observed;
}

std::size_t service::implementation::find_effect(const std::uint64_t id) const noexcept {
    for (std::size_t index = 0U; index < effects.size(); ++index) {
        if (effects[index].id == id && id != 0U) {
            return index;
        }
    }
    return effects.size();
}

std::size_t service::implementation::find_context(
    const std::shared_ptr<const output::detail::window_acquisition>& token) const noexcept {
    for (std::size_t index = 0U; index < acquisitions.size(); ++index) {
        if (acquisitions[index].output.matches(token)) {
            return index;
        }
    }
    return acquisitions.size();
}

void service::implementation::signal_locked() noexcept {
    if (wake_generation != std::numeric_limits<std::uint64_t>::max()) {
        ++wake_generation;
    }
    changed.notify_all();
}

HRESULT service::implementation::ensure_worker_locked() noexcept {
    if (worker_failed.load(std::memory_order_acquire)) {
        return DIERR_GENERIC;
    }
    if (worker.joinable()) {
        return S_OK;
    }
    if (admission_closed) {
        return DIERR_NOTINITIALIZED;
    }
    try {
        worker = std::thread([this] { run_worker(); });
    } catch (...) {
        return DIERR_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT service::implementation::prepare_start_context_locked(
    const std::shared_ptr<const output::detail::window_acquisition>& token,
    const motors supported,
    std::size_t& context_index,
    bool& drive) noexcept {
    const auto thread = ensure_worker_locked();
    return FAILED(thread) ? thread : open_context_locked(token, context_index, supported, drive);
}

HRESULT service::implementation::open_context_locked(
    const std::shared_ptr<const output::detail::window_acquisition>& token,
    std::size_t& index,
    const motors supported,
    bool& drive) noexcept {
    if (token == nullptr || !token->live() || !token->target().valid()) {
        return DIERR_INPUTLOST;
    }
    index = find_context(token);
    if (index != acquisitions.size()) {
        auto& existing = acquisitions[index];
        if (existing.output.closing()) {
            return DIERR_OUTOFMEMORY;
        }
        existing.supported = supported;
        return S_OK;
    }
    const auto free = std::ranges::find_if(acquisitions,
        [](const acquisition_entry& entry) { return !entry.output.occupied(); });
    if (free == acquisitions.end()) {
        return DIERR_OUTOFMEMORY;
    }
    index = static_cast<std::size_t>(std::distance(acquisitions.begin(), free));
    auto& entry = acquisitions[index];
    entry.supported = supported;
    return entry.output.open(composition, token, supported.backend, drive);
}

void service::implementation::discard_token_locked(
    const std::shared_ptr<const output::detail::window_acquisition>& token) noexcept {
    for (auto& effect : effects) {
        if (effect.id == 0U) {
            continue;
        }
        if (same_token(effect.downloaded_token, token)) {
            effect.downloaded_token.reset();
            effect.has_downloaded = false;
        }
        if (same_token(effect.active_token, token)) {
            effect.active_token.reset();
            effect.playing = false;
        }
    }
}

void service::implementation::reconcile_backend_locked(
    const std::shared_ptr<const output::detail::window_acquisition>& token,
    const motors supported,
    bool& drive) noexcept {
    if (token == nullptr) {
        return;
    }
    const auto context_index = find_context(token);
    bool changed_backend =
        context_index != acquisitions.size() && acquisitions[context_index].supported != supported;
    for (const auto& effect : effects) {
        if (effect.id != 0U && effect.has_downloaded &&
            same_token(effect.downloaded_token, token) && effect.supported != supported) {
            changed_backend = true;
            break;
        }
    }
    if (!changed_backend) {
        return;
    }
    discard_token_locked(token);
    if (context_index != acquisitions.size()) {
        acquisitions[context_index].output.retire(drive);
    }
    signal_locked();
}

service::implementation::mixed_sample service::implementation::mix_locked(const std::size_t index,
    const clock::time_point now,
    const effect_entry* replacement,
    const bool solo) noexcept {
    mixed_sample mixed{};
    const auto& context = acquisitions[index];
    const auto& token = context.output.token();
    for (auto& stored : effects) {
        const auto& effect =
            replacement != nullptr && stored.id == replacement->id ? *replacement : stored;
        if (effect.id == 0U || !effect.playing || !same_token(effect.active_token, token) ||
            (solo && replacement != nullptr && effect.device == replacement->device &&
                effect.id != replacement->id)) {
            continue;
        }
        const auto sample = effect.timeline.evaluate(now);
        if (sample.finished) {
            if (replacement == nullptr) {
                stored.playing = false;
                stored.active_token.reset();
            }
            continue;
        }
        mixed.boundary = std::min(mixed.boundary, sample.next_boundary);
        mixed.playing = true;
        if (!effect.actuators_on || effect.device_gain == 0U) {
            continue;
        }
        const auto gain = static_cast<float>(effect.device_gain) / DI_FFNOMINALMAX;
        if (context.supported.low) {
            mixed.value.low_frequency += sample.amplitude * gain;
        }
        if (context.supported.high) {
            mixed.value.high_frequency += sample.amplitude * gain;
        }
    }
    mixed.value.low_frequency = std::clamp(mixed.value.low_frequency, 0.0F, 1.0F);
    mixed.value.high_frequency = std::clamp(mixed.value.high_frequency, 0.0F, 1.0F);
    return mixed;
}

HRESULT service::implementation::publish_context_locked(const std::size_t index,
    const clock::time_point now,
    clock::time_point& next_boundary,
    bool& drive) noexcept {
    if (index >= acquisitions.size() || !acquisitions[index].output.occupied()) {
        return S_OK;
    }
    auto& context = acquisitions[index];
    if (context.output.closing()) {
        return context.output.close(drive);
    }
    const auto token = context.output.token();
    if (token == nullptr || !token->live()) {
        discard_token_locked(token);
        return context.output.close(drive);
    }

    const auto mixed = mix_locked(index, now);
    next_boundary = std::min(next_boundary, mixed.boundary);
    if (!mixed.playing) {
        return context.output.close(drive);
    }
    if (!nonzero(mixed.value)) {
        return context.output.clear(drive);
    }
    return context.output.submit(mixed.value, now, drive, next_boundary);
}

void service::implementation::reconcile_locked(const clock::time_point now, bool& drive) noexcept {
    for (auto& effect : effects) {
        if (effect.id == 0U) {
            continue;
        }
        if (effect.downloaded_token != nullptr && !effect.downloaded_token->live()) {
            effect.downloaded_token.reset();
            effect.has_downloaded = false;
        }
        if (effect.active_token != nullptr && !effect.active_token->live()) {
            effect.active_token.reset();
            effect.playing = false;
        }
    }
    for (const auto& effect : effects) {
        if (effect.id == 0U || !effect.playing || effect.active_token == nullptr ||
            !effect.active_token->live() ||
            find_context(effect.active_token) != acquisitions.size()) {
            continue;
        }
        std::size_t opened_index{};
        const auto opened =
            open_context_locked(effect.active_token, opened_index, effect.supported, drive);
        if (FAILED(opened) && !effect.active_token->live()) {
            discard_token_locked(effect.active_token);
        }
    }
    for (std::size_t index = 0U; index < acquisitions.size(); ++index) {
        const auto& contribution = acquisitions[index].output;
        if (contribution.occupied() &&
            (contribution.token() == nullptr || !contribution.token()->live() ||
                contribution.closing())) {
            clock::time_point ignored = clock::time_point::max();
            static_cast<void>(publish_context_locked(index, now, ignored, drive));
        }
    }
}

HRESULT service::implementation::update_context_locked(const std::size_t index,
    const clock::time_point now,
    bool& drive) noexcept {
    auto boundary = clock::time_point::max();
    return publish_context_locked(index, now, boundary, drive);
}

HRESULT service::implementation::retire_effect_token_locked(effect_entry& effect,
    const clock::time_point now,
    bool& drive) noexcept {
    if (effect.active_token == nullptr) {
        return S_OK;
    }
    const auto token = effect.active_token;
    effect.active_token.reset();
    effect.playing = false;
    const auto context_index = find_context(token);
    if (context_index == acquisitions.size()) {
        return S_OK;
    }
    return update_context_locked(context_index, now, drive);
}

HRESULT service::implementation::stop_device_locked(const void* device,
    const std::uint64_t except_id,
    const clock::time_point now,
    bool& drive) noexcept {
    std::array<std::shared_ptr<const output::detail::window_acquisition>, max_effects> tokens{};
    std::size_t token_count{};
    for (auto& effect : effects) {
        if (effect.id == 0U || effect.device != device || effect.id == except_id ||
            !effect.playing) {
            continue;
        }
        if (effect.active_token != nullptr &&
            std::ranges::none_of(tokens.begin(),
                tokens.begin() + static_cast<std::ptrdiff_t>(token_count),
                [&effect](const auto& token) { return same_token(token, effect.active_token); })) {
            tokens[token_count++] = effect.active_token;
        }
        effect.playing = false;
        effect.active_token.reset();
    }
    HRESULT result{};
    for (std::size_t index = 0U; index < token_count; ++index) {
        const auto context_index = find_context(tokens[index]);
        if (context_index == acquisitions.size()) {
            continue;
        }
        const auto updated = update_context_locked(context_index, now, drive);
        if (FAILED(updated) && SUCCEEDED(result)) {
            result = updated;
        }
    }
    return result;
}

HRESULT service::implementation::download_locked(effect_entry& effect,
    const effect_parameters& candidate,
    const eligibility& acquisition,
    const motors supported,
    const clock::time_point now,
    bool& drive) noexcept {
    if (!eligible(acquisition)) {
        return eligibility_error(acquisition);
    }
    if (!supported.low && !supported.high) {
        return DIERR_UNSUPPORTED;
    }
    if (!complete(candidate)) {
        return DIERR_INCOMPLETEEFFECT;
    }
    if (effect.active_token != nullptr && !same_token(effect.active_token, acquisition.token)) {
        const auto stopped = retire_effect_token_locked(effect, now, drive);
        if (FAILED(stopped)) {
            return stopped;
        }
    }
    if (effect.has_downloaded && !same_token(effect.downloaded_token, acquisition.token)) {
        effect.downloaded_token.reset();
        effect.has_downloaded = false;
    }
    effect.downloaded = candidate;
    effect.downloaded_token = acquisition.token;
    effect.device_gain = acquisition.gain;
    effect.actuators_on = acquisition.actuators_on;
    effect.supported = supported;
    effect.has_downloaded = true;
    effect.pending_parameters = 0U;
    return S_OK;
}

HRESULT service::implementation::validate_start_locked(const effect_entry& effect,
    const start_options& options,
    const eligibility& acquisition,
    const motors supported,
    effect_parameters& candidate) const noexcept {
    if ((options.flags & ~start_flags) != 0U || options.iterations == 0U) {
        return DIERR_INVALIDPARAM;
    }
    if (!eligible(acquisition)) {
        return eligibility_error(acquisition);
    }
    if (!supported.low && !supported.high) {
        return DIERR_UNSUPPORTED;
    }
    for (const auto& other : effects) {
        if (other.id != 0U && other.device == effect.device && other.playing &&
            !same_token(other.active_token, acquisition.token)) {
            return DIERR_INPUTLOST;
        }
    }

    const bool no_download = (options.flags & DIES_NODOWNLOAD) != 0U;
    if (no_download &&
        (!effect.has_downloaded || !same_token(effect.downloaded_token, acquisition.token))) {
        return DIERR_NOTDOWNLOADED;
    }
    candidate = no_download ? effect.downloaded : effect.staged;
    return complete(candidate) ? S_OK : DIERR_INCOMPLETEEFFECT;
}

void service::implementation::commit_playback_locked(effect_entry& effect,
    effect_entry&& replacement,
    const bool solo,
    const bool any_playing,
    contribution& output,
    bool& drive) noexcept {
    if (solo) {
        for (auto& other : effects) {
            if (other.id != 0U && other.device == effect.device && other.id != effect.id) {
                other.playing = false;
                other.active_token.reset();
            }
        }
    }
    effect = std::move(replacement);
    if (!any_playing) {
        effect.playing = false;
        effect.active_token.reset();
        output.retire(drive);
    }
    signal_locked();
}

HRESULT service::implementation::start_locked(effect_entry& effect,
    const start_options& options,
    const eligibility& acquisition,
    const motors supported,
    const clock::time_point now,
    bool& drive) noexcept {
    effect_parameters candidate{};
    const auto validated =
        validate_start_locked(effect, options, acquisition, supported, candidate);
    if (FAILED(validated)) {
        return validated;
    }

    playback timeline{};
    if (!playback::begin(timeline, effect.descriptor, candidate, options.iterations, now)) {
        return DIERR_INCOMPLETEEFFECT;
    }
    auto replacement = effect;
    if ((options.flags & DIES_NODOWNLOAD) == 0U) {
        replacement.downloaded = candidate;
        replacement.downloaded_token = acquisition.token;
        replacement.has_downloaded = true;
        replacement.pending_parameters = 0U;
    }
    replacement.timeline = timeline;
    replacement.started_at = now;
    replacement.iterations = options.iterations;
    replacement.active_token = acquisition.token;
    replacement.device_gain = acquisition.gain;
    replacement.actuators_on = acquisition.actuators_on;
    replacement.supported = supported;
    replacement.playing = !timeline.evaluate(now).finished;
    if (!replacement.playing) {
        replacement.active_token.reset();
    }
    return replace_playback_locked(effect,
        std::move(replacement),
        acquisition.token,
        (options.flags & DIES_SOLO) != 0U,
        now,
        drive);
}

HRESULT service::implementation::replace_playback_locked(effect_entry& effect,
    effect_entry&& replacement,
    const std::shared_ptr<const output::detail::window_acquisition>& token,
    const bool solo,
    const clock::time_point now,
    bool& drive) noexcept {
    const bool new_context = find_context(token) == acquisitions.size();
    std::size_t context_index{};
    const auto prepared =
        prepare_start_context_locked(token, replacement.supported, context_index, drive);
    if (FAILED(prepared)) {
        if (new_context && context_index < acquisitions.size() &&
            acquisitions[context_index].output.matches(token)) {
            acquisitions[context_index].output.retire(drive);
        }
        return prepared;
    }
    const auto mixed = mix_locked(context_index, now, &replacement, solo);
    auto& output = acquisitions[context_index].output;
    const auto published = nonzero(mixed.value)
                               ? output.submit(mixed.value,
                                     now,
                                     drive,
                                     mixed.boundary,
                                     mutation_policy::preserve_on_failure)
                               : output.clear(drive, mutation_policy::preserve_on_failure);
    if (FAILED(published)) {
        if (new_context) {
            output.retire(drive);
        }
        return published;
    }
    commit_playback_locked(effect, std::move(replacement), solo, mixed.playing, output, drive);
    return S_OK;
}

HRESULT service::implementation::set_downloaded_locked(effect_entry& effect,
    const DWORD flags,
    const eligibility& acquisition,
    const motors supported,
    const clock::time_point now,
    bool& drive) noexcept {
    if (!eligible(acquisition)) {
        return (flags & DIEP_START) != 0U ? eligibility_error(acquisition) : DI_DOWNLOADSKIPPED;
    }
    if (!complete(effect.staged)) {
        return DIERR_INCOMPLETEEFFECT;
    }
    if ((flags & DIEP_START) != 0U) {
        return start_locked(effect,
            start_options{.iterations = 1U},
            acquisition,
            supported,
            now,
            drive);
    }

    const bool preserve_playback =
        effect.playing && same_token(effect.active_token, acquisition.token);
    if (preserve_playback &&
        (effect.pending_parameters & ~effect.descriptor.dynamic_params) != 0U) {
        if ((flags & DIEP_NORESTART) != 0U) {
            return DIERR_EFFECTPLAYING;
        }
        const auto restarted = start_locked(effect,
            start_options{.iterations = effect.iterations},
            acquisition,
            supported,
            now,
            drive);
        return SUCCEEDED(restarted) ? DI_EFFECTRESTARTED : restarted;
    }
    if (!preserve_playback) {
        return download_locked(effect, effect.staged, acquisition, supported, now, drive);
    }
    auto replacement = effect;
    const auto downloaded =
        download_locked(replacement, effect.staged, acquisition, supported, now, drive);
    if (FAILED(downloaded)) {
        return downloaded;
    }
    if (!playback::begin(replacement.timeline,
            effect.descriptor,
            replacement.downloaded,
            effect.iterations,
            effect.started_at)) {
        return DIERR_INCOMPLETEEFFECT;
    }
    replacement.playing = !replacement.timeline.evaluate(now).finished;
    if (!replacement.playing) {
        replacement.active_token.reset();
    }
    return replace_playback_locked(effect,
        std::move(replacement),
        acquisition.token,
        false,
        now,
        drive);
}

void service::implementation::run_worker() noexcept {
    try {
        run_worker_loop();
    } catch (...) {
        try {
            std::scoped_lock lock{gate};
            worker_failed = true;
            worker_stop = true;
            for (auto& effect : effects) {
                effect.playing = false;
                effect.active_token.reset();
            }
            changed.notify_all();
        } catch (...) {
            // Preserve the worker failure even if state-lock recovery is unavailable.
            worker_failed.store(true, std::memory_order_release);
            changed.notify_all();
        }
    }
}

bool service::implementation::refresh_worker_capabilities(std::unique_lock<std::mutex>& lock,
    const std::uint64_t observed_generation,
    bool& drive) {
    std::array<capability_sample, max_acquisitions> samples{};
    std::size_t sample_count{};
    for (const auto& context : acquisitions) {
        if (context.output.occupied() && !context.output.closing()) {
            samples[sample_count++] = {.identity = context.output.identity(),
                .token = context.output.token()};
        }
    }
    lock.unlock();
    for (std::size_t index = 0U; index < sample_count; ++index) {
        auto& sample = samples[index];
        if (sample.token != nullptr) {
            sample.supported = capabilities(sample.token->target());
        }
    }
    lock.lock();
    if (worker_stop || wake_generation != observed_generation) {
        return false;
    }
    for (std::size_t index = 0U; index < sample_count; ++index) {
        const auto& sample = samples[index];
        const auto context_index = find_context(sample.token);
        if (context_index != acquisitions.size() &&
            acquisitions[context_index].output.identity() == sample.identity) {
            reconcile_backend_locked(sample.token, sample.supported, drive);
        }
    }
    return true;
}

void service::implementation::run_worker_loop() {
    std::unique_lock lock{gate};
    while (!worker_stop) {
        const auto observed_generation = wake_generation;
        bool any_work{};
        for (const auto& effect : effects) {
            any_work = any_work || (effect.id != 0U && effect.playing);
        }
        for (const auto& context : acquisitions) {
            any_work = any_work || (context.output.occupied() && context.output.closing());
        }
        if (!any_work) {
            changed.wait(lock, [this, observed_generation] {
                return worker_stop || wake_generation != observed_generation;
            });
            continue;
        }

        const auto next_wakeup = clock::now() + tick_period;
        bool drive{};
        if (!refresh_worker_capabilities(lock, observed_generation, drive)) {
            continue;
        }
        const auto now = clock::now();
        reconcile_locked(now, drive);
        for (std::size_t index = 0U; index < acquisitions.size(); ++index) {
            if (!acquisitions[index].output.occupied()) {
                continue;
            }
            auto boundary = clock::time_point::max();
            static_cast<void>(publish_context_locked(index, now, boundary, drive));
        }
        lock.unlock();
        if (drive) {
            executor.request_drive();
        }
        lock.lock();
        if (!worker_stop && wake_generation == observed_generation) {
            changed.wait_until(lock, next_wakeup, [this, observed_generation] {
                return worker_stop || wake_generation != observed_generation;
            });
        }
    }
}

} // namespace inputnext::dinput::force_feedback
