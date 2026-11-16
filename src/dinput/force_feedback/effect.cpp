#include "dinput/force_feedback/effect.hpp"

#include "dinput/device_input.hpp"
#include "dinput/device_properties.hpp"
#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/service.hpp"
#include "dinput/input_access.hpp"

#include <dinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext::dinput::force_feedback {
namespace {

template <typename Operation>
HRESULT guarded(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return DIERR_OUTOFMEMORY;
    } catch (...) {
        return DIERR_GENERIC;
    }
}

[[nodiscard]] eligibility current_eligibility(const device_input::feedback_capture& captured,
    DWORD gain,
    bool actuators_on) noexcept {
    return eligibility{.status = captured.status,
        .token = captured.token,
        .gain = gain,
        .actuators_on = actuators_on};
}

} // namespace

class effect final : public IDirectInputEffect {
public:
    effect(device_effects& owner,
        IDirectInputDevice8A& parent,
        const effect_descriptor& descriptor,
        std::uint64_t id) noexcept
        : owner_(owner),
          parent_(parent),
          descriptor_(descriptor),
          id_(id) {
        parent_.AddRef();
    }

    ~effect() {
        owner_.release(this, id_);
        parent_.Release();
    }

    effect(const effect&) = delete;
    effect& operator=(const effect&) = delete;
    effect(effect&&) = delete;
    effect& operator=(effect&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirectInputEffect) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IDirectInputEffect*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        auto observed = references_.load(std::memory_order_relaxed);
        while (observed != 0U && observed != std::numeric_limits<ULONG>::max()) {
            if (references_.compare_exchange_weak(observed,
                    observed + 1U,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return observed + 1U;
            }
        }
        return observed;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        auto observed = references_.load(std::memory_order_acquire);
        while (observed != 0U) {
            if (references_.compare_exchange_weak(observed,
                    observed - 1U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (observed == 1U) {
                    delete this;
                    return 0U;
                }
                return observed - 1U;
            }
        }
        return 0U;
    }

    [[nodiscard]] bool try_retain() noexcept {
        auto observed = references_.load(std::memory_order_acquire);
        while (observed != 0U && observed != std::numeric_limits<ULONG>::max()) {
            if (references_.compare_exchange_weak(observed,
                    observed + 1U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    HRESULT STDMETHODCALLTYPE Initialize([[maybe_unused]] HINSTANCE instance,
        [[maybe_unused]] DWORD version,
        [[maybe_unused]] REFGUID device_instance) noexcept override {
        return DIERR_ALREADYINITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID out) noexcept override {
        if (out == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        *out = *descriptor_.id;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT parameters, DWORD flags) noexcept override {
        return owner_.get_parameters(id_, parameters, flags);
    }

    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT parameters, DWORD flags) noexcept override {
        return owner_.set_parameters(id_, parameters, flags);
    }

    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD flags) noexcept override {
        constexpr DWORD supported = DIES_SOLO | DIES_NODOWNLOAD;
        return (flags & ~supported) != 0U ? DIERR_INVALIDPARAM
                                          : owner_.start(id_, iterations, flags);
    }

    HRESULT STDMETHODCALLTYPE Stop() noexcept override { return owner_.stop(id_); }

    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD status) noexcept override {
        return owner_.effect_status(id_, status);
    }

    HRESULT STDMETHODCALLTYPE Download() noexcept override { return owner_.download(id_); }

    HRESULT STDMETHODCALLTYPE Unload() noexcept override { return owner_.unload(id_); }

    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) noexcept override {
        if (escape == nullptr || escape->dwSize != sizeof(*escape) ||
            (escape->cbInBuffer != 0U && escape->lpvInBuffer == nullptr) ||
            (escape->cbOutBuffer != 0U && escape->lpvOutBuffer == nullptr)) {
            return DIERR_INVALIDPARAM;
        }
        return DIERR_UNSUPPORTED;
    }

private:
    device_effects& owner_;
    IDirectInputDevice8A& parent_;
    const effect_descriptor& descriptor_;
    const std::uint64_t id_;
    std::atomic<ULONG> references_{1U};
};

namespace {

class effect_snapshot final {
public:
    ~effect_snapshot() {
        for (std::size_t index = 0U; index < count_; ++index) {
            static_cast<void>(effects_[index]->Release());
        }
    }

    effect_snapshot(const effect_snapshot&) = delete;
    effect_snapshot& operator=(const effect_snapshot&) = delete;
    effect_snapshot(effect_snapshot&&) = delete;
    effect_snapshot& operator=(effect_snapshot&&) = delete;

    effect_snapshot() = default;

    void append(effect* value) noexcept { effects_[count_++] = value; }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] effect* operator[](std::size_t index) const noexcept { return effects_[index]; }

private:
    std::array<effect*, max_effects_per_device> effects_{};
    std::size_t count_{};
};

[[nodiscard]] bool valid_gain_property(const DIPROPHEADER* header) noexcept {
    return header != nullptr && valid_property_header(*header) &&
           header->dwSize == sizeof(DIPROPDWORD) && header->dwHow == DIPH_DEVICE &&
           header->dwObj == 0U;
}

[[nodiscard]] DIPROPDWORD read_dword_property(const DIPROPHEADER* header) noexcept {
    DIPROPDWORD value{};
    std::memcpy(&value, header, sizeof(value));
    return value;
}

[[nodiscard]] HRESULT prepare_initial_parameters(effect_parameters& initial,
    const effect_descriptor& descriptor,
    const DIEFFECT* parameters,
    const compiled_format* format) noexcept {
    if (parameters == nullptr) {
        return DI_OK;
    }
    const DWORD selected = parameters->dwSize == sizeof(DIEFFECT_DX5)
                               ? static_cast<DWORD>(DIEP_ALLPARAMS_DX5)
                               : static_cast<DWORD>(DIEP_ALLPARAMS);
    return force_feedback::set_parameters(initial, descriptor, parameters, selected, format);
}

void write_dword_property(DIPROPHEADER* header, const DIPROPDWORD& value) noexcept {
    std::memcpy(header, &value, sizeof(value));
}

} // namespace

device_effects::device_effects(IDirectInputDevice8A& parent,
    std::shared_ptr<input_access> access,
    device_input& input) noexcept
    : parent_(parent),
      access_(std::move(access)),
      input_(input),
      device_key_(this) {
}

HRESULT device_effects::create(REFGUID id,
    const DIEFFECT* parameters,
    IDirectInputEffect** out,
    IUnknown* outer) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }
    const auto* const descriptor = find_effect(id);
    if (descriptor == nullptr) {
        return DIERR_UNSUPPORTED;
    }

    return guarded([&] -> HRESULT {
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        if (!input_.feedback_capable(call)) {
            return DIERR_UNSUPPORTED;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_UNSUPPORTED;
        }
        std::scoped_lock lock{gate_};
        const auto captured = input_.capture_feedback(call);
        const auto acquisition = current_eligibility(captured, gain_, actuators_on_);
        return create_effect_locked(*output,
            *descriptor,
            parameters,
            captured.has_format ? &captured.format : nullptr,
            acquisition,
            out);
    });
}

