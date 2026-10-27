#pragma once

#include "core/core.hpp"
#include "cpp/integration/runtime/session/native_session_test_support.hpp"
#include "effects/effects.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output_submission.hpp>
#include <inputnext/xinput_types.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::effects_integration_test_support {

using namespace std::chrono_literals;
using gameinput_test_support::native_history_script;

class session_fixture final {
public:
    explicit session_fixture(effects::identity_source* const identities = nullptr,
        std::unique_ptr<audio_detail::native_port_factory> audio_factory = {},
        std::unique_ptr<metadata_provider> metadata = {})
        : runtime(nullptr, nullptr, identities, std::move(audio_factory), std::move(metadata)) {}

    session_fixture(const session_fixture&) = delete;
    session_fixture& operator=(const session_fixture&) = delete;
    session_fixture(session_fixture&&) = delete;
    session_fixture& operator=(session_fixture&&) = delete;
    ~session_fixture() = default;

    void start_runtime() {
        history = std::make_shared<native_history_script>();
        runtime.configure_device(history);
        const auto started = runtime.start();
        if (!started.has_value()) {
            throw std::runtime_error{"Effects integration GameInput session failed to start"};
        }
        runtime.session->activate();
    }

    void start_core_only() {
        start_runtime();
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto catalogue = runtime.session->core().enumerate();
            if (catalogue.has_value() && catalogue->count != 0U) {
                target = catalogue->entries[0U].target;
                return;
            }
            std::this_thread::sleep_for(2ms);
        }
        throw std::runtime_error{"Effects integration target was not enumerated"};
    }

    void start() {
        start_core_only();
        if (!runtime.wait_for_button(XINPUT_GAMEPAD_A)) {
            throw std::runtime_error{"Effects integration foreground input did not become ready"};
        }
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto resolved = runtime.session->effects().resolve(selector());
            if (resolved.has_value()) {
                return;
            }
            std::this_thread::sleep_for(2ms);
        }
        throw std::runtime_error{"Effects integration target did not acquire an output backend"};
    }

    [[nodiscard]] effects::service& service() const noexcept { return runtime.session->effects(); }

    [[nodiscard]] core::selector selector() const noexcept {
        return core::selector{.kind = core::selector_kind::exact_target, .target = target};
    }

    [[nodiscard]] static output::finite_submission_window window(
        const std::chrono::milliseconds ttl = 2s) {
        const auto created =
            output::make_finite_submission_window(std::chrono::steady_clock::now(), ttl);
        if (!created.has_value()) {
            throw std::runtime_error{
                "Effects integration fixture could not create a finite window"};
        }
        return *created;
    }

    [[nodiscard]] effects::context_handle open() const {
        const auto opened = service().open(target);
        if (!opened.has_value()) {
            throw std::runtime_error{"Effects integration fixture could not open a context"};
        }
        return *opened;
    }

    runtime_session_test_support::session_fixture runtime;
    std::shared_ptr<native_history_script> history;
    connection_target target{};
};

} // namespace inputnext::effects_integration_test_support
