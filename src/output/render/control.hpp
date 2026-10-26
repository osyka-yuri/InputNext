#pragma once

#include "output/authority/admitted_body_render.hpp"

#include <inputnext/output_executor.hpp>

#include <cstddef>
#include <optional>

namespace inputnext::output::detail {

// Narrow internal capability for native output workers. The executor must outlive this object.
class body_render_control final {
public:
    explicit body_render_control(executor& owner) noexcept : owner_(owner) {}

    ~body_render_control() = default;

    body_render_control(const body_render_control&) = delete;
    body_render_control& operator=(const body_render_control&) = delete;
    body_render_control(body_render_control&&) = delete;
    body_render_control& operator=(body_render_control&&) = delete;

    [[nodiscard]] execution_result<std::optional<admitted_body_render>> admit_current(
        backend_handle expected_backend,
        audio_instance expected_audio,
        std::size_t requested_frames) const noexcept;

private:
    executor& owner_;
};

} // namespace inputnext::output::detail
