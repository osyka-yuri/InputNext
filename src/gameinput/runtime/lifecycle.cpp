#include "state.hpp"

#include "factory.hpp"
#include "gameinput/core/publication_source.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "metadata/provider.hpp"
#include "metadata/windows/provider.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace inputnext {

namespace {

void include_deadline(std::optional<maintenance_time>& deadline,
    const std::optional<maintenance_time>& candidate) {
    if (candidate.has_value() && (!deadline.has_value() || *candidate < *deadline)) {
        deadline = candidate;
    }
}

[[nodiscard]] gameinput_failure map_failure(
    const gameinput_detail::native_failure& failure) noexcept {
    switch (failure.cause) {
    case gameinput_detail::native_failure_cause::load_failed:
        return gameinput_failure{.code = gameinput_error::load_failed,
            .native_code = failure.native_code};
    case gameinput_detail::native_failure_cause::api_unavailable:
        return gameinput_failure{.code = gameinput_error::api_unavailable,
            .native_code = failure.native_code};
    case gameinput_detail::native_failure_cause::registration_failed:
        return gameinput_failure{.code = gameinput_error::registration_failed,
            .native_code = failure.native_code};
    case gameinput_detail::native_failure_cause::resource_exhausted:
        return gameinput_failure{.code = gameinput_error::resource_exhausted,
            .native_code = failure.native_code};
    default:
        return gameinput_failure{.code = gameinput_error::native_failure,
            .native_code = failure.native_code};
    }
}

} // namespace

gameinput_runtime::implementation::implementation(device_registry& device_registry,
    gameinput_options runtime_options,
    std::unique_ptr<gameinput_detail::native_gameinput> native_port,
    std::unique_ptr<metadata_provider> metadata_port)
    : registry(device_registry),
      options(std::move(runtime_options)),
      native(std::move(native_port)),
      metadata(std::move(metadata_port)),
      history_provider_port(*this),
      history(history_provider_port) {
    watch_context =
        gameinput_detail::native_device_callback_context{.owner = this, .deliver = &device_event};
    rescan_context =
        gameinput_detail::native_device_callback_context{.owner = this, .deliver = &device_event};
    for (std::size_t index = 0; index < owners.size(); ++index) {
        owners[index].reading_owner_context =
            reading_owner{.runtime = this, .slot_index = index, .generation = 0U};
        owners[index].reading_context = gameinput_detail::native_reading_callback_context{
            .owner = &owners[index].reading_owner_context,
            .deliver = &reading_event,
        };
    }
}

[[nodiscard]] gameinput_result<void> gameinput_runtime::implementation::start(
    maintenance_time now) {
    std::scoped_lock service_lock{service_gate};
    {
        std::scoped_lock lock{table_gate};
        if (state != phase::created) {
            return std::unexpected(gameinput_failure{.code = gameinput_error::already_started});
        }
        state = phase::starting;
    }

    if (!options.runtime_library.is_absolute()) {
        std::scoped_lock lock{table_gate};
        state = phase::failed;
        return std::unexpected(gameinput_failure{.code = gameinput_error::invalid_argument});
    }

    auto initialized = native->initialize(options.runtime_library);
    if (!initialized.has_value()) {
        std::scoped_lock lock{table_gate};
        state = phase::failed;
        return std::unexpected(map_failure(initialized.error()));
    }

    auto registration = native->register_device_watch(watch_context);
    {
        std::scoped_lock lock{table_gate};
        if (registration.has_value() && registration->valid()) {
            watch_token = *registration;
            state = phase::running;
            next_maintenance = now;
            return {};
        }
        if (!registration.has_value() && registration.error().partial_token.valid()) {
            watch_token = registration.error().partial_token;
            watch_drain_retry_at = now + runtime_detail::retry_period;
        }
        state = phase::failed;
    }
    if (!registration.has_value()) {
        return std::unexpected(map_failure(registration.error()));
    }
    return std::unexpected(gameinput_failure{.code = gameinput_error::registration_failed});
}

void gameinput_runtime::implementation::maintain(maintenance_time now) {
    std::unique_lock service_lock{service_gate};
    {
        std::scoped_lock lock{table_gate};
        if (state != phase::running) {
            return;
        }
    }

    facts_attempted.fill(false);
    service_retirements(now);
    service_reading_drains(now);
    service_releasable_owners();
    service_admissions(now);
    service_readings(now);
    service_facts_refresh(now);
    service_rescan(now);
    const auto reclaimed = registry.drain_retired();
    static_cast<void>(reclaimed);

    {
        std::scoped_lock lock{table_gate};
        if (state == phase::running) {
            next_maintenance = now + runtime_detail::maintenance_period;
        }
    }
    service_lock.unlock();
    static_cast<void>(history.drain_retired());
}

