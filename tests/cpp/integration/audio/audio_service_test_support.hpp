#pragma once

#include "audio/audio_service.hpp"
#include "audio/controlled_audio_worker.hpp"
#include "audio/controlled_native_port.hpp"
#include "cpp/integration/gameinput/output/connection_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "output/render/control.hpp"

#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <inputnext/device_metadata.hpp>

namespace inputnext::audio_integration_test_support {

using namespace std::chrono_literals;

struct audio_service_fixture_options final {
    bool invalid_format{};
    std::optional<audio_detail::native_operation> fail_at = std::nullopt;
    bool closest_format{};
    bool block_initial_begin{};
    std::shared_ptr<test::audio::controlled_audio_worker_state> worker_state = nullptr;
    metadata_device_model initial_model{metadata_device_model::dualsense};
    test::audio::controlled_factory_creation_mode factory_creation_mode{
        test::audio::controlled_factory_creation_mode::normal};
    std::uint32_t factory_failure_code{1U};
};

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

template <typename Result>
[[nodiscard]] auto required_value(Result candidate, const char* message) {
    if (!candidate.has_value()) {
        throw std::runtime_error{message};
    }
    using value_type = std::remove_cvref_t<decltype(*candidate)>;
    return value_type{std::move(*candidate)};
}

class release_controlled_native_block final {
public:
    explicit release_controlled_native_block(
        std::shared_ptr<test::audio::controlled_native_port_state> state) noexcept
        : state_(std::move(state)) {}

    ~release_controlled_native_block() noexcept { release(); }

    release_controlled_native_block(const release_controlled_native_block&) = delete;
    release_controlled_native_block& operator=(const release_controlled_native_block&) = delete;
    release_controlled_native_block(release_controlled_native_block&&) = delete;
    release_controlled_native_block& operator=(release_controlled_native_block&&) = delete;

    void release() noexcept {
        if (active_) {
            state_->release_block();
            active_ = false;
        }
    }

private:
    std::shared_ptr<test::audio::controlled_native_port_state> state_;
    bool active_{true};
};

class audio_service_fixture final {
public:
    audio_service_fixture() : audio_service_fixture(audio_service_fixture_options{}) {}

    explicit audio_service_fixture(audio_service_fixture_options options)
        : worker_state(std::move(options.worker_state)) {
        gameinput_detail::test_support::configure_dualsense(graph, options.initial_model);
        if (options.initial_model == metadata_device_model::generic_gamepad) {
            graph.backing.native->device_facts.vendor_id.value = 0x045EU;
            graph.backing.native->device_facts.product_id.value = 0x02FDU;
        }
        if (!graph.start()) {
            throw std::runtime_error{"audio service fixture could not start its GameInput graph"};
        }
        if (!graph.wait_for_backend(graph.target)) {
            throw std::runtime_error{"audio service fixture backend did not become current"};
        }

        route_control = std::make_unique<output::detail::audio_route_control>(
            graph.composition->audio_routes());
        render_control =
            std::make_unique<output::detail::body_render_control>(*graph.output_executor);
        auto owned_factory = std::make_unique<test::audio::controlled_native_port_factory>();
        native_factory = owned_factory.get();
        native_factory->set_creation_mode(options.factory_creation_mode,
            options.factory_failure_code);
        const auto native_state = configure_native_port(0U);
        {
            std::scoped_lock lock{native_state->mutex};
            if (options.invalid_format) {
                native_state->mix_format.channel_mask = 0x0FU;
            }
            if (options.closest_format) {
                native_state->format_support = audio_detail::native_format_support_result{
                    .support = audio_detail::native_format_support::closest,
                    .closest_format = native_state->mix_format};
            }
        }
        if (options.block_initial_begin) {
            native_state->block_on(audio_detail::native_operation::begin_worker_sta);
        }
        if (options.fail_at.has_value()) {
            native_state->fail_on(*options.fail_at);
        }

        auto created = audio_detail::audio_service::create(audio_detail::audio_dependencies{
            .runtime = *graph.backing.runtime,
            .connections = *graph.connections,
            .route_control = *route_control,
            .render_control = *render_control,
            .native_factory = std::move(owned_factory),
            .worker_launcher = make_worker_launcher(),
        });
        if (!created.has_value()) {
            throw std::runtime_error{"audio service fixture could not create audio service"};
        }
        service = std::move(*created);
        service->synchronize(graph.backing.runtime->connections());
    }

