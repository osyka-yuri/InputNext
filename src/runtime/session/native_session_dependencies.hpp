#pragma once

#include "audio/audio_service.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "metadata/provider.hpp"
#include "profiles/model.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include <filesystem>
#include <memory>
#include <thread>

namespace inputnext::runtime_detail {

using worker_entry = void (*)(void*) noexcept;
using worker_launcher = std::thread (*)(void*, worker_entry, void*);

// Per-session owned dependencies supplied to the internal preparation seam. A launcher must
// either return the owned joinable worker or throw before creating a thread.
struct native_session_dependencies final {
    runtime_id identity{};
    std::filesystem::path runtime_library;
    std::unique_ptr<gameinput_detail::native_gameinput> native;
    std::unique_ptr<metadata_provider> metadata;
    std::unique_ptr<focus::source> focus_source;
    std::unique_ptr<audio_detail::native_port_factory> audio_factory;
    std::unique_ptr<audio_detail::audio_worker_launcher> audio_launcher;
    profiles::profile profile_settings;
    worker_launcher launch_worker{};
    void* launch_context{};
};

} // namespace inputnext::runtime_detail