std::optional<maintenance_time>
gameinput_runtime::implementation::next_rescan_deadline_locked() const {
    if (rescan_requested == rescan_acknowledged || rescan_exhausted ||
        rescan_registration_inflight) {
        return std::nullopt;
    }
    return rescan_token.valid() ? rescan_drain_retry_at : rescan_registration_retry_at;
}

std::optional<maintenance_time> gameinput_runtime::implementation::next_owner_deadline_locked(
    const owner_slot& owner,
    bool admission_capacity_available) {
    std::optional<maintenance_time> result;
    if (owner.state == owner_phase::observed && !owner.retire_requested &&
        admission_capacity_available) {
        include_deadline(result, owner.admission_retry_at);
    }
    if (owner.reading_token.valid() && !owner.reading_registration_inflight &&
        (owner.state == owner_phase::retiring || owner.retire_requested ||
            owner.registration_failed)) {
        include_deadline(result, owner.reading_drain_retry_at);
    }
    if (owner.state != owner_phase::active || owner.retire_requested) {
        return result;
    }
    if (!owner.reading_token.valid() && !owner.reading_registration_inflight &&
        !owner.reading_drain_retry_at.has_value()) {
        include_deadline(result, owner.registration_retry_at);
    }
    if (polling_required_locked(owner)) {
        include_deadline(result, owner.poll_at);
    }
    if (!owner.metadata_fault && owner.facts_requested != owner.facts_installed) {
        include_deadline(result, owner.facts_retry_at);
    }
    return result;
}

[[nodiscard]] std::optional<maintenance_time>
gameinput_runtime::implementation::next_deadline() const {
    std::scoped_lock lock{table_gate};
    if (state != phase::running) {
        return std::nullopt;
    }

    auto result = next_maintenance;
    const auto active_or_admitting =
        static_cast<std::size_t>(std::ranges::count_if(owners, [](const owner_slot& owner) {
            return owner.state == owner_phase::active || owner.state == owner_phase::admitting;
        }));
    include_deadline(result, next_rescan_deadline_locked());
    const bool admission_capacity_available =
        active_or_admitting < gameinput_live_connection_capacity;
    for (const auto& owner : owners) {
        include_deadline(result, next_owner_deadline_locked(owner, admission_capacity_available));
    }
    return result;
}

[[nodiscard]] connection_inventory gameinput_runtime::implementation::connections() const {
    connection_inventory result{};
    std::scoped_lock lock{table_gate};
    for (const auto& owner : owners) {
        if (!owner.target.valid() || owner.state == owner_phase::free ||
            result.size == result.entries.size()) {
            continue;
        }
        if (owner.state == owner_phase::retiring || owner.state == owner_phase::releasing) {
            continue;
        }

        auto health = connection_health::discovering;
        if (owner.source != nullptr) {
            const auto source_state = owner.source->status();
            if (source_state.closed) {
                health = connection_health::retirement_pending;
            } else if (source_state.fault != gameinput_detail::publication_fault::none) {
                health = connection_health::failed;
            } else if (owner.reading_token.valid() && !owner.registration_failed) {
                health = connection_health::callback_driven;
            } else {
                health = connection_health::polling_fallback;
            }
            result.entries[result.size++] = gameinput_connection{
                .target = owner.target,
                .reader = owner.reader,
                .health = health,
                .pad_known = source_state.pad_known,
                .guide_known = source_state.guide_known,
            };
        }
    }
    return result;
}

