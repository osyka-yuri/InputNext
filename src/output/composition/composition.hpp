#pragma once

#include "internal_producer.hpp"
#include "output/body/audio_route_control.hpp"
#include "output/xinput/rumble.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <memory>

namespace inputnext::output::detail {

// Owns the authority. Registry and observer must outlive this object; producer callers must be
// quiesced before destruction.
class composition final {
public:
    composition(runtime_id runtime,
        device_registry& registry,
        focus::observer& focus_observer,
        authority_seed seed = {});
    ~composition() noexcept = default;

    composition(const composition&) = delete;
    composition& operator=(const composition&) = delete;
    composition(composition&&) = delete;
    composition& operator=(composition&&) = delete;

    [[nodiscard]] authority& public_authority() & noexcept;
    [[nodiscard]] audio_route_control audio_routes() & noexcept;
    [[nodiscard]] result<internal_producer> make_producer(
        const internal_context_options& options) & noexcept;
    [[nodiscard]] result<std::unique_ptr<xinput_rumble>> make_xinput_rumble() & noexcept;

private:
    authority owner_;
};

} // namespace inputnext::output::detail
