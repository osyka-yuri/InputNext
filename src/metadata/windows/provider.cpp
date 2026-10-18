#include "provider.hpp"

#include "metadata/policy.hpp"
#include "metadata/provider.hpp"
#include "os_port.hpp"
#include <inputnext/device_metadata.hpp>

#include <Windows.h>

#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace inputnext::metadata_detail {
namespace {

[[nodiscard]] metadata_field<native_guid> normalized_container(
    metadata_field<native_guid> field) noexcept {
    if (field.has_value() && field.value.is_zero()) {
        field.availability = metadata_availability::not_present;
        field.value = {};
    } else if (!field.has_value()) {
        field.value = {};
    }
    return field;
}

template <typename T>
[[nodiscard]] metadata_field<T> copy_field(const metadata_field<T>& field) {
    if (field.has_value()) {
        return field;
    }
    return {field.availability, {}, field.native_code};
}

[[nodiscard]] metadata_field<std::string> copy_pnp_path(const metadata_field<std::string>& field) {
    if (!field.has_value()) {
        return {.availability = field.availability, .value = {}, .native_code = field.native_code};
    }
    return copy_bounded_pnp_path(field.value, field.native_code);
}

[[nodiscard]] metadata_field<std::u16string> copy_haptic_endpoint_id(
    const metadata_field<std::u16string>& field) {
    if (!field.has_value()) {
        return {.availability = field.availability, .value = {}, .native_code = field.native_code};
    }
    return copy_bounded_utf16_content(field.value, haptic_endpoint_content_code_units);
}

[[nodiscard]] bool equal_ordinal_ignore_case(const std::u16string_view left,
    const std::u16string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto* const left_wide = reinterpret_cast<LPCWCH>(left.data());
    const auto* const right_wide = reinterpret_cast<LPCWCH>(right.data());
    const int length = static_cast<int>(left.size());
    const int comparison = CompareStringOrdinal(left_wide, length, right_wide, length, TRUE);
    return comparison == CSTR_EQUAL || (comparison == 0 && left == right);
}

struct endpoint_accumulator final {
    native_guid container{};
    const metadata_field<std::u16string>* haptic_id{};
    std::size_t distinct_count{};
    bool incomplete{};
    bool capacity_exceeded{};
    audio_endpoint_metadata selected{};
};

void collect_endpoint(void* const context, const audio_endpoint_observation& observation) {
    auto& accumulator = *static_cast<endpoint_accumulator*>(context);
    if (!observation.container_id.has_value()) {
        accumulator.incomplete = true;
        return;
    }
    if (observation.container_id.value != accumulator.container) {
        return;
    }
    if (observation.endpoint_id.availability == metadata_availability::capacity_exceeded) {
        accumulator.capacity_exceeded = true;
        return;
    }
    if (observation.endpoint_id.has_value() &&
        observation.endpoint_id.value.size() > headset_endpoint_content_code_units) {
        accumulator.capacity_exceeded = true;
        return;
    }
    if (!observation.endpoint_id.has_value() || observation.endpoint_id.value.empty() ||
        observation.endpoint_id.value.contains(u'\0')) {
        accumulator.incomplete = true;
        return;
    }
    if (accumulator.haptic_id != nullptr && accumulator.haptic_id->has_value() &&
        equal_ordinal_ignore_case(observation.endpoint_id.value, accumulator.haptic_id->value)) {
        return;
    }

    if (accumulator.distinct_count == 0U) {
        accumulator.selected.id = observation.endpoint_id;
        accumulator.selected.direct_sound_guid = observation.direct_sound_guid;
        accumulator.distinct_count = 1U;
        return;
    }
    if (equal_ordinal_ignore_case(accumulator.selected.id.value, observation.endpoint_id.value)) {
        if (!accumulator.selected.direct_sound_guid.has_value() &&
            observation.direct_sound_guid.has_value()) {
            accumulator.selected.direct_sound_guid = observation.direct_sound_guid;
        }
        return;
    }
    accumulator.distinct_count = 2U;
}

[[nodiscard]] audio_endpoint_metadata resolve_audio_flow(windows_metadata_os& os,
    const audio_flow flow,
    const native_guid& container,
    const metadata_field<std::u16string>& haptic_id) {
    endpoint_accumulator accumulator{};
    accumulator.container = container;
    accumulator.haptic_id = &haptic_id;
    const auto scan = os.scan_active_audio(flow, container, collect_endpoint, &accumulator);
    if (scan.availability == metadata_availability::capacity_exceeded) {
        return {.id = {.availability = metadata_availability::capacity_exceeded,
                    .value = {},
                    .native_code = scan.native_code}};
    }
    if (scan.availability != metadata_availability::available) {
        return {.id = {.availability = metadata_availability::unavailable,
                    .value = {},
                    .native_code = scan.native_code}};
    }
    if (accumulator.capacity_exceeded) {
        return {.id = {.availability = metadata_availability::capacity_exceeded,
                    .value = {},
                    .native_code = 0U}};
    }
    if (accumulator.incomplete) {
        return {.id = {.availability = metadata_availability::unavailable,
                    .value = {},
                    .native_code = 0U}};
    }
    if (accumulator.distinct_count == 0U) {
        return {.id = {.availability = metadata_availability::not_present,
                    .value = {},
                    .native_code = 0U}};
    }
    if (accumulator.distinct_count > 1U) {
        return {.id = {.availability = metadata_availability::ambiguous,
                    .value = {},
                    .native_code = 0U}};
    }
    return accumulator.selected;
}

[[nodiscard]] metadata_field<native_guid> resolve_container(
    const metadata_field<native_guid>& native_container,
    const metadata_field<native_guid>& pnp_container,
    container_id_provenance& provenance) noexcept {
    const bool has_native = native_container.has_value();
    const bool has_pnp = pnp_container.has_value();
    if (has_native && has_pnp) {
        if (native_container.value != pnp_container.value) {
            provenance = container_id_provenance::conflicting;
            return {.availability = metadata_availability::ambiguous,
                .value = {},
                .native_code = 0U};
        }
        provenance = container_id_provenance::gameinput_native;
        return native_container;
    }
    if (has_native) {
        provenance = container_id_provenance::gameinput_native;
        return native_container;
    }
    if (has_pnp) {
        provenance = container_id_provenance::exact_pnp_interface;
        return pnp_container;
    }
    provenance = container_id_provenance::unavailable;
    if (native_container.availability == metadata_availability::not_present &&
        pnp_container.availability == metadata_availability::not_present) {
        return {.availability = metadata_availability::not_present, .value = {}, .native_code = 0U};
    }
    if (native_container.availability == metadata_availability::capacity_exceeded ||
        pnp_container.availability == metadata_availability::capacity_exceeded) {
        return {.availability = metadata_availability::capacity_exceeded,
            .value = {},
            .native_code = 0U};
    }
    return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
}

class windows_metadata_provider final : public metadata_provider {
public:
    explicit windows_metadata_provider(std::unique_ptr<windows_metadata_os> owned_os)
        : owned_os_(std::move(owned_os)),
          os_(owned_os_.get()) {}

