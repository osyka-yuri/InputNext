#include "audio_service.hpp"

#include "service/detail.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>

namespace inputnext::audio_detail {
namespace {

class std_audio_worker final : public audio_worker {
public:
    std_audio_worker(const audio_worker_launcher::entry_point entry, void* const context)
        : thread_([entry, context] { entry(context); }) {}

    [[nodiscard]] bool joinable() const noexcept override { return thread_.joinable(); }

    [[nodiscard]] bool join() noexcept override {
        if (!thread_.joinable()) {
            return true;
        }
        try {
            thread_.join();
            return true;
        } catch (const std::system_error&) {
            return false;
        } catch (...) {
            return false;
        }
    }

private:
    std::thread thread_;
};

class std_audio_worker_launcher final : public audio_worker_launcher {
public:
    [[nodiscard]] std::unique_ptr<audio_worker>
    launch(const audio_worker_launcher::entry_point entry, void* const context) noexcept override {
        if (entry == nullptr || context == nullptr) {
            return {};
        }
        try {
            return std::make_unique<std_audio_worker>(entry, context);
        } catch (...) {
            return {};
        }
    }
};

} // namespace

std::unique_ptr<audio_worker_launcher> make_std_audio_worker_launcher() noexcept {
    return std::unique_ptr<audio_worker_launcher>{new (std::nothrow) std_audio_worker_launcher{}};
}

audio_service::audio_service(std::unique_ptr<implementation> state) noexcept
    : implementation_(std::move(state)) {
}

std::expected<std::unique_ptr<audio_service>, audio_service_error> audio_service::create(
    audio_dependencies dependencies) {
    if (dependencies.native_factory == nullptr || dependencies.worker_launcher == nullptr) {
        return std::unexpected{audio_service_error::resource_exhausted};
    }

    auto state =
        std::unique_ptr<implementation>{new (std::nothrow) implementation{std::move(dependencies)}};
    if (state == nullptr) {
        return std::unexpected{audio_service_error::resource_exhausted};
    }

    auto service =
        std::unique_ptr<audio_service>{new (std::nothrow) audio_service{std::move(state)}};
    if (service == nullptr) {
        return std::unexpected{audio_service_error::resource_exhausted};
    }

    auto* const service_state = service->implementation_.get();
    try {
        service_state->control_worker =
            std::thread{[service_state] { service_state->run_control(); }};
    } catch (...) {
        return std::unexpected{audio_service_error::worker_start_failed};
    }
    return service;
}

audio_service::~audio_service() noexcept {
    shutdown();
}

void audio_service::synchronize(const connection_inventory& inventory) noexcept {
    if (implementation_ == nullptr || !implementation_->accepting.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::scoped_lock lock{implementation_->publication_mutex};
        implementation_->latest_inventory = inventory;
        implementation_->latest_inventory.size = std::min(inventory.size, inventory.entries.size());
    }
    implementation_->control_wakeup.notify();
}

void audio_service::close_admission() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    implementation_->accepting.store(false, std::memory_order_seq_cst);
    implementation_->control_wakeup.notify();
}

void audio_service::request_stop() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    implementation_->accepting.store(false, std::memory_order_seq_cst);
    implementation_->stopping.store(true, std::memory_order_release);
    implementation_->control_wakeup.notify();
}

audio_service_progress audio_service::progress() const noexcept {
    if (implementation_ == nullptr) {
        return {};
    }
    std::scoped_lock lock{implementation_->publication_mutex};
    return implementation_->published_progress;
}

std::optional<audio_target_progress> audio_service::progress(
    const connection_target target) const noexcept {
    if (implementation_ == nullptr) {
        return std::nullopt;
    }
    std::scoped_lock lock{implementation_->publication_mutex};
    const auto& snapshot = implementation_->published_progress;
    const auto end = snapshot.targets.begin() + static_cast<std::ptrdiff_t>(snapshot.target_count);
    const auto found = std::ranges::find_if(snapshot.targets.begin(),
        end,
        [target](const audio_target_progress& value) { return value.target == target; });
    if (found != end) {
        return *found;
    }
    const auto failures_end =
        snapshot.start_failures.begin() + static_cast<std::ptrdiff_t>(snapshot.start_failure_count);
    const auto failure = std::ranges::find_if(snapshot.start_failures.begin(),
        failures_end,
        [target](const audio_start_failure& value) { return value.target == target; });
    if (failure == failures_end) {
        return std::nullopt;
    }
    return audio_target_progress{.target = target,
        .backend = failure->backend,
        .phase = audio_target_phase::waiting_for_retry,
        .last_native_operation = failure->error.operation,
        .last_native_error = failure->error};
}

void audio_service::shutdown() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    request_stop();
    if (!implementation_->control_worker.joinable()) {
        return;
    }
    while (implementation_->control_worker.joinable()) {
        try {
            implementation_->control_worker.join();
        } catch (...) {
            // Keep borrowed owners in scope until the control and native workers are proven joined.
            std::this_thread::yield();
        }
    }
}

} // namespace inputnext::audio_detail
