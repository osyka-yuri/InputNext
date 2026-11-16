#include "support.hpp"

#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/service.hpp"
#include "dinput/profile.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/window_acquisition.hpp"

#include <catch2/catch_approx.hpp>

#include <inputnext/output_executor.hpp>
#include <inputnext/source_lifetime.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::dinput::feedback_test_support {

using namespace inputnext;
using namespace inputnext::dinput::force_feedback;
using namespace std::chrono_literals;

[[nodiscard]] effect_parameters constant(const LONG magnitude) {
    std::array<DWORD, 1U> axes{dinput::objects()[0U].id | DIDFT_FFACTUATOR};
    std::array<LONG, 1U> direction{1};
    DICONSTANTFORCE force{magnitude};
    DIEFFECT description{};
    description.dwSize = sizeof(description);
    description.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    description.dwDuration = INFINITE;
    description.dwGain = DI_FFNOMINALMAX;
    description.dwTriggerButton = DIEB_NOTRIGGER;
    description.cAxes = 1U;
    description.rgdwAxes = axes.data();
    description.rglDirection = direction.data();
    description.cbTypeSpecificParams = sizeof(force);
    description.lpvTypeSpecificParams = &force;
    effect_parameters result{};
    const auto status = set_parameters(result,
        *find_effect(GUID_ConstantForce),
        &description,
        DIEP_ALLPARAMS,
        nullptr);
    if (FAILED(status)) {
        throw std::runtime_error{"invalid constant effect test parameters"};
    }
    return result;
}

feedback_fixture::feedback_fixture()
    : registry(runtime),
      trusted(runtime, registry, focus.observer()) {
    std::unique_ptr<source_lifetime> source = std::make_unique<test::output::registry_source>();
    const auto connected = registry.admit(source);
    if (!connected.has_value()) {
        throw std::runtime_error{"feedback fixture connection admission failed"};
    }
    target = connected->target;
    focus.set_window(101U, 2U);
    if (!focus.wait_for_window(101U, 2U)) {
        throw std::runtime_error{"feedback fixture window unavailable"};
    }
    auto created = output::executor::create(trusted.public_authority());
    if (!created.has_value()) {
        throw std::runtime_error{"feedback fixture executor creation failed"};
    }
    executor = std::move(*created);
    const auto execution =
        output::detail::authority_test_access::attached_execution(trusted.public_authority());
    if (!execution.has_value()) {
        throw std::runtime_error{"feedback fixture has no attached executor"};
    }
    backend_owner = *execution;
    feedback = std::make_unique<service>(trusted, *executor, this, &query_motors);
}

feedback_fixture::~feedback_fixture() {
    release_query.store(true, std::memory_order_release);
    feedback->shutdown();
    executor->shutdown();
}

[[nodiscard]] eligibility feedback_fixture::acquisition() {
    const auto guard = focus.observer().lock_presentation();
    const auto epoch = guard.current_foreground_window_epoch(101U);
    if (!epoch.has_value()) {
        throw std::runtime_error{"feedback fixture has no foreground epoch"};
    }
    return {.status = DI_OK,
        .token = std::make_shared<output::detail::window_acquisition>(target, *epoch)};
}

[[nodiscard]] std::uint64_t feedback_fixture::create(const void* device,
    const LONG magnitude) const {
    std::uint64_t id{};
    if (FAILED(
            feedback->create(device, *find_effect(GUID_ConstantForce), constant(magnitude), id))) {
        throw std::runtime_error{"feedback fixture effect creation failed"};
    }
    return id;
}

[[nodiscard]] bool feedback_fixture::wait_for_amplitude(const float value) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = trusted.public_authority().read_snapshot(target);
        if (state.has_value() && state->rumble.has_value() &&
            state->rumble->value.low_frequency == Catch::Approx(value)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

[[nodiscard]] bool feedback_fixture::wait_for_stopped(const std::uint64_t id) const {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD state{};
        if (SUCCEEDED(feedback->status(id, &state)) && (state & DIEGES_PLAYING) == 0U) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

[[nodiscard]] bool feedback_fixture::pause_next_query() noexcept {
    pause_query.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (query_entered.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

[[nodiscard]] motors feedback_fixture::query_motors(void* context,
    const connection_target requested) noexcept {
    auto& fixture = *static_cast<feedback_fixture*>(context);
    const bool available =
        fixture.available.load(std::memory_order_acquire) && requested == fixture.target;
    const auto sample =
        available ? motors{.low = fixture.low.load(std::memory_order_acquire),
                        .high = fixture.high.load(std::memory_order_acquire),
                        .backend = {.target = requested,
                            .incarnation = {.owner = fixture.backend_owner,
                                .id = {fixture.backend_id.load(std::memory_order_acquire)}}}}
                  : motors{};
    const auto paused_thread = fixture.paused_query_thread.load(std::memory_order_acquire);
    if (fixture.pause_query.load(std::memory_order_acquire) &&
        (paused_thread == 0U || paused_thread == GetCurrentThreadId()) &&
        !fixture.query_entered.exchange(true, std::memory_order_acq_rel)) {
        while (!fixture.release_query.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return sample;
}

} // namespace inputnext::dinput::feedback_test_support
