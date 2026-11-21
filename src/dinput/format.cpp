#include "format.hpp"

#include "native_types.hpp"
#include "profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

namespace inputnext::dinput {
namespace {

constexpr DWORD optional_object_flag = 0x80000000U;
constexpr DWORD format_type_mask = 0x000000ffU;
constexpr DWORD format_instance_mask = DIDFT_INSTANCEMASK;
constexpr DWORD allowed_format_type_bits =
    format_type_mask | format_instance_mask | optional_object_flag | DIDFT_FFACTUATOR;
constexpr DWORD object_aspect_mask = static_cast<DWORD>(DIDOI_ASPECTMASK);

struct staged_binding final {
    DWORD data_offset{};
    DWORD profile_object_index{no_profile_object};
    object_kind kind{};
    GUID guid{};
    DWORD instance{};
    bool has_guid{};
    bool wildcard{};
    bool optional{};
    bool neutral_only{};
    bool requires_actuator{};
};

[[nodiscard]] bool matches_profile_object(const staged_binding& binding,
    const object_descriptor& descriptor) noexcept {
    return descriptor.kind == binding.kind &&
           (!binding.requires_actuator ||
               (descriptor.kind == object_kind::axis && descriptor.source_index < 2U)) &&
           (!binding.has_guid || binding.guid == descriptor.guid);
}

[[nodiscard]] DWORD object_width(object_kind kind) noexcept {
    switch (kind) {
    case object_kind::axis:
    case object_kind::pov:
        return static_cast<DWORD>(sizeof(DWORD));
    case object_kind::button:
        return static_cast<DWORD>(sizeof(BYTE));
    }
    return 0U;
}

[[nodiscard]] HRESULT decode_object_type(DWORD type, object_kind& kind) noexcept {
    const auto object_type = type & format_type_mask;
    if (object_type == DIDFT_RELAXIS) {
        return DIERR_UNSUPPORTED;
    }
    if (object_type == DIDFT_ABSAXIS || object_type == DIDFT_AXIS) {
        kind = object_kind::axis;
        return DI_OK;
    }
    if (object_type == DIDFT_BUTTON || object_type == DIDFT_PSHBUTTON ||
        object_type == DIDFT_TGLBUTTON) {
        kind = object_kind::button;
        return DI_OK;
    }
    if (object_type == DIDFT_POV) {
        kind = object_kind::pov;
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

[[nodiscard]] HRESULT validate_compiled_binding(const compiled_format& format,
    const data_binding& binding,
    std::span<const object_descriptor> profile) noexcept {
    const auto width = object_width(binding.kind);
    if (width == 0U || binding.data_offset > format.data_size ||
        width > format.data_size - binding.data_offset) {
        return DIERR_INVALIDPARAM;
    }
    if ((binding.kind == object_kind::axis || binding.kind == object_kind::pov) &&
        binding.data_offset % sizeof(DWORD) != 0U) {
        return DIERR_INVALIDPARAM;
    }
    if (binding.profile_object_index == no_profile_object) {
        return DI_OK;
    }

    if (binding.profile_object_index >= profile.size()) {
        return DIERR_INVALIDPARAM;
    }
    const auto& descriptor = profile[binding.profile_object_index];
    if (descriptor.kind != binding.kind) {
        return DIERR_INVALIDPARAM;
    }
    switch (binding.kind) {
    case object_kind::axis:
        return descriptor.source_index < axis_count ? DI_OK : DIERR_INVALIDPARAM;
    case object_kind::button:
        return descriptor.source_index < max_button_count ? DI_OK : DIERR_INVALIDPARAM;
    case object_kind::pov:
        return descriptor.source_index == 0U ? DI_OK : DIERR_INVALIDPARAM;
    }
    return DIERR_INVALIDPARAM;
}

[[nodiscard]] HRESULT validate_format_header(const DIDATAFORMAT& source) noexcept {
    if (source.dwSize != static_cast<DWORD>(sizeof(DIDATAFORMAT)) ||
        source.dwObjSize != static_cast<DWORD>(sizeof(DIOBJECTDATAFORMAT))) {
        return DIERR_INVALIDPARAM;
    }
    if ((source.dwFlags & static_cast<DWORD>(DIDF_RELAXIS)) != 0U) {
        return DIERR_UNSUPPORTED;
    }
    if (source.dwFlags != static_cast<DWORD>(DIDF_ABSAXIS)) {
        return DIERR_INVALIDPARAM;
    }
    if (source.dwDataSize == 0U || source.dwDataSize > max_format_data_size ||
        source.dwDataSize % sizeof(DWORD) != 0U) {
        return DIERR_INVALIDPARAM;
    }
    if (source.dwNumObjs > max_format_objects) {
        return DIERR_OUTOFMEMORY;
    }
    if (source.dwNumObjs != 0U && source.rgodf == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT validate_aspect(DWORD flags, bool optional, bool& neutral_only) noexcept {
    if ((flags & ~object_aspect_mask) != 0U) {
        return DIERR_UNSUPPORTED;
    }
    const auto aspect = flags & object_aspect_mask;
    if (aspect == 0U || aspect == static_cast<DWORD>(DIDOI_ASPECTPOSITION)) {
        return DI_OK;
    }
    const auto is_known_non_position = aspect == static_cast<DWORD>(DIDOI_ASPECTVELOCITY) ||
                                       aspect == static_cast<DWORD>(DIDOI_ASPECTACCEL) ||
                                       aspect == static_cast<DWORD>(DIDOI_ASPECTFORCE);
    if (!optional || !is_known_non_position) {
        return DIERR_UNSUPPORTED;
    }
    neutral_only = true;
    return DI_OK;
}

[[nodiscard]] HRESULT validate_object_storage(DWORD data_size,
    DWORD offset,
    DWORD width,
    object_kind kind,
    bool occupied_range,
    std::array<bool, max_format_data_size>& occupied) noexcept {
    if (offset > data_size || width > data_size - offset) {
        return DIERR_INVALIDPARAM;
    }
    const auto needs_word_alignment = kind == object_kind::axis || kind == object_kind::pov;
    if (needs_word_alignment && offset % sizeof(DWORD) != 0U) {
        return DIERR_INVALIDPARAM;
    }
    if (!occupied_range) {
        return DI_OK;
    }

    const auto begin = static_cast<std::size_t>(offset);
    const auto end = begin + static_cast<std::size_t>(width);
    for (auto byte = begin; byte < end; ++byte) {
        if (occupied[byte]) {
            return DIERR_INVALIDPARAM;
        }
    }
    for (auto byte = begin; byte < end; ++byte) {
        occupied[byte] = true;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT stage_object(const DIOBJECTDATAFORMAT& source,
    DWORD data_size,
    staged_binding& destination,
    std::array<bool, max_format_data_size>& occupied,
    bool force_feedback) noexcept {
    destination.data_offset = source.dwOfs;
    destination.optional = (source.dwType & optional_object_flag) != 0U;
    destination.requires_actuator = (source.dwType & DIDFT_FFACTUATOR) != 0U;
    if ((source.dwType & ~allowed_format_type_bits) != 0U) {
        return DIERR_UNSUPPORTED;
    }

    auto result = validate_aspect(source.dwFlags, destination.optional, destination.neutral_only);
    if (FAILED(result)) {
        return result;
    }
    if (destination.requires_actuator && !force_feedback) {
        if (!destination.optional) {
            return DIERR_UNSUPPORTED;
        }
        destination.neutral_only = true;
    }
    result = decode_object_type(source.dwType, destination.kind);
    if (FAILED(result)) {
        return result;
    }
    if ((source.dwType & format_type_mask) == DIDFT_TGLBUTTON) {
        if (!destination.optional) {
            return DIERR_UNSUPPORTED;
        }
        destination.neutral_only = true;
    }
    // DIJOYSTATE2 can alias optional non-position axis placeholders with position
    // fields. An unbound POV still writes its neutral value and must own its bytes.
    const bool reserves_storage = !destination.neutral_only || destination.requires_actuator ||
                                  destination.kind != object_kind::axis;
    result = validate_object_storage(data_size,
        destination.data_offset,
        object_width(destination.kind),
        destination.kind,
        reserves_storage,
        occupied);
    if (FAILED(result)) {
        return result;
    }

    destination.instance = (source.dwType & format_instance_mask) >> 8U;
    destination.wildcard = destination.instance == 0xffffU;
    destination.has_guid = source.pguid != nullptr;
    if (destination.has_guid) {
        destination.guid = *source.pguid;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT stage_objects(const DIDATAFORMAT& source,
    std::array<staged_binding, max_format_objects>& staged,
    bool force_feedback) noexcept {
    std::array<bool, max_format_data_size> occupied{};
    for (DWORD index = 0U; index < source.dwNumObjs; ++index) {
        const auto result = stage_object(source.rgodf[index],
            source.dwDataSize,
            staged[index],
            occupied,
            force_feedback);
        if (FAILED(result)) {
            return result;
        }
    }
    return DI_OK;
}

[[nodiscard]] std::optional<std::size_t> find_explicit_match(const staged_binding& entry,
    std::span<const object_descriptor> profile) noexcept {
    for (std::size_t index = 0U; index < profile.size(); ++index) {
        const auto& descriptor = profile[index];
        if (matches_profile_object(entry, descriptor) &&
            DIDFT_GETINSTANCE(descriptor.id) == entry.instance) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] HRESULT bind_explicit_instances(DWORD count,
    std::array<staged_binding, max_format_objects>& staged,
    std::span<const object_descriptor> profile,
    std::array<bool, max_object_count>& assigned) noexcept {
    for (DWORD index = 0U; index < count; ++index) {
        auto& entry = staged[index];
        if (entry.wildcard || entry.neutral_only) {
            continue;
        }
        const auto match = find_explicit_match(entry, profile);
        if (!match.has_value()) {
            if (!entry.optional) {
                return DIERR_UNSUPPORTED;
            }
            continue;
        }
        if (assigned[*match]) {
            return DIERR_INVALIDPARAM;
        }
        entry.profile_object_index = static_cast<DWORD>(*match);
        assigned[*match] = true;
    }
    return DI_OK;
}

struct wildcard_match final {
    std::optional<std::size_t> index;
    bool has_candidate{};
};

[[nodiscard]] wildcard_match find_wildcard_match(const staged_binding& entry,
    std::span<const object_descriptor> profile,
    const std::array<bool, max_object_count>& assigned) noexcept {
    for (std::size_t index = 0U; index < profile.size(); ++index) {
        if (!matches_profile_object(entry, profile[index])) {
            continue;
        }
        if (!assigned[index]) {
            return wildcard_match{.index = index, .has_candidate = true};
        }
    }
    const auto has_candidate = std::ranges::any_of(profile,
        [&](const auto& object) { return matches_profile_object(entry, object); });
    return wildcard_match{.index = std::nullopt, .has_candidate = has_candidate};
}

[[nodiscard]] HRESULT bind_wildcard_instances(DWORD count,
    std::array<staged_binding, max_format_objects>& staged,
    std::span<const object_descriptor> profile,
    std::array<bool, max_object_count>& assigned) noexcept {
    for (DWORD index = 0U; index < count; ++index) {
        auto& entry = staged[index];
        if (!entry.wildcard || entry.neutral_only) {
            continue;
        }
        const auto match = find_wildcard_match(entry, profile, assigned);
        if (!match.index.has_value()) {
            if (!entry.optional) {
                return match.has_candidate ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
            }
            continue;
        }
        entry.profile_object_index = static_cast<DWORD>(*match.index);
        assigned[*match.index] = true;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT bind_objects(DWORD count,
    std::array<staged_binding, max_format_objects>& staged,
    std::span<const object_descriptor> profile) noexcept {
    std::array<bool, max_object_count> assigned{};
    const auto explicit_result = bind_explicit_instances(count, staged, profile, assigned);
    if (FAILED(explicit_result)) {
        return explicit_result;
    }
    return bind_wildcard_instances(count, staged, profile, assigned);
}

} // namespace

HRESULT compile_format(const DIDATAFORMAT& source,
    compiled_format& destination,
    const device_model& model,
    const bool force_feedback) noexcept {
    const auto header_result = validate_format_header(source);
    if (FAILED(header_result)) {
        return header_result;
    }

    std::array<staged_binding, max_format_objects> staged{};
    auto result = stage_objects(source, staged, force_feedback);
    if (FAILED(result)) {
        return result;
    }
    result = bind_objects(source.dwNumObjs, staged, model.objects());
    if (FAILED(result)) {
        return result;
    }

    compiled_format compiled{};
    compiled.data_size = source.dwDataSize;
    for (DWORD index = 0U; index < source.dwNumObjs; ++index) {
        const auto& entry = staged[index];
        if (entry.profile_object_index == no_profile_object && entry.kind != object_kind::pov) {
            continue;
        }
        compiled.bindings[compiled.binding_count++] = data_binding{.data_offset = entry.data_offset,
            .profile_object_index = entry.profile_object_index,
            .kind = entry.kind};
    }
    destination = compiled;
    return DI_OK;
}

HRESULT write_state(const compiled_format& format,
    const profile_values& values,
    std::span<std::byte> destination,
    const device_model& model) noexcept {
    if (format.data_size == 0U || format.data_size > max_format_data_size ||
        format.binding_count > max_format_objects || destination.size() < format.data_size) {
        return DIERR_INVALIDPARAM;
    }
    const auto profile = model.objects();
    for (DWORD index = 0U; index < format.binding_count; ++index) {
        const auto result = validate_compiled_binding(format, format.bindings[index], profile);
        if (FAILED(result)) {
            return result;
        }
    }

    std::memset(destination.data(), 0, format.data_size);
    for (DWORD index = 0U; index < format.binding_count; ++index) {
        const auto& binding = format.bindings[index];
        const auto offset = static_cast<std::size_t>(binding.data_offset);
        if (binding.profile_object_index == no_profile_object) {
            if (binding.kind == object_kind::pov) {
                const DWORD neutral_pov = 0xffffffffU;
                std::memcpy(destination.data() + offset, &neutral_pov, sizeof(neutral_pov));
            }
            continue;
        }

        const auto& descriptor = profile[binding.profile_object_index];
        switch (binding.kind) {
        case object_kind::axis:
            std::memcpy(destination.data() + offset,
                &values.axes[descriptor.source_index],
                sizeof(values.axes[descriptor.source_index]));
            break;
        case object_kind::button:
            std::memcpy(destination.data() + offset,
                &values.buttons[descriptor.source_index],
                sizeof(values.buttons[descriptor.source_index]));
            break;
        case object_kind::pov:
            std::memcpy(destination.data() + offset, &values.pov, sizeof(values.pov));
            break;
        }
    }
    return DI_OK;
}

namespace {

[[nodiscard]] bool matches_object_id(const object_descriptor& descriptor, DWORD value) noexcept {
    const auto known_type_bits = format_type_mask | format_instance_mask | DIDFT_FFACTUATOR;
    if ((value & ~known_type_bits) != 0U ||
        DIDFT_GETINSTANCE(value) != DIDFT_GETINSTANCE(descriptor.id)) {
        return false;
    }
    if ((value & DIDFT_FFACTUATOR) != 0U &&
        (descriptor.kind != object_kind::axis || descriptor.source_index > 1U)) {
        return false;
    }
    const auto type = value & format_type_mask;
    switch (descriptor.kind) {
    case object_kind::axis:
        return type == DIDFT_ABSAXIS;
    case object_kind::button:
        return type == DIDFT_BUTTON || type == DIDFT_PSHBUTTON;
    case object_kind::pov:
        return type == DIDFT_POV;
    }
    return false;
}

} // namespace

// Win32 DIPH lookup order is object value followed by selector mode.
std::optional<std::size_t>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): preserve Win32 selector argument order.
find_object(DWORD value,
    DWORD how,
    const compiled_format* format,
    const device_model& model) noexcept {
    const auto profile = model.objects();
    if (how == DIPH_BYID) {
        for (std::size_t index = 0U; index < profile.size(); ++index) {
            if (matches_object_id(profile[index], value)) {
                return index;
            }
        }
        return std::nullopt;
    }
    if (how == DIPH_BYUSAGE) {
        for (std::size_t index = 0U; index < profile.size(); ++index) {
            const auto usage = DIMAKEUSAGEDWORD(profile[index].usage_page, profile[index].usage);
            if (usage == value) {
                return index;
            }
        }
        return std::nullopt;
    }
    if (how != DIPH_BYOFFSET || format == nullptr || format->binding_count > max_format_objects ||
        format->data_size == 0U || format->data_size > max_format_data_size) {
        return std::nullopt;
    }

    std::optional<std::size_t> result;
    for (DWORD index = 0U; index < format->binding_count; ++index) {
        const auto& binding = format->bindings[index];
        if (binding.data_offset != value || binding.profile_object_index == no_profile_object) {
            continue;
        }
        if (validate_compiled_binding(*format, binding, profile) != DI_OK || result.has_value()) {
            return std::nullopt;
        }
        result = binding.profile_object_index;
    }
    return result;
}

} // namespace inputnext::dinput