[[nodiscard]] gameinput_stop_report gameinput_runtime::implementation::stop(maintenance_time now) {
    {
        std::scoped_lock service_lock{service_gate};
        {
            std::scoped_lock lock{table_gate};
            if ((state == phase::created || state == phase::drained) &&
                (state == phase::drained || !output_enabled)) {
                state = phase::drained;
                return stop_status_locked(0U);
            }
            if (state != phase::stopping) {
                state = phase::stopping;
                output_admission_open = false;
                for (auto& owner : owners) {
                    if (owner.state != owner_phase::free) {
                        owner.retire_requested = true;
                    }
                }
            }
        }

        history.close_all();
        service_retirements(now);
        service_reading_drains(now);
        service_rescan_stop(now);
        service_watch_stop(now);
        service_releasable_owners();
        const auto reclaimed = registry.drain_retired();
        static_cast<void>(reclaimed);
    }

    static_cast<void>(history.drain_retired());
    const auto resident_history_banks = history.resident_bank_count();
    {
        std::scoped_lock service_lock{service_gate};
        service_releasable_owners();
        const auto re_reclaimed = registry.drain_retired();
        static_cast<void>(re_reclaimed);
    }

    bool wake_output_cleanup{};
    {
        std::scoped_lock lock{table_gate};
        const bool runtime_drained = resident_history_banks == 0U &&
                                     all_native_owners_drained_locked() && active_queries == 0U &&
                                     active_history_calls == 0U;
        if (runtime_drained && output_enabled && !native_cleanup_requested &&
            !native_cleanup_finished) {
            native_cleanup_requested = true;
            wake_output_cleanup = true;
        } else if (runtime_drained && (!output_enabled || native_cleanup_finished)) {
            state = phase::drained;
        }
        const auto report = stop_status_locked(resident_history_banks);
        if (wake_output_cleanup) {
            output_cleanup_wake.notify_all();
        }
        return report;
    }
}

void gameinput_runtime::implementation::shutdown() noexcept {
    for (;;) {
        const auto report = stop(std::chrono::steady_clock::now());
        if (report.drained) {
            break;
        }
        std::unique_lock lock{table_gate};
        query_drained.wait_for(lock, runtime_detail::retry_period);
    }
    if (output_cleanup_worker.joinable()) {
        output_cleanup_worker.join();
    }
}

void gameinput_runtime::implementation::request_rescan_locked() noexcept {
    if (rescan_requested == std::numeric_limits<std::uint64_t>::max()) {
        rescan_exhausted = true;
        return;
    }
    ++rescan_requested;
}

void gameinput_runtime::implementation::request_facts_locked(owner_slot& owner) noexcept {
    if (owner.metadata_fault) {
        return;
    }
    if (owner.facts_requested == std::numeric_limits<std::uint64_t>::max()) {
        owner.metadata_fault = true;
        return;
    }
    ++owner.facts_requested;
}

void gameinput_runtime::implementation::service_retirements(maintenance_time now) {
    (void)now;
    for (auto& owner : owners) {
        connection_target target{};
        std::uint64_t generation{};
        bool retire_registry{};
        {
            std::scoped_lock lock{table_gate};
            if (owner.state == owner_phase::free || !owner.retire_requested ||
                owner.registry_retire_started) {
                continue;
            }
            owner.state = owner_phase::retiring;
            owner.admission_retry_at.reset();
            owner.registration_retry_at.reset();
            owner.poll_at.reset();
            owner.facts_retry_at.reset();
            if (owner.source != nullptr) {
                owner.source->close();
            }
            owner.registry_retire_started = true;
            target = owner.target;
            generation = owner.generation;
            retire_registry = target.valid();
        }

        erase_catalogue(target, generation);
        if (retire_registry) {
            history.retire(target);
        }
        const bool registry_retired = !retire_registry || retire_registry_target(target);

        {
            std::scoped_lock lock{table_gate};
            if (owner.generation == generation && owner.state == owner_phase::retiring) {
                owner.registry_retire_started = registry_retired;
                owner.registry_retired = registry_retired;
            }
        }
    }
}

void gameinput_runtime::implementation::mark_retiring(std::size_t index,
    std::uint64_t generation) noexcept {
    std::scoped_lock lock{table_gate};
    if (index < owners.size() && owners[index].generation == generation &&
        owners[index].state != owner_phase::free) {
        auto& owner = owners[index];
        owner.retire_requested = true;
        owner.admission_retry_at.reset();
        owner.registration_retry_at.reset();
        owner.poll_at.reset();
        owner.facts_retry_at.reset();
    }
}