    explicit audio_service_fixture(const bool invalid_format,
        const std::optional<audio_detail::native_operation> fail_at = std::nullopt,
        const bool closest_format = false,
        const bool block_initial_begin = false,
        std::shared_ptr<test::audio::controlled_audio_worker_state> worker_state = {})
        : audio_service_fixture(audio_service_fixture_options{.invalid_format = invalid_format,
              .fail_at = fail_at,
              .closest_format = closest_format,
              .block_initial_begin = block_initial_begin,
              .worker_state = std::move(worker_state)}) {}

    ~audio_service_fixture() noexcept {
        release_all_native_blocks();
        if (worker_state != nullptr) {
            worker_state->allow_all_joins();
        }
        if (service != nullptr) {
            service->shutdown();
            service.reset();
        }
    }

    audio_service_fixture(const audio_service_fixture&) = delete;
    audio_service_fixture& operator=(const audio_service_fixture&) = delete;
    audio_service_fixture(audio_service_fixture&&) = delete;
    audio_service_fixture& operator=(audio_service_fixture&&) = delete;

    [[nodiscard]] bool wait_for_live() { return wait_for_live(graph.target); }

    [[nodiscard]] bool wait_for_live(const connection_target target) {
        return wait_until([this, target] {
            graph.backing.maintain();
            const auto row = service->progress(target);
            return row.has_value() && row->phase == audio_detail::audio_target_phase::live &&
                   row->ready && row->audio.has_value();
        });
    }

    [[nodiscard]] bool wait_for_backend(const connection_target target) {
        return wait_until([this, target] {
            graph.backing.maintain();
            const auto current = graph.connections->query_backend_progress(target);
            return current.has_value() && current->phase == output::backend_phase::current;
        });
    }

    [[nodiscard]] bool synchronize_inventory(const connection_inventory& inventory) const {
        const auto updated = graph.connections->synchronize(inventory);
        if (!updated.has_value()) {
            return false;
        }
        service->synchronize(inventory);
        return true;
    }

    void set_dualsense_usb() {
        graph.backing.metadata->set_observation({.availability = metadata_availability::available,
                                                    .value = metadata_device_model::dualsense},
            {.availability = metadata_availability::available,
                .value = {.value = physical_transport::usb,
                    .provenance = transport_provenance::native_bus_type}},
            false);
        graph.backing.native->device_facts.vendor_id.value = 0x054CU;
        graph.backing.native->device_facts.product_id.value = 0x0CE6U;
        graph.backing.native->emit(graph.device,
            gameinput_detail::native_device_event::haptic_info_updated);
        graph.backing.maintain();
    }

    [[nodiscard]] std::string readiness_diagnostic() const {
        return readiness_diagnostic(graph.target);
    }

    [[nodiscard]] std::string readiness_diagnostic(const connection_target selected) const {
        std::ostringstream output;
        const auto snapshot = service->progress();
        output << "service accepting=" << snapshot.accepting << " stopping=" << snapshot.stopping
               << " desired=" << snapshot.desired_targets << " resident=" << snapshot.resident_slots
               << " live=" << snapshot.live_slots << " retained=" << snapshot.retained_slots;
        const auto target = service->progress(selected);
        if (target.has_value()) {
            output << "; target phase=" << static_cast<unsigned>(target->phase)
                   << " ready=" << target->ready << " transport=" << target->transport_open
                   << " backend=" << target->backend.has_value()
                   << " audio=" << target->audio.has_value()
                   << " setup=" << target->native_setup_started
                   << " start=" << target->native_stream_started;
            if (target->last_metadata_error.has_value()) {
                output << " metadata_error=" << static_cast<unsigned>(*target->last_metadata_error);
            }
            if (target->last_route_error.has_value()) {
                output << " route_error=" << static_cast<unsigned>(*target->last_route_error);
            }
            if (target->last_native_error.has_value()) {
                output << " native_error="
                       << static_cast<unsigned>(target->last_native_error->operation) << ':'
                       << target->last_native_error->native_code;
            }
        } else {
            output << "; target progress absent";
        }
        const auto control = graph.connections->progress();
        output << "; connections phase=" << static_cast<unsigned>(control.phase)
               << " backends=" << control.backend_obligations
               << " candidates=" << control.candidate_obligations
               << " retiring=" << control.pending_retirements;
        if (control.failure.has_value()) {
            output << " operation=" << static_cast<unsigned>(control.failure->operation);
            if (control.failure->metadata_failure.has_value()) {
                output << " metadata_error="
                       << static_cast<unsigned>(*control.failure->metadata_failure);
            }
            if (control.failure->runtime_failure.has_value()) {
                output << " runtime_error="
                       << static_cast<unsigned>(control.failure->runtime_failure->code);
            }
        }
        const auto backend = graph.connections->query_backend_progress(selected);
        if (backend.has_value()) {
            output << "; managed backend phase=" << static_cast<unsigned>(backend->phase)
                   << " valid=" << backend->backend.valid();
        } else {
            output << "; managed backend absent";
        }
        if (!native_states.empty() && native_states.front() != nullptr) {
            const auto& state = *native_states.front();
            const auto count = std::min<std::size_t>(state.call_count(), 24U);
            output << "; native calls=";
            for (std::size_t index = 0U; index < count; ++index) {
                const auto call = state.call_at(index);
                if (call.has_value()) {
                    output << static_cast<unsigned>(call->operation) << ',';
                }
            }
        }
        return output.str();
    }

