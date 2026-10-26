#include <inputnext/output_body_playback.hpp>

#include "body_pcm_mixer.hpp"
#include "body_playback_bank.hpp"

#include <atomic>
#include <cstddef>
#include <span>
#include <utility>

namespace inputnext::output {

body_render_permit::body_render_permit(detail::body_playback_voice_slot& voice,
    std::atomic<bool>& backend_busy,
    const std::size_t requested_frames) noexcept
    : voice_(&voice),
      backend_busy_(&backend_busy),
      requested_frames_(requested_frames) {
}

body_render_permit::body_render_permit(body_render_permit&& other) noexcept
    : voice_(std::exchange(other.voice_, nullptr)),
      backend_busy_(std::exchange(other.backend_busy_, nullptr)),
      requested_frames_(std::exchange(other.requested_frames_, 0U)) {
}

body_render_permit& body_render_permit::operator=(body_render_permit&& other) noexcept {
    if (this != &other) {
        release();
        voice_ = std::exchange(other.voice_, nullptr);
        backend_busy_ = std::exchange(other.backend_busy_, nullptr);
        requested_frames_ = std::exchange(other.requested_frames_, 0U);
    }
    return *this;
}

body_render_permit::~body_render_permit() noexcept {
    release();
}

bool body_render_permit::valid() const noexcept {
    return voice_ != nullptr && backend_busy_ != nullptr;
}

void body_render_permit::release() noexcept {
    auto* const voice = std::exchange(voice_, nullptr);
    auto* const backend_busy = std::exchange(backend_busy_, nullptr);
    requested_frames_ = 0U;
    if (voice == nullptr || backend_busy == nullptr) {
        return;
    }
    voice->busy.store(false, std::memory_order_release);
    backend_busy->store(false, std::memory_order_release);
    backend_busy->notify_all();
}

body_render_result body_render_permit::render(const std::span<float> quad_output) && noexcept {
    const auto submission = std::move(*this).render_for_submission(quad_output);
    return submission.result();
}

body_render_submission body_render_permit::render_for_submission(
    const std::span<float> quad_output) && noexcept {
    auto* const voice = std::exchange(voice_, nullptr);
    auto* const backend_busy = std::exchange(backend_busy_, nullptr);
    const auto requested_frames = std::exchange(requested_frames_, 0U);
    if (voice == nullptr || backend_busy == nullptr) {
        return {};
    }

    const auto rendered = detail::mix_body_pcm_block(voice->samples.view(),
        voice->cursor,
        quad_output,
        requested_frames);
    voice->completed = rendered.completed;
    return body_render_submission{*voice, *backend_busy, rendered};
}

body_render_submission::body_render_submission(detail::body_playback_voice_slot& voice,
    std::atomic<bool>& backend_busy,
    const body_render_result result) noexcept
    : voice_(&voice),
      backend_busy_(&backend_busy),
      result_(result) {
}

body_render_submission::body_render_submission(body_render_submission&& other) noexcept
    : voice_(std::exchange(other.voice_, nullptr)),
      backend_busy_(std::exchange(other.backend_busy_, nullptr)),
      result_(std::exchange(other.result_, body_render_result{})) {
}

body_render_submission& body_render_submission::operator=(body_render_submission&& other) noexcept {
    if (this != &other) {
        release();
        voice_ = std::exchange(other.voice_, nullptr);
        backend_busy_ = std::exchange(other.backend_busy_, nullptr);
        result_ = std::exchange(other.result_, body_render_result{});
    }
    return *this;
}

body_render_submission::~body_render_submission() noexcept {
    release();
}

bool body_render_submission::valid() const noexcept {
    return voice_ != nullptr && backend_busy_ != nullptr;
}

void body_render_submission::release() noexcept {
    auto* const voice = std::exchange(voice_, nullptr);
    auto* const backend_busy = std::exchange(backend_busy_, nullptr);
    result_ = {};
    if (voice == nullptr || backend_busy == nullptr) {
        return;
    }
    voice->busy.store(false, std::memory_order_release);
    backend_busy->store(false, std::memory_order_release);
    backend_busy->notify_all();
}

} // namespace inputnext::output