HRESULT device_effects::create_effect_locked(service& output,
    const effect_descriptor& descriptor,
    const DIEFFECT* parameters,
    const compiled_format* format,
    const eligibility& acquisition,
    IDirectInputEffect** out) {
    const auto free =
        std::ranges::find_if(slots_, [](const slot& current) { return current.object == nullptr; });
    if (free == slots_.end()) {
        return DIERR_DEVICEFULL;
    }
    effect_parameters initial{};
    const auto prepared = prepare_initial_parameters(initial, descriptor, parameters, format);
    if (FAILED(prepared)) {
        return prepared;
    }

    std::uint64_t id_value{};
    const auto created = output.create(device_key_, descriptor, initial, id_value);
    if (FAILED(created)) {
        return created;
    }
    if (parameters != nullptr && complete(initial) && acquisition.status == DI_OK) {
        const auto downloaded = output.download(id_value, acquisition);
        if (FAILED(downloaded)) {
            output.release(id_value);
            return downloaded;
        }
    }
    auto* const object = new (std::nothrow) effect{*this, parent_, descriptor, id_value};
    if (object == nullptr) {
        output.release(id_value);
        return DIERR_OUTOFMEMORY;
    }
    free->object = object;
    *out = object;
    return DI_OK;
}

HRESULT device_effects::enumerate_created(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
    void* context,
    DWORD flags) noexcept {
    if (callback == nullptr || flags != 0U) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        effect_snapshot snapshot;
        {
            std::scoped_lock lock{gate_};
            for (const auto& current : slots_) {
                if (current.object != nullptr && current.object->try_retain()) {
                    snapshot.append(current.object);
                }
            }
        }
        parent_.AddRef();
        struct parent_release final {
            IDirectInputDevice8A& parent;
            explicit parent_release(IDirectInputDevice8A& value) noexcept : parent(value) {}
            ~parent_release() { parent.Release(); }
            parent_release(const parent_release&) = delete;
            parent_release& operator=(const parent_release&) = delete;
            parent_release(parent_release&&) = delete;
            parent_release& operator=(parent_release&&) = delete;
        };
        const parent_release retained{parent_};
        for (std::size_t index = 0U; index < snapshot.size(); ++index) {
            if (callback(snapshot[index], context) == DIENUM_STOP) {
                break;
            }
        }
        return DI_OK;
    });
}