    [[nodiscard]] bool wait_for_residents(const std::size_t expected) const {
        return wait_until(
            [this, expected] { return service->progress().resident_slots == expected; });
    }

    [[nodiscard]] std::shared_ptr<test::audio::controlled_native_port_state> configure_native_port(
        const std::size_t index,
        const std::u16string_view endpoint_text = u"old-endpoint",
        const native_guid container = gameinput_test_support::available_container().value) {
        auto state = native_factory->state_for(index);
        if (state == nullptr) {
            throw std::runtime_error{
                "audio service fixture could not allocate controlled port state"};
        }
        const auto endpoint = audio_detail::fixed_haptic_endpoint_id::create(endpoint_text);
        if (!endpoint.has_value()) {
            throw std::runtime_error{"audio service fixture endpoint ID was invalid"};
        }
        {
            std::scoped_lock lock{state->mutex};
            state->endpoint = audio_detail::native_endpoint_snapshot{
                .id = *endpoint,
                .state = audio_detail::native_endpoint_state::active,
                .flow = audio_detail::native_endpoint_flow::render,
                .container_id = container,
            };
        }
        native_states.push_back(state);
        return state;
    }

    void release_all_native_blocks() noexcept {
        for (const auto& state : native_states) {
            state->release_block();
        }
    }

    [[nodiscard]] output::authority& authority() const noexcept {
        return graph.composition->public_authority();
    }

    [[nodiscard]] output::audio_instance audio_instance(const connection_target target) const {
        const auto row = service->progress(target);
        if (!row.has_value() || !row->audio.has_value()) {
            throw std::runtime_error{"audio service has no current audio incarnation"};
        }
        return *row->audio;
    }

    [[nodiscard]] output::audio_instance audio_instance() const {
        return audio_instance(graph.target);
    }

    [[nodiscard]] bool wait_for_hd_ack(const output::audio_instance audio) const {
        return wait_for_hd_ack(graph.target, audio);
    }

    [[nodiscard]] bool wait_for_hd_ack(const connection_target target,
        const output::audio_instance audio) const {
        return wait_until([this, target, audio] {
            const auto current = graph.connections->query_backend_progress(target);
            return current.has_value() && current->acknowledged_body_route.has_value() &&
                   current->acknowledged_body_route->intent.identity.audio == audio;
        });
    }

    [[nodiscard]] std::unique_ptr<audio_detail::audio_worker_launcher> make_worker_launcher() {
        if (worker_state == nullptr) {
            return audio_detail::make_std_audio_worker_launcher();
        }
        return std::make_unique<test::audio::controlled_audio_worker_launcher>(worker_state);
    }

    gameinput_detail::test_support::connection_fixture graph;
    std::unique_ptr<output::detail::audio_route_control> route_control;
    std::unique_ptr<output::detail::body_render_control> render_control;
    std::unique_ptr<audio_detail::audio_service> service;
    test::audio::controlled_native_port_factory* native_factory{};
    std::vector<std::shared_ptr<test::audio::controlled_native_port_state>> native_states;
    std::shared_ptr<test::audio::controlled_audio_worker_state> worker_state;
};

} // namespace inputnext::audio_integration_test_support
