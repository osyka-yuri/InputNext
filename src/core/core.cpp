#include "core.hpp"

#include "core_state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/input_binding.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace inputnext::core {

service::implementation::implementation(const runtime_id runtime_identity,
    const std::uint64_t frequency,
    device_registry& registry_owner,
    gameinput_runtime& gameinput_owner,
    focus::observer& focus_owner,
    xinput::service& presentation_owner) noexcept
    : identity(runtime_identity),
      qpc_frequency(frequency),
      registry(registry_owner),
      runtime(gameinput_owner),
      focus_observer(focus_owner),
      presentation(presentation_owner) {
}

service::service(const runtime_id identity,
    const std::uint64_t qpc_frequency,
    device_registry& registry,
    gameinput_runtime& runtime,
    focus::observer& focus,
    xinput::service& presentation) {
    if (!identity.valid() || qpc_frequency == 0U) {
        throw std::invalid_argument{"core service requires a runtime identity and QPC frequency"};
    }
    implementation_ = std::make_unique<implementation>(identity,
        qpc_frequency,
        registry,
        runtime,
        focus,
        presentation);
}

service::~service() noexcept = default;

runtime_info service::info() const noexcept {
    return runtime_info{.runtime = implementation_->identity,
        .qpc_frequency = implementation_->qpc_frequency};
}

result<device_catalogue> service::enumerate() {
    return implementation_->enumerate();
}

result<connection_target> service::resolve(const selector& selection) {
    return implementation_->resolve(selection);
}

result<connection_snapshot> service::read_latest(const connection_target target) const {
    return implementation_->read_latest(target);
}

result<reader_handle> service::open_reader(const connection_target target,
    const reader_focus focus) {
    return implementation_->open_reader(target, focus);
}

reader_result service::read_events(const reader_handle reader, const std::size_t capacity) {
    return implementation_->read_events(reader, capacity);
}

status service::close_reader(const reader_handle reader) {
    return implementation_->close_reader(reader);
}

void service::close_admission() noexcept {
    if (implementation_ != nullptr) {
        implementation_->close_admission();
    }
}

void service::implementation::close_admission() noexcept {
    admission_closed.store(true, std::memory_order_release);
}

} // namespace inputnext::core
