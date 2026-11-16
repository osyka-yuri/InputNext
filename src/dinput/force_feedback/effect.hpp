#pragma once

#include "dinput/device_info.hpp"
#include "dinput/format.hpp"
#include "dinput/native_types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace inputnext::dinput {

class device_input;
class input_access;

namespace force_feedback {

class effect;
class service;
struct effect_descriptor;
struct effect_parameters;
struct eligibility;

// Owns a device's COM effect slots and serializes device-wide feedback controls.
class device_effects final {
public:
    device_effects(IDirectInputDevice8A& parent,
        std::shared_ptr<input_access> access,
        device_input& input) noexcept;
    ~device_effects() = default;

    device_effects(const device_effects&) = delete;
    device_effects& operator=(const device_effects&) = delete;
    device_effects(device_effects&&) = delete;
    device_effects& operator=(device_effects&&) = delete;

    [[nodiscard]] HRESULT create(REFGUID id,
        const DIEFFECT* parameters,
        IDirectInputEffect** out,
        IUnknown* outer) noexcept;
    [[nodiscard]] HRESULT enumerate_created(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
        void* context,
        DWORD flags) noexcept;
    [[nodiscard]] HRESULT get_state(DWORD* state) noexcept;
    [[nodiscard]] HRESULT command(DWORD command) noexcept;
    [[nodiscard]] HRESULT get_gain(const GUID* property, DIPROPHEADER* header) noexcept;
    [[nodiscard]] HRESULT set_gain(const GUID* property, const DIPROPHEADER* header) noexcept;

private:
    friend class effect;

    struct slot final {
        effect* object{};
    };

    [[nodiscard]] HRESULT
    get_parameters(std::uint64_t id, DIEFFECT* parameters, DWORD flags) noexcept;
    [[nodiscard]] HRESULT create_effect_locked(service& output,
        const effect_descriptor& descriptor,
        const DIEFFECT* parameters,
        const compiled_format* format,
        const eligibility& acquisition,
        IDirectInputEffect** out);
    [[nodiscard]] HRESULT
    set_parameters(std::uint64_t id, const DIEFFECT* parameters, DWORD flags) noexcept;
    [[nodiscard]] HRESULT start(std::uint64_t id, DWORD iterations, DWORD flags) noexcept;
    [[nodiscard]] HRESULT stop(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT download(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT unload(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT effect_status(std::uint64_t id, DWORD* status) noexcept;
    void release(effect* object, std::uint64_t id) noexcept;
    IDirectInputDevice8A& parent_;
    const std::shared_ptr<input_access> access_;
    device_input& input_;
    const void* const device_key_;
    std::mutex gate_;
    std::array<slot, 16U> slots_{};
    DWORD gain_{DI_FFNOMINALMAX};
    bool actuators_on_{true};
};

} // namespace force_feedback
} // namespace inputnext::dinput