void gameinput_runtime::implementation::service_rescan(maintenance_time now) {
    gameinput_detail::native_callback_token existing{};
    bool due{};
    std::uint64_t request_generation{};
    {
        std::scoped_lock lock{table_gate};
        if (state != phase::running || rescan_exhausted ||
            rescan_requested == rescan_acknowledged) {
            return;
        }
        request_generation = rescan_requested;
        existing = rescan_token;
        due = !rescan_drain_retry_at.has_value() || *rescan_drain_retry_at <= now;
        if (!due || rescan_registration_inflight ||
            (rescan_registration_retry_at.has_value() && *rescan_registration_retry_at > now)) {
            return;
        }
        rescan_registration_inflight = true;
    }

    if (existing.valid()) {
        const bool drained = native->unregister_callback(existing);
        std::scoped_lock lock{table_gate};
        rescan_registration_inflight = false;
        if (drained) {
            rescan_token = {};
            rescan_drain_retry_at.reset();
            rescan_registration_retry_at.reset();
        } else {
            rescan_drain_retry_at = now + runtime_detail::retry_period;
        }
        return;
    }

    auto registered = native->register_device_rescan(rescan_context);
    {
        std::scoped_lock lock{table_gate};
        rescan_registration_inflight = false;
        if (registered.has_value() && registered->valid()) {
            rescan_token = *registered;
            rescan_acknowledged = request_generation;
            rescan_drain_retry_at.reset();
            rescan_registration_retry_at.reset();
        } else {
            if (!registered.has_value() && registered.error().partial_token.valid()) {
                rescan_token = registered.error().partial_token;
                rescan_drain_retry_at = now + runtime_detail::retry_period;
            } else {
                rescan_drain_retry_at.reset();
            }
            rescan_registration_retry_at = now + runtime_detail::retry_period;
        }
    }
}

void gameinput_runtime::implementation::service_rescan_stop(maintenance_time now) {
    (void)now;
    gameinput_detail::native_callback_token token{};
    {
        std::scoped_lock lock{table_gate};
        if (!rescan_token.valid() || rescan_registration_inflight ||
            (rescan_drain_retry_at.has_value() && *rescan_drain_retry_at > now)) {
            return;
        }
        rescan_registration_inflight = true;
        token = rescan_token;
    }
    const bool drained = native->unregister_callback(token);
    {
        std::scoped_lock lock{table_gate};
        rescan_registration_inflight = false;
        if (drained) {
            rescan_token = {};
            rescan_drain_retry_at.reset();
        } else {
            rescan_drain_retry_at = now + runtime_detail::retry_period;
        }
    }
}

void gameinput_runtime::implementation::service_watch_stop(maintenance_time now) {
    gameinput_detail::native_callback_token token{};
    {
        std::scoped_lock lock{table_gate};
        if (!watch_token.valid() || watch_registration_inflight ||
            (watch_drain_retry_at.has_value() && *watch_drain_retry_at > now)) {
            return;
        }
        watch_registration_inflight = true;
        token = watch_token;
    }
    const bool drained = native->unregister_callback(token);
    {
        std::scoped_lock lock{table_gate};
        watch_registration_inflight = false;
        if (drained) {
            watch_token = {};
            watch_drain_retry_at.reset();
        } else {
            watch_drain_retry_at = now + runtime_detail::retry_period;
        }
    }
}

void gameinput_runtime::implementation::service_releasable_owners() {
    if (output_enabled) {
        bool wake{};
        {
            std::scoped_lock lock{table_gate};
            for (auto& owner : owners) {
                if (owner.state != owner_phase::retiring || !owner.registry_retired ||
                    owner.release_started || owner.reading_token.valid() ||
                    owner.reading_registration_inflight || owner.history_leases != 0U ||
                    owner.output_leases != 0U) {
                    continue;
                }
                owner.state = owner_phase::releasing;
                owner.release_started = true;
                wake = true;
            }
        }
        if (wake) {
            notify_output_cleanup();
        }
        return;
    }

    for (std::size_t index = 0; index < owners.size(); ++index) {
        gameinput_detail::native_device_handle device{};
        read_pin pin{};
        std::uint64_t generation{};
        bool release{};
        {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.state != owner_phase::retiring || !owner.registry_retired ||
                owner.release_started || owner.reading_token.valid() ||
                owner.reading_registration_inflight || owner.history_leases != 0U) {
                continue;
            }
            owner.state = owner_phase::releasing;
            owner.release_started = true;
            device = owner.device;
            pin = std::move(owner.residency);
            generation = owner.generation;
            release = true;
        }
        if (!release) {
            continue;
        }

        if (device.valid()) {
            native->release(device);
        }
        pin.release();
        const auto reclaimed = registry.drain_retired();
        static_cast<void>(reclaimed);
        {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.generation == generation && owner.state == owner_phase::releasing) {
                owner = owner_slot{};
                owner.reading_owner_context =
                    reading_owner{.runtime = this, .slot_index = index, .generation = 0U};
                owner.reading_context = gameinput_detail::native_reading_callback_context{
                    .owner = &owner.reading_owner_context,
                    .deliver = &reading_event,
                };
                request_rescan_locked();
            }
        }
    }
}