HRESULT device_effects::get_state(DWORD* state_value) noexcept {
    if (state_value == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        if (!input_.feedback_capable(call)) {
            return DIERR_UNSUPPORTED;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_UNSUPPORTED;
        }
        const auto captured = input_.capture_feedback(call);
        return output->state(device_key_,
            state_value,
            current_eligibility(captured, gain_, actuators_on_));
    });
}

HRESULT device_effects::command(DWORD command_value) noexcept {
    constexpr DWORD supported =
        DISFFC_RESET | DISFFC_STOPALL | DISFFC_SETACTUATORSON | DISFFC_SETACTUATORSOFF;
    if (command_value == 0U || std::popcount(command_value) != 1) {
        return DIERR_INVALIDPARAM;
    }
    if ((command_value & ~supported) != 0U) {
        return DIERR_UNSUPPORTED;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        if (!input_.feedback_capable(call)) {
            return DIERR_UNSUPPORTED;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_UNSUPPORTED;
        }
        const auto captured = input_.capture_feedback(call);
        const auto result = output->command(device_key_,
            command_value,
            current_eligibility(captured, gain_, actuators_on_));
        if (SUCCEEDED(result)) {
            if (command_value == DISFFC_RESET || command_value == DISFFC_SETACTUATORSOFF) {
                actuators_on_ = false;
            } else if (command_value == DISFFC_SETACTUATORSON) {
                actuators_on_ = true;
            }
        }
        return result;
    });
}

HRESULT device_effects::get_gain(const GUID* property, DIPROPHEADER* header) noexcept {
    if (property != &DIPROP_FFGAIN) {
        return DIERR_UNSUPPORTED;
    }
    if (!valid_gain_property(header)) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto value = read_dword_property(header);
        value.dwData = gain_;
        write_dword_property(header, value);
        return DI_OK;
    });
}

HRESULT device_effects::set_gain(const GUID* property, const DIPROPHEADER* header) noexcept {
    if (property != &DIPROP_FFGAIN) {
        return DIERR_UNSUPPORTED;
    }
    if (!valid_gain_property(header)) {
        return DIERR_INVALIDPARAM;
    }
    const auto requested = read_dword_property(header).dwData;
    if (requested > DI_FFNOMINALMAX) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        const auto result = output == nullptr ? DI_OK : output->set_gain(device_key_, requested);
        if (SUCCEEDED(result)) {
            gain_ = requested;
        }
        return result;
    });
}

HRESULT
device_effects::get_parameters(std::uint64_t id, DIEFFECT* parameters, DWORD flags) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_INPUTLOST;
        }
        const auto captured = input_.capture_feedback(call);
        return output->get(id, parameters, flags, captured.has_format ? &captured.format : nullptr);
    });
}

HRESULT
device_effects::set_parameters(std::uint64_t id, const DIEFFECT* parameters, DWORD flags) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_INPUTLOST;
        }
        const auto captured = input_.capture_feedback(call);
        return output->set(id,
            parameters,
            flags,
            captured.has_format ? &captured.format : nullptr,
            current_eligibility(captured, gain_, actuators_on_));
    });
}

HRESULT device_effects::start(std::uint64_t id, DWORD iterations, DWORD flags) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        if (call.feedback() == nullptr) {
            return DIERR_INPUTLOST;
        }
        const auto captured = input_.capture_feedback(call);
        return call.feedback()->start(id,
            start_options{.iterations = iterations, .flags = flags},
            current_eligibility(captured, gain_, actuators_on_));
    });
}

HRESULT device_effects::stop(std::uint64_t id) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        return output == nullptr ? DIERR_INPUTLOST : output->stop(id);
    });
}

HRESULT device_effects::download(std::uint64_t id) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        if (output == nullptr) {
            return DIERR_INPUTLOST;
        }
        const auto captured = input_.capture_feedback(call);
        return output->download(id, current_eligibility(captured, gain_, actuators_on_));
    });
}

HRESULT device_effects::unload(std::uint64_t id) noexcept {
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        return output == nullptr ? DIERR_INPUTLOST : output->unload(id);
    });
}

HRESULT device_effects::effect_status(std::uint64_t id, DWORD* status) noexcept {
    if (status == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        auto* const output = call.feedback();
        return output == nullptr ? DIERR_INPUTLOST : output->status(id, status);
    });
}

void device_effects::release(effect* object, std::uint64_t id) noexcept {
    std::scoped_lock lock{gate_};
    const auto call = access_->enter();
    if (call) {
        if (auto* const output = call.feedback(); output != nullptr) {
            output->release(id);
        }
    }
    for (auto& current : slots_) {
        if (current.object == object) {
            current = {};
            break;
        }
    }
}

} // namespace inputnext::dinput::force_feedback