    explicit windows_metadata_provider(windows_metadata_os& os) noexcept : os_(&os) {}

    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const metadata_request& request) override {
        if (!request.target.valid()) {
            return std::unexpected{metadata_error::not_connected};
        }
        try {
            auto snapshot = make_base_snapshot(request);
            exact_pnp_observation pnp{};
            if (snapshot.native.pnp_path_utf8.has_value()) {
                pnp = os_->inspect_exact_pnp_interface(snapshot.native.pnp_path_utf8.value);
            }

            snapshot.container_id = resolve_container(snapshot.native.container_id,
                normalized_container(pnp.container_id),
                snapshot.container_id_source);
            snapshot.transport =
                pnp.transport.has_value()
                    ? pnp.transport
                    : infer_transport_from_pnp_path(snapshot.native.pnp_path_utf8.value);

            if (snapshot.container_id_source == container_id_provenance::conflicting) {
                snapshot.headset_render = {.id = {.availability = metadata_availability::ambiguous,
                                               .value = {},
                                               .native_code = 0U}};
                snapshot.headset_capture = {.id = {.availability = metadata_availability::ambiguous,
                                                .value = {},
                                                .native_code = 0U}};
                return snapshot;
            }
            if (snapshot.model.has_value() &&
                (snapshot.model.value == metadata_device_model::dualsense ||
                    snapshot.model.value == metadata_device_model::dualsense_edge)) {
                snapshot.headset_render = {
                    .id = {.availability = metadata_availability::not_present,
                        .value = {},
                        .native_code = 0U}};
                snapshot.headset_capture = {
                    .id = {.availability = metadata_availability::not_present,
                        .value = {},
                        .native_code = 0U}};
                return snapshot;
            }
            if (!snapshot.container_id.has_value()) {
                const auto unavailable =
                    snapshot.container_id.availability == metadata_availability::not_present
                        ? metadata_availability::not_present
                        : snapshot.container_id.availability;
                snapshot.headset_render = {.id = {.availability = unavailable,
                                               .value = {},
                                               .native_code = snapshot.container_id.native_code}};
                snapshot.headset_capture = {.id = {.availability = unavailable,
                                                .value = {},
                                                .native_code = snapshot.container_id.native_code}};
                return snapshot;
            }
            if (snapshot.native.haptic_endpoint_id.availability ==
                metadata_availability::capacity_exceeded) {
                snapshot.headset_render = {
                    .id = {.availability = metadata_availability::capacity_exceeded,
                        .value = {},
                        .native_code = 0U}};
                snapshot.headset_capture = {
                    .id = {.availability = metadata_availability::capacity_exceeded,
                        .value = {},
                        .native_code = 0U}};
                return snapshot;
            }

            snapshot.headset_render = resolve_audio_flow(*os_,
                audio_flow::render,
                snapshot.container_id.value,
                snapshot.native.haptic_endpoint_id);
            snapshot.headset_capture = resolve_audio_flow(*os_,
                audio_flow::capture,
                snapshot.container_id.value,
                snapshot.native.haptic_endpoint_id);
            return snapshot;
        } catch (const std::bad_alloc&) {
            return std::unexpected{metadata_error::resource_exhausted};
        }
    }

private:
    [[nodiscard]] static metadata_snapshot make_base_snapshot(const metadata_request& request) {
        metadata_snapshot snapshot{};
        snapshot.target = request.target;
        snapshot.catalogue_generation = request.catalogue_generation;
        snapshot.native.vendor_id = copy_field(request.facts.vendor_id);
        snapshot.native.product_id = copy_field(request.facts.product_id);
        snapshot.native.revision = copy_field(request.facts.revision);
        snapshot.native.gameinput_device_id = copy_field(request.facts.gameinput_device_id);
        snapshot.native.gameinput_root_id = copy_field(request.facts.gameinput_root_id);
        snapshot.native.container_id = normalized_container(request.facts.container_id);
        snapshot.native.pnp_path_utf8 = copy_pnp_path(request.facts.pnp_path_utf8);
        snapshot.native.gameinput_capabilities = copy_field(request.facts.gameinput_capabilities);
        snapshot.native.haptic_endpoint_id =
            copy_haptic_endpoint_id(request.facts.haptic_endpoint_id);
        snapshot.model = classify_device_model(snapshot.native);
        snapshot.capabilities = derive_hardware_capabilities(snapshot.native);
        return snapshot;
    }

    std::unique_ptr<windows_metadata_os> owned_os_;
    windows_metadata_os* os_;
};

} // namespace

std::unique_ptr<metadata_provider> make_windows_metadata_provider() {
    return std::make_unique<windows_metadata_provider>(make_system_windows_metadata_os());
}

std::unique_ptr<metadata_provider> make_windows_metadata_provider(windows_metadata_os& os) {
    return std::make_unique<windows_metadata_provider>(os);
}

} // namespace inputnext::metadata_detail