[[nodiscard]] bool
gameinput_runtime::implementation::all_native_owners_drained_locked() const noexcept {
    return !watch_token.valid() && !rescan_token.valid() && !watch_registration_inflight &&
           !rescan_registration_inflight &&
           std::ranges::all_of(owners, [](const owner_slot& owner) {
               return owner.state == owner_phase::free && !owner.reading_token.valid() &&
                      !owner.reading_registration_inflight && owner.history_leases == 0U &&
                      owner.output_leases == 0U;
           });
}

[[nodiscard]] gameinput_stop_report gameinput_runtime::implementation::stop_status_locked(
    std::size_t resident_history_banks) const noexcept {
    std::size_t callback_owners{};
    if (watch_token.valid() || watch_registration_inflight) {
        ++callback_owners;
    }
    if (rescan_token.valid() || rescan_registration_inflight) {
        ++callback_owners;
    }
    for (const auto& owner : owners) {
        if (owner.reading_token.valid() || owner.reading_registration_inflight) {
            ++callback_owners;
        }
    }
    const auto pending_history_owners = static_cast<std::size_t>(std::ranges::count_if(owners,
        [](const owner_slot& owner) { return owner.history_leases != 0U; }));
    std::size_t pending_output_leases{};
    for (const auto& owner : owners) {
        pending_output_leases += owner.output_leases;
    }
    const auto pending_native_cleanup_owners =
        static_cast<std::size_t>(std::ranges::count_if(owners,
            [](const owner_slot& owner) { return owner.state == owner_phase::releasing; }));
    return gameinput_stop_report{
        .drained = state == phase::drained && active_queries == 0U && active_history_calls == 0U &&
                   pending_history_owners == 0U && resident_history_banks == 0U,
        .pending_callback_owners = callback_owners,
        .active_metadata_queries = active_queries,
        .active_history_calls = active_history_calls,
        .pending_history_owners = pending_history_owners,
        .pending_output_leases = pending_output_leases,
        .pending_native_cleanup_owners = pending_native_cleanup_owners,
        .native_api_cleanup_complete = !output_enabled || native_cleanup_finished,
    };
}

gameinput_runtime::gameinput_runtime(device_registry& registry, gameinput_options options)
    : implementation_(std::make_unique<implementation>(registry,
          std::move(options),
          gameinput_detail::make_native_gameinput(),
          metadata_detail::make_windows_metadata_provider())) {
}

gameinput_runtime::gameinput_runtime(std::unique_ptr<implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {
}

gameinput_runtime::~gameinput_runtime() noexcept {
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

gameinput_result<void> gameinput_runtime::start(maintenance_time now) {
    return implementation_->start(now);
}

void gameinput_runtime::maintain(maintenance_time now) {
    implementation_->maintain(now);
}

std::optional<maintenance_time> gameinput_runtime::next_deadline() const {
    return implementation_->next_deadline();
}

connection_inventory gameinput_runtime::connections() const {
    return implementation_->connections();
}

metadata_result<metadata_snapshot> gameinput_runtime::query_metadata(connection_target target) {
    return implementation_->query_metadata(target);
}

history_result<history_reader_handle> gameinput_runtime::open_history(connection_target target,
    history_time now) {
    return implementation_->open_history(target, now);
}

history_result<history_reader_handle> gameinput_runtime::open_xinput_history(std::uint32_t position,
    connection_target target,
    history_time now) {
    return implementation_->open_history(target, now, position);
}

history_batch gameinput_runtime::read_history(history_reader_handle reader, history_time now) {
    return implementation_->read_history(reader, now);
}

history_result<void> gameinput_runtime::close_history(history_reader_handle reader) {
    return implementation_->close_history(reader);
}

gameinput_stop_report gameinput_runtime::stop(maintenance_time now) {
    return implementation_->stop(now);
}

std::unique_ptr<gameinput_runtime> gameinput_detail::runtime_factory::create(
    device_registry& registry,
    gameinput_options options,
    std::unique_ptr<gameinput_detail::native_gameinput> native,
    std::unique_ptr<metadata_provider> metadata) {
    auto implementation = std::make_unique<gameinput_runtime::implementation>(registry,
        std::move(options),
        std::move(native),
        std::move(metadata));
    return std::unique_ptr<gameinput_runtime>{new gameinput_runtime{std::move(implementation)}};
}

} // namespace inputnext
