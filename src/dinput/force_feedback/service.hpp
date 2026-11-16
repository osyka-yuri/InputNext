#pragma once

#include "dinput/force_feedback/parameters.hpp"
#include "dinput/native_types.hpp"
#include "output/composition/composition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace inputnext::output::detail {
class window_acquisition;
}

namespace inputnext::dinput::force_feedback {

inline constexpr std::size_t max_acquisitions = 16U;
inline constexpr std::size_t max_effects_per_device = 16U;
inline constexpr std::size_t max_effects = 64U;

struct motors final {
    bool low{};
    bool high{};
    output::backend_handle backend{};

    friend constexpr bool operator==(const motors&, const motors&) noexcept = default;
};

using motor_query = motors (*)(void* context, connection_target target) noexcept;

struct eligibility final {
    HRESULT status{static_cast<HRESULT>(DIERR_NOTEXCLUSIVEACQUIRED)};
    std::shared_ptr<const output::detail::window_acquisition> token;
    DWORD gain{DI_FFNOMINALMAX};
    bool actuators_on{true};
};

struct start_options final {
    DWORD iterations{};
    DWORD flags{};
};

// Owns the bounded logical effect slots and one shared playback worker for a session.
// Composition and executor are borrowed and must outlive shutdown().
class service final {
public:
    service(output::detail::composition& composition,
        output::executor& executor,
        void* capability_context,
        motor_query query_motors);
    ~service() noexcept;

    service(const service&) = delete;
    service& operator=(const service&) = delete;
    service(service&&) = delete;
    service& operator=(service&&) = delete;

    [[nodiscard]] motors capabilities(connection_target target) const noexcept;

    [[nodiscard]] HRESULT create(const void* device,
        const effect_descriptor& descriptor,
        const effect_parameters& initial,
        std::uint64_t& id) noexcept;
    void release(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT get(std::uint64_t id,
        DIEFFECT* parameters,
        DWORD flags,
        const compiled_format* format) noexcept;
    [[nodiscard]] HRESULT set(std::uint64_t id,
        const DIEFFECT* parameters,
        DWORD flags,
        const compiled_format* format,
        const eligibility& acquisition) noexcept;
    [[nodiscard]] HRESULT
    start(std::uint64_t id, const start_options& options, const eligibility& acquisition) noexcept;
    [[nodiscard]] HRESULT download(std::uint64_t id, const eligibility& acquisition) noexcept;
    [[nodiscard]] HRESULT stop(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT unload(std::uint64_t id) noexcept;
    [[nodiscard]] HRESULT status(std::uint64_t id, DWORD* result) noexcept;
    [[nodiscard]] HRESULT
    command(const void* device, DWORD command, const eligibility& acquisition) noexcept;
    [[nodiscard]] HRESULT
    state(const void* device, DWORD* result, const eligibility& acquisition) noexcept;
    [[nodiscard]] HRESULT set_gain(const void* device, DWORD gain) noexcept;

    // Closes admission, revokes all logical effects, retires each private context,
    // and joins the shared worker before borrowed output services are released.
    void shutdown() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::dinput::force_feedback
