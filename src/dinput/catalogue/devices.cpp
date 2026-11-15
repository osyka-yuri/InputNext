#include "devices.hpp"

#include "core/core.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/device_info.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/property_abi.h"
#include "identity.hpp"
#include "inputnext/device_identity.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace inputnext::dinput {
namespace {

struct system_device final {
    DIDEVICEINSTANCEW instance{};
    std::optional<DIPROPGUIDANDPATH> property;
    std::wstring path;
};

struct gameinput_device final {
    core::device_record record{};
    std::wstring path;
    std::size_t system_index{std::numeric_limits<std::size_t>::max()};
    GUID generated_instance{};
};

struct system_capture final {
    std::vector<DIDEVICEINSTANCEW>* instances{};
    HRESULT failure{S_OK};
};

[[nodiscard]] HRESULT map_core_status(const core::status status) noexcept {
    switch (status) {
    case core::status::capacity_exceeded:
        return DIERR_OUTOFMEMORY;
    case core::status::stale_target:
    case core::status::source_retired:
    case core::status::not_admitted:
    case core::status::unavailable:
        return DIERR_INPUTLOST;
    case core::status::busy:
    case core::status::ambiguous_target:
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    case core::status::invalid_argument:
        return DIERR_INVALIDPARAM;
    case core::status::ok:
    case core::status::buffer_too_small:
    case core::status::stale_reader:
    case core::status::io_failed:
    case core::status::no_data:
        return DIERR_GENERIC;
    }
    return DIERR_GENERIC;
}

[[nodiscard]] std::size_t bounded_length(const wchar_t* value,
    const std::size_t capacity) noexcept {
    for (std::size_t index = 0U; index < capacity; ++index) {
        if (value[index] == L'\0') {
            return index;
        }
    }
    return capacity;
}

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(const std::string& value) {
    if (value.empty() || value.size() > max_pnp_path_utf8_bytes ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            input_size,
            result.data(),
            required) != required) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool same_path(const std::wstring& left, const std::wstring& right) noexcept {
    if (left.empty() || right.empty() ||
        left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] BOOL CALLBACK capture_system_instance(LPCDIDEVICEINSTANCEW instance,
    LPVOID context) noexcept {
    auto& capture = *static_cast<system_capture*>(context);
    if (instance == nullptr || capture.instances == nullptr) {
        capture.failure = DIERR_INVALIDPARAM;
        return DIENUM_STOP;
    }
    try {
        capture.instances->push_back(*instance);
        return DIENUM_CONTINUE;
    } catch (const std::bad_alloc&) {
        capture.failure = DIERR_OUTOFMEMORY;
    } catch (...) {
        capture.failure = DIERR_GENERIC;
    }
    return DIENUM_STOP;
}

[[nodiscard]] std::vector<system_device> read_system_devices(IDirectInput8W& system) {
    std::vector<DIDEVICEINSTANCEW> instances;
    system_capture capture{.instances = &instances};
    constexpr DWORD flags =
        DIEDFL_ALLDEVICES | DIEDFL_INCLUDEALIASES | DIEDFL_INCLUDEPHANTOMS | DIEDFL_INCLUDEHIDDEN;
    const auto status =
        system.EnumDevices(DI8DEVCLASS_GAMECTRL, &capture_system_instance, &capture, flags);

    std::vector<system_device> result;
    if (FAILED(capture.failure) || FAILED(status)) {
        return result;
    }
    result.reserve(instances.size());
    const auto release_device = [](IDirectInputDevice8W* device) noexcept {
        device->Release();
    };
    for (const auto& instance : instances) {
        system_device entry{.instance = instance, .property = std::nullopt, .path = {}};
        IDirectInputDevice8W* device{};
        const auto created = system.CreateDevice(instance.guidInstance, &device, nullptr);
        const std::unique_ptr<IDirectInputDevice8W, decltype(release_device)> owned{device,
            release_device};
        if (SUCCEEDED(created) && owned != nullptr) {
            DIPROPGUIDANDPATH property{};
            property.diph.dwSize = static_cast<DWORD>(sizeof(property));
            property.diph.dwHeaderSize = static_cast<DWORD>(sizeof(property.diph));
            property.diph.dwObj = 0U;
            property.diph.dwHow = DIPH_DEVICE;
            if (SUCCEEDED(
                    inputnext_dinput_get_property_w(device, &DIPROP_GUIDANDPATH, &property.diph))) {
                const auto length = bounded_length(property.wszPath, std::size(property.wszPath));
                if (length > 0U && length < std::size(property.wszPath)) {
                    entry.path.assign(property.wszPath, length);
                    entry.property = property;
                }
            }
        }
        result.push_back(std::move(entry));
    }
    return result;
}

[[nodiscard]] bool guid_appears_once(const GUID& guid,
    const std::vector<system_device>& devices) noexcept {
    std::size_t count{};
    for (const auto& device : devices) {
        if (IsEqualGUID(device.instance.guidInstance, guid) != 0) {
            ++count;
        }
    }
    return count == 1U;
}

[[nodiscard]] bool has_guid(const std::vector<GUID>& guids, const GUID& guid) noexcept {
    return std::ranges::any_of(guids,
        [&guid](const GUID& item) { return IsEqualGUID(item, guid); });
}

[[nodiscard]] DIDEVICEINSTANCEW virtual_instance() noexcept {
    DIDEVICEINSTANCEW result{};
    result.dwSize = static_cast<DWORD>(sizeof(result));
    result.dwDevType = DIDEVTYPE_HID |
                       static_cast<DWORD>(MAKEWORD(DI8DEVTYPE_GAMEPAD, DI8DEVTYPEGAMEPAD_STANDARD));
    result.wUsagePage = 0x01U;
    result.wUsage = 0x05U;
    constexpr std::wstring_view name = L"InputNext Gamepad";
    constexpr std::wstring_view product = L"InputNext Normalized Gamepad";
    std::ranges::copy(name, std::begin(result.tszInstanceName));
    std::ranges::copy(product, std::begin(result.tszProductName));
    return result;
}

[[nodiscard]] bool record_has_fingerprint(const core::device_record& record) noexcept {
    return record.identity == core::identity_quality::platform;
}

[[nodiscard]] HRESULT validate_live_targets(input_access::call& access,
    std::vector<gameinput_device>& devices) {
    for (auto entry = devices.begin(); entry != devices.end();) {
        const auto latest = access.core().read_latest(entry->record.target);
        if (!latest.has_value() && latest.error() == core::status::stale_target) {
            entry = devices.erase(entry);
            continue;
        }
        if (!latest.has_value() && latest.error() != core::status::no_data) {
            return map_core_status(latest.error());
        }
        if (latest.has_value() && latest->target != entry->record.target) {
            entry = devices.erase(entry);
            continue;
        }
        ++entry;
    }
    return S_OK;
}

[[nodiscard]] std::expected<std::vector<gameinput_device>, HRESULT> read_connection_facts(
    input_access::call& access) {
    const auto observed = access.core().enumerate();
    if (!observed.has_value()) {
        return std::unexpected{map_core_status(observed.error())};
    }
    if (observed->count > observed->entries.size()) {
        return std::unexpected{DIERR_GENERIC};
    }
    std::vector<gameinput_device> result;
    result.reserve(observed->count);
    for (std::size_t index = 0U; index < observed->count; ++index) {
        gameinput_device entry{.record = observed->entries[index],
            .path = {},
            .system_index = std::numeric_limits<std::size_t>::max(),
            .generated_instance = {}};
        const auto metadata = access.runtime().query_metadata(entry.record.target);
        if (metadata.has_value() && metadata->target == entry.record.target &&
            metadata->native.pnp_path_utf8.has_value()) {
            auto path = utf8_to_wide(metadata->native.pnp_path_utf8.value);
            if (path.has_value()) {
                entry.path = std::move(*path);
            }
        }
        result.push_back(std::move(entry));
    }
    const auto valid = validate_live_targets(access, result);
    if (FAILED(valid)) {
        return std::unexpected{valid};
    }
    return result;
}

void correlate_paths(std::vector<gameinput_device>& inputs,
    const std::vector<system_device>& system) noexcept {
    for (auto& input : inputs) {
        if (input.path.empty()) {
            continue;
        }
        const auto input_count = std::ranges::count_if(inputs,
            [&input](const auto& other) { return same_path(input.path, other.path); });
        std::size_t system_count{};
        std::size_t match{};
        for (std::size_t index = 0U; index < system.size(); ++index) {
            if (same_path(input.path, system[index].path)) {
                ++system_count;
                match = index;
            }
        }
        if (input_count == 1 && system_count == 1U &&
            guid_appears_once(system[match].instance.guidInstance, system)) {
            input.system_index = match;
        }
    }
}

[[nodiscard]] HRESULT generate_identities(std::vector<gameinput_device>& devices,
    const std::vector<system_device>& system) {
    for (auto& device : devices) {
        if (device.system_index < system.size()) {
            device.generated_instance = system[device.system_index].instance.guidInstance;
        } else if (record_has_fingerprint(device.record)) {
            if (!catalogue_identity::fingerprint_guid(device.record.hardware_fingerprint,
                    device.generated_instance)) {
                return DIERR_GENERIC;
            }
        } else if (!catalogue_identity::connection_guid(device.record.target,
                       device.generated_instance)) {
            return DIERR_GENERIC;
        }
    }
    return S_OK;
}

void append_projection(const gameinput_device& source,
    const std::vector<system_device>& system,
    catalogue_projection& out) {
    auto identity = source.system_index < system.size() ? system[source.system_index].instance
                                                        : virtual_instance();
    identity.dwSize = sizeof(identity);
    identity.guidFFDriver = {};
    identity.wUsagePage = 0x01U;
    identity.wUsage = 0x05U;
    identity.guidInstance = source.generated_instance;
    if (source.system_index >= system.size()) {
        identity.guidProduct = {};
    }
    const auto vendor_product =
        source.record.vendor_id != 0U && source.record.product_id != 0U
            ? std::optional<DWORD>{static_cast<DWORD>(source.record.vendor_id) |
                                   (static_cast<DWORD>(source.record.product_id) << 16U)}
            : std::nullopt;
    const auto path =
        source.system_index < system.size() ? system[source.system_index].property : std::nullopt;
    out.devices.push_back(device_identity{.target = source.record.target,
        .instance = identity,
        .path = path,
        .vendor_product = vendor_product});
    if (source.system_index < system.size()) {
        const auto& native_guid = system[source.system_index].instance.guidInstance;
        if (!has_guid(out.suppressed_system_guids, native_guid)) {
            out.suppressed_system_guids.push_back(native_guid);
        }
    }
}

enum class discovery_probe : std::uint8_t { pending, found, stop };

[[nodiscard]] std::expected<std::wstring, HRESULT> unique_native_path(IDirectInput8W& system,
    REFGUID instance) {
    const auto devices = read_system_devices(system);
    const system_device* match{};
    for (const auto& device : devices) {
        if (IsEqualGUID(device.instance.guidInstance, instance) == 0) {
            continue;
        }
        if (match != nullptr) {
            return std::unexpected{DIERR_UNSUPPORTED};
        }
        match = &device;
    }
    if (match == nullptr || match->path.empty() ||
        !guid_appears_once(match->instance.guidInstance, devices) ||
        std::ranges::count_if(devices,
            [match](const auto& device) { return same_path(match->path, device.path); }) != 1) {
        return std::unexpected{DIERR_UNSUPPORTED};
    }
    return match->path;
}

[[nodiscard]] discovery_probe probe_cached_path(input_access& access,
    IDirectInput8W& system,
    REFGUID instance,
    const std::wstring& native_path,
    device_identity& out) {
    auto current_access = access.enter();
    if (!current_access) {
        return discovery_probe::stop;
    }
    auto facts = read_connection_facts(current_access);
    current_access = input_access::call{};
    if (!facts.has_value()) {
        return facts.error() == HRESULT_FROM_WIN32(ERROR_BUSY) ? discovery_probe::pending
                                                               : discovery_probe::stop;
    }

    const gameinput_device* match{};
    for (const auto& fact : *facts) {
        if (!same_path(native_path, fact.path)) {
            continue;
        }
        if (match != nullptr) {
            return discovery_probe::stop;
        }
        match = &fact;
    }
    if (match == nullptr) {
        return discovery_probe::pending;
    }

    const auto exact_target = match->record.target;
    auto verification_access = access.enter();
    if (!verification_access) {
        return discovery_probe::stop;
    }
    device_identity verified{};
    const auto status =
        find_projected_device(std::move(verification_access), system, instance, verified);
    if (status == S_OK && verified.target == exact_target) {
        out = verified;
        return discovery_probe::found;
    }
    return status == HRESULT_FROM_WIN32(ERROR_BUSY) ? discovery_probe::pending
                                                    : discovery_probe::stop;
}

} // namespace

bool is_game_controller_type(const DWORD device_type) noexcept {
    switch (GET_DIDEVICE_TYPE(device_type)) {
    case DI8DEVTYPE_JOYSTICK:
    case DI8DEVTYPE_GAMEPAD:
    case DI8DEVTYPE_DRIVING:
    case DI8DEVTYPE_FLIGHT:
    case DI8DEVTYPE_1STPERSON:
    case DI8DEVTYPE_REMOTE:
    case DI8DEVTYPE_SUPPLEMENTAL:
        return true;
    default:
        return false;
    }
}

bool is_game_controller_request(const DWORD device_type) noexcept {
    return device_type == DI8DEVCLASS_ALL || device_type == DI8DEVCLASS_GAMECTRL ||
           is_game_controller_type(device_type);
}

bool instance_matches_type(const DIDEVICEINSTANCEW& instance, const DWORD device_type) noexcept {
    if (device_type == DI8DEVCLASS_ALL || device_type == DI8DEVCLASS_GAMECTRL) {
        return true;
    }
    if (!is_game_controller_type(device_type)) {
        return false;
    }

    const auto requested_type = device_type & 0xffU;
    const auto requested_subtype = (device_type >> 8U) & 0xffU;
    const auto actual_type = static_cast<DWORD>(GET_DIDEVICE_TYPE(instance.dwDevType));
    const auto actual_subtype = static_cast<DWORD>(GET_DIDEVICE_SUBTYPE(instance.dwDevType));
    return requested_type == actual_type &&
           (requested_subtype == 0U || requested_subtype == actual_subtype);
}

bool instance_matches_flags(const DIDEVICEINSTANCEW& /*unused*/, const DWORD flags) noexcept {
    return (flags & DIEDFL_FORCEFEEDBACK) == 0U;
}

HRESULT project_catalogue(input_access::call access,
    IDirectInput8W& system,
    catalogue_projection& out) noexcept {
    out = {};
    if (!access) {
        return DIERR_INPUTLOST;
    }
    try {
        auto facts = read_connection_facts(access);
        if (!facts.has_value()) {
            return facts.error();
        }
        access = input_access::call{};
        const auto system_devices = read_system_devices(system);
        correlate_paths(*facts, system_devices);
        const auto generated = generate_identities(*facts, system_devices);
        if (FAILED(generated)) {
            return generated;
        }
        out.devices.reserve(facts->size());
        out.suppressed_system_guids.reserve(facts->size());
        for (const auto& device : *facts) {
            append_projection(device, system_devices, out);
        }
        return S_OK;
    } catch (const std::bad_alloc&) {
        out = {};
        return DIERR_OUTOFMEMORY;
    } catch (...) {
        out = {};
        return DIERR_GENERIC;
    }
}

HRESULT find_projected_device(input_access::call access,
    IDirectInput8W& system,
    REFGUID instance,
    device_identity& out) noexcept {
    out = {};
    catalogue_projection projection{};
    const auto status = project_catalogue(std::move(access), system, projection);
    if (FAILED(status)) {
        return status;
    }
    const auto found = std::ranges::find_if(projection.devices, [&instance](const auto& item) {
        return IsEqualGUID(item.instance.guidInstance, instance);
    });
    if (found == projection.devices.end()) {
        return S_FALSE;
    }
    out = *found;
    return S_OK;
}

HRESULT discover_projected_device(input_access& access,
    IDirectInput8W& system,
    REFGUID instance,
    device_identity& out) noexcept {
    out = {};
    try {
        auto warm_access = access.enter();
        if (!warm_access) {
            return S_FALSE;
        }
        const auto warm_status =
            find_projected_device(std::move(warm_access), system, instance, out);
        if (warm_status == S_OK) {
            return S_OK;
        }
        out = {};
        if (warm_status != S_FALSE && warm_status != HRESULT_FROM_WIN32(ERROR_BUSY)) {
            return S_FALSE;
        }
        if (native_control_reentrant()) {
            return S_FALSE;
        }
        auto native_path = unique_native_path(system, instance);
        if (!native_path.has_value()) {
            return S_FALSE;
        }

        const auto waiter = access.discovery().begin();
        if (waiter == nullptr) {
            return S_FALSE;
        }

        while (waiter->prepare()) {
            const auto probe = probe_cached_path(access, system, instance, *native_path, out);
            if (probe == discovery_probe::found) {
                return S_OK;
            }
            if (probe == discovery_probe::stop) {
                return S_FALSE;
            }

            const std::array events{waiter->event()};
            std::size_t signalled{};
            const auto wait_status = wait_for_native_control(events, signalled, waiter->deadline());
            if (wait_status != S_OK || signalled != 0U) {
                return S_FALSE;
            }
        }
        return S_FALSE;
    } catch (...) {
        out = {};
        return S_FALSE;
    }
}

} // namespace inputnext::dinput
