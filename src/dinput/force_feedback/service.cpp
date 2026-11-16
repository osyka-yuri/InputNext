#include "service.hpp"

#include "state.hpp"

#include "dinput/force_feedback/parameters.hpp"
#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/window_acquisition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_executor.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace inputnext::dinput::force_feedback {

HRESULT service::implementation::create(const void* device,
    const effect_descriptor& descriptor,
    const effect_parameters& initial,
    std::uint64_t& id) noexcept {
    id = 0U;
    if (device == nullptr || descriptor.id == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    std::scoped_lock lock{gate};
    if (admission_closed) {
        return DIERR_NOTINITIALIZED;
    }
    const auto live = std::ranges::count_if(effects,
        [device](const effect_entry& entry) { return entry.id != 0U && entry.device == device; });
    if (std::cmp_greater_equal(live, max_effects_per_device)) {
        return DIERR_OUTOFMEMORY;
    }
    const auto free =
        std::ranges::find_if(effects, [](const effect_entry& entry) { return entry.id == 0U; });
    if (free == effects.end() || next_effect_id == 0U) {
        return DIERR_OUTOFMEMORY;
    }
    const auto next_id = next_effect_id;
    next_effect_id =
        next_effect_id == std::numeric_limits<std::uint64_t>::max() ? 0U : next_effect_id + 1U;
    *free = effect_entry{};
    free->device = device;
    free->id = next_id;
    free->descriptor = descriptor;
    free->staged = initial;
    free->device_gain = initial.gain;
    id = next_id;
    return S_OK;
}

HRESULT service::implementation::get(const std::uint64_t id,
    DIEFFECT* parameters,
    const DWORD flags,
    const compiled_format* format) noexcept {
    if (parameters == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    std::scoped_lock lock{gate};
    if (admission_closed) {
        return DIERR_NOTINITIALIZED;
    }
    const auto index = find_effect(id);
    if (index == effects.size()) {
        return DIERR_INVALIDPARAM;
    }
    auto& effect = effects[index];
    if (effect.invalidated) {
        return DIERR_NOTINITIALIZED;
    }
    if (effect.downloaded_token != nullptr && !effect.downloaded_token->live()) {
        effect.downloaded_token.reset();
        effect.has_downloaded = false;
    }
    return get_parameters(effect.staged, effect.descriptor, parameters, flags, format);
}

HRESULT service::implementation::set(const std::uint64_t id,
    const DIEFFECT* parameters,
    const DWORD flags,
    const compiled_format* format,
    const eligibility& acquisition,
    const observed_motors observed) noexcept {
    const auto supported = observed.supported;
    const auto controls = flags & set_control_flags;
    if ((flags & ~(parameter_flags | set_control_flags)) != 0U || std::popcount(controls) > 1 ||
        ((flags & parameter_flags) != 0U && parameters == nullptr)) {
        return DIERR_INVALIDPARAM;
    }
    bool drive{};
    HRESULT result = S_OK;
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        if (observed.generation != wake_generation) {
            return DIERR_GENERIC;
        }
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return DIERR_INVALIDPARAM;
        }
        reconcile_backend_locked(acquisition.token, supported, drive);
        reconcile_locked(clock::now(), drive);
        auto& effect = effects[index];
        if (effect.invalidated) {
            return DIERR_NOTINITIALIZED;
        }
        auto candidate = effect.staged;
        const auto selected = flags & parameter_flags;
        if (selected != 0U) {
            const auto updated =
                set_parameters(candidate, effect.descriptor, parameters, selected, format);
            if (FAILED(updated)) {
                return updated;
            }
        }

        const auto supported_parameters =
            effect.descriptor.static_params | effect.descriptor.dynamic_params;
        const auto effective_selected = selected & supported_parameters;
        const auto pending_parameters = effect.pending_parameters | effective_selected;
        if ((flags & DIEP_NORESTART) != 0U && eligible(acquisition) && effect.playing &&
            same_token(effect.active_token, acquisition.token) &&
            (pending_parameters & ~effect.descriptor.dynamic_params) != 0U) {
            return DIERR_EFFECTPLAYING;
        }
        effect.staged = candidate;
        effect.pending_parameters = pending_parameters;

        if ((flags & DIEP_NODOWNLOAD) != 0U) {
            result = DI_DOWNLOADSKIPPED;
        } else {
            result =
                set_downloaded_locked(effect, flags, acquisition, supported, clock::now(), drive);
        }
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::start(const std::uint64_t id,
    const start_options& options,
    const eligibility& acquisition,
    const observed_motors observed) noexcept {
    const auto supported = observed.supported;
    bool drive{};
    HRESULT result{};
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        if (observed.generation != wake_generation) {
            return DIERR_GENERIC;
        }
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return DIERR_INVALIDPARAM;
        }
        if (effects[index].invalidated) {
            return DIERR_NOTINITIALIZED;
        }
        reconcile_backend_locked(acquisition.token, supported, drive);
        reconcile_locked(clock::now(), drive);
        result = start_locked(effects[index], options, acquisition, supported, clock::now(), drive);
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::download(const std::uint64_t id,
    const eligibility& acquisition,
    const observed_motors observed) noexcept {
    const auto supported = observed.supported;
    bool drive{};
    HRESULT result{};
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        if (observed.generation != wake_generation) {
            return DIERR_GENERIC;
        }
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return DIERR_INVALIDPARAM;
        }
        if (effects[index].invalidated) {
            return DIERR_NOTINITIALIZED;
        }
        if (!eligible(acquisition)) {
            return eligibility_error(acquisition);
        }
        reconcile_backend_locked(acquisition.token, supported, drive);
        reconcile_locked(clock::now(), drive);
        const auto& effect = effects[index];
        if (effect.has_downloaded && same_token(effect.downloaded_token, acquisition.token) &&
            effect.pending_parameters == 0U && (supported.low || supported.high)) {
            result = S_FALSE;
        } else {
            result = set_downloaded_locked(effects[index],
                0U,
                acquisition,
                supported,
                clock::now(),
                drive);
            if (result == DI_EFFECTRESTARTED) {
                result = DI_OK;
            }
        }
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::stop(const std::uint64_t id) noexcept {
    bool drive{};
    HRESULT result{};
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return DIERR_INVALIDPARAM;
        }
        if (effects[index].invalidated) {
            return DIERR_NOTINITIALIZED;
        }
        result = retire_effect_token_locked(effects[index], clock::now(), drive);
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::unload(const std::uint64_t id) noexcept {
    bool drive{};
    HRESULT result{};
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return DIERR_INVALIDPARAM;
        }
        auto& effect = effects[index];
        if (effect.invalidated) {
            return DIERR_NOTINITIALIZED;
        }
        result = retire_effect_token_locked(effect, clock::now(), drive);
        effect.has_downloaded = false;
        effect.downloaded_token.reset();
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::status(const std::uint64_t id, DWORD* result) noexcept {
    if (result == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    std::scoped_lock lock{gate};
    if (admission_closed) {
        return DIERR_NOTINITIALIZED;
    }
    const auto index = find_effect(id);
    if (index == effects.size()) {
        return DIERR_INVALIDPARAM;
    }
    auto& effect = effects[index];
    if (effect.invalidated) {
        return DIERR_NOTINITIALIZED;
    }
    if (effect.active_token != nullptr && !effect.active_token->live()) {
        effect.active_token.reset();
        effect.playing = false;
    }
    *result = effect.playing ? DIEGES_PLAYING : 0U;
    return S_OK;
}

HRESULT
service::implementation::set_actuators_locked(const void* device,
    const bool enabled,
    bool& drive) noexcept {
    std::array<std::size_t, max_acquisitions> touched{};
    std::array<bool, max_effects> previous{};
    std::size_t touched_count{};
    for (std::size_t effect_index = 0U; effect_index < effects.size(); ++effect_index) {
        auto& effect = effects[effect_index];
        if (effect.id == 0U || effect.device != device) {
            continue;
        }
        previous[effect_index] = effect.actuators_on;
        effect.actuators_on = enabled;
        if (!effect.playing || effect.active_token == nullptr) {
            continue;
        }
        const auto context_index = find_context(effect.active_token);
        if (context_index < acquisitions.size() &&
            std::ranges::find(touched.begin(),
                touched.begin() + static_cast<std::ptrdiff_t>(touched_count),
                context_index) == touched.begin() + static_cast<std::ptrdiff_t>(touched_count)) {
            touched[touched_count++] = context_index;
        }
    }
    const auto result = update_contexts_locked(touched, touched_count, drive);
    if (FAILED(result)) {
        for (std::size_t index = 0U; index < effects.size(); ++index) {
            if (effects[index].id != 0U && effects[index].device == device) {
                effects[index].actuators_on = previous[index];
            }
        }
        static_cast<void>(update_contexts_locked(touched, touched_count, drive));
    }
    return result;
}

HRESULT service::implementation::reset_device_locked(const void* device, bool& drive) noexcept {
    const auto stopped = stop_device_locked(device, 0U, clock::now(), drive);
    for (auto& effect : effects) {
        if (effect.id == 0U || effect.device != device) {
            continue;
        }
        effect.has_downloaded = false;
        effect.downloaded_token.reset();
        effect.actuators_on = false;
        effect.invalidated = true;
    }
    return stopped;
}

HRESULT service::implementation::update_contexts_locked(
    const std::array<std::size_t, max_acquisitions>& contexts,
    const std::size_t count,
    bool& drive) noexcept {
    HRESULT result = S_OK;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto updated = update_context_locked(contexts[index], clock::now(), drive);
        if (FAILED(updated) && SUCCEEDED(result)) {
            result = updated;
        }
    }
    return result;
}

HRESULT service::implementation::command(const void* device,
    const DWORD command_value,
    const eligibility& acquisition,
    const observed_motors observed) noexcept {
    const auto supported = observed.supported;
    if (device == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    bool drive{};
    HRESULT result = S_OK;
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        if (observed.generation != wake_generation) {
            return DIERR_GENERIC;
        }
        if (!eligible(acquisition)) {
            return eligibility_error(acquisition);
        }
        if (!supported.low && !supported.high) {
            return DIERR_UNSUPPORTED;
        }
        reconcile_backend_locked(acquisition.token, supported, drive);
        reconcile_locked(clock::now(), drive);
        switch (command_value) {
        case DISFFC_STOPALL:
            result = stop_device_locked(device, 0U, clock::now(), drive);
            break;
        case DISFFC_RESET:
            result = reset_device_locked(device, drive);
            break;
        case DISFFC_SETACTUATORSOFF:
        case DISFFC_SETACTUATORSON: {
            const auto enabled = command_value == DISFFC_SETACTUATORSON;
            result = set_actuators_locked(device, enabled, drive);
            break;
        }
        default:
            result = DIERR_UNSUPPORTED;
            break;
        }
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

HRESULT service::implementation::state(const void* device,
    DWORD* result,
    const eligibility& acquisition,
    const observed_motors observed) noexcept {
    const auto supported = observed.supported;
    if (device == nullptr || result == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    std::scoped_lock lock{gate};
    if (admission_closed) {
        return DIERR_NOTINITIALIZED;
    }
    if (observed.generation != wake_generation) {
        return DIERR_GENERIC;
    }
    if (!eligible(acquisition)) {
        return eligibility_error(acquisition);
    }
    if (!supported.low && !supported.high) {
        return DIERR_UNSUPPORTED;
    }
    DWORD flags{};
    if (acquisition.actuators_on && (supported.low || supported.high)) {
        flags |= DIGFFS_ACTUATORSON;
    } else {
        flags |= DIGFFS_ACTUATORSOFF;
    }
    bool has_effect{};
    bool is_playing{};
    for (const auto& effect : effects) {
        if (effect.id != 0U && effect.device == device && effect.has_downloaded &&
            same_token(effect.downloaded_token, acquisition.token) &&
            effect.supported == supported) {
            has_effect = true;
            is_playing = is_playing || effect.playing;
        }
    }
    if (!has_effect) {
        flags |= DIGFFS_EMPTY;
    } else if (!is_playing) {
        flags |= DIGFFS_STOPPED;
    }
    *result = flags;
    return S_OK;
}

HRESULT service::implementation::set_gain(const void* device, const DWORD gain) noexcept {
    if (device == nullptr || gain > DI_FFNOMINALMAX) {
        return DIERR_INVALIDPARAM;
    }
    bool drive{};
    HRESULT result = S_OK;
    {
        std::scoped_lock lock{gate};
        if (admission_closed) {
            return DIERR_NOTINITIALIZED;
        }
        std::array<std::size_t, max_acquisitions> touched{};
        std::array<DWORD, max_effects> previous{};
        std::size_t touched_count{};
        for (std::size_t effect_index = 0U; effect_index < effects.size(); ++effect_index) {
            auto& effect = effects[effect_index];
            if (effect.id == 0U || effect.device != device) {
                continue;
            }
            previous[effect_index] = effect.device_gain;
            effect.device_gain = gain;
            if (!effect.playing || effect.active_token == nullptr) {
                continue;
            }
            const auto context_index = find_context(effect.active_token);
            if (context_index < acquisitions.size() &&
                std::ranges::find(touched.begin(),
                    touched.begin() + static_cast<std::ptrdiff_t>(touched_count),
                    context_index) ==
                    touched.begin() + static_cast<std::ptrdiff_t>(touched_count)) {
                touched[touched_count++] = context_index;
            }
        }
        result = update_contexts_locked(touched, touched_count, drive);
        if (FAILED(result)) {
            for (std::size_t index = 0U; index < effects.size(); ++index) {
                if (effects[index].id != 0U && effects[index].device == device) {
                    effects[index].device_gain = previous[index];
                }
            }
            static_cast<void>(update_contexts_locked(touched, touched_count, drive));
        }
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
    return result;
}

void service::implementation::release(const std::uint64_t id) noexcept {
    bool drive{};
    {
        std::scoped_lock lock{gate};
        const auto index = find_effect(id);
        if (index == effects.size()) {
            return;
        }
        const auto token = effects[index].active_token;
        effects[index] = effect_entry{};
        if (token != nullptr) {
            const auto context_index = find_context(token);
            if (context_index < acquisitions.size()) {
                static_cast<void>(update_context_locked(context_index, clock::now(), drive));
            }
        }
        signal_locked();
    }
    if (drive) {
        executor.request_drive();
    }
}

void service::implementation::shutdown() noexcept {
    std::scoped_lock shutdown_lock{shutdown_gate};
    {
        std::scoped_lock lock{gate};
        if (shutdown_complete) {
            return;
        }
        admission_closed = true;
        worker_stop = true;
        for (auto& effect : effects) {
            effect.playing = false;
            effect.active_token.reset();
            effect.has_downloaded = false;
            effect.downloaded_token.reset();
        }
        signal_locked();
    }
    if (worker.joinable()) {
        worker.join();
    }

    bool drive{};
    {
        std::scoped_lock lock{gate};
        for (auto& acquisition : acquisitions) {
            acquisition.output.retire(drive);
        }
        shutdown_complete = true;
    }
    if (drive) {
        executor.request_drive();
    }
}

service::service(output::detail::composition& composition,
    output::executor& executor,
    void* capability_context,
    const motor_query query_motors)
    : implementation_(std::make_unique<implementation>(composition,
          executor,
          capability_context,
          query_motors)) {
}

service::~service() noexcept {
    shutdown();
}

motors service::capabilities(const connection_target target) const noexcept {
    return implementation_->capabilities(target);
}

HRESULT service::create(const void* device,
    const effect_descriptor& descriptor,
    const effect_parameters& initial,
    std::uint64_t& id) noexcept {
    return implementation_->create(device, descriptor, initial, id);
}

void service::release(const std::uint64_t id) noexcept {
    implementation_->release(id);
}

HRESULT service::get(const std::uint64_t id,
    DIEFFECT* parameters,
    const DWORD flags,
    const compiled_format* format) noexcept {
    return implementation_->get(id, parameters, flags, format);
}

HRESULT service::set(const std::uint64_t id,
    const DIEFFECT* parameters,
    const DWORD flags,
    const compiled_format* format,
    const eligibility& acquisition) noexcept {
    const auto supported = implementation_->observe_capabilities(acquisition);
    return implementation_->set(id, parameters, flags, format, acquisition, supported);
}

HRESULT service::start(const std::uint64_t id,
    const start_options& options,
    const eligibility& acquisition) noexcept {
    const auto supported = implementation_->observe_capabilities(acquisition);
    return implementation_->start(id, options, acquisition, supported);
}

HRESULT service::download(const std::uint64_t id, const eligibility& acquisition) noexcept {
    const auto supported = implementation_->observe_capabilities(acquisition);
    return implementation_->download(id, acquisition, supported);
}

HRESULT service::stop(const std::uint64_t id) noexcept {
    return implementation_->stop(id);
}

HRESULT service::unload(const std::uint64_t id) noexcept {
    return implementation_->unload(id);
}

HRESULT service::status(const std::uint64_t id, DWORD* result) noexcept {
    return implementation_->status(id, result);
}

HRESULT
service::command(const void* device, const DWORD command, const eligibility& acquisition) noexcept {
    const auto supported = implementation_->observe_capabilities(acquisition);
    return implementation_->command(device, command, acquisition, supported);
}

HRESULT service::state(const void* device, DWORD* result, const eligibility& acquisition) noexcept {
    const auto supported = implementation_->observe_capabilities(acquisition);
    return implementation_->state(device, result, acquisition, supported);
}

HRESULT service::set_gain(const void* device, const DWORD gain) noexcept {
    return implementation_->set_gain(device, gain);
}

void service::shutdown() noexcept {
    implementation_->shutdown();
}

} // namespace inputnext::dinput::force_feedback
