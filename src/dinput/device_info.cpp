#include "device_info.hpp"

#include "native_types.hpp"
#include "profile.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>

namespace inputnext::dinput {
namespace {

constexpr DWORD force_feedback_filters = DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER;

[[nodiscard]] bool copy_ansi(std::span<char, MAX_PATH> destination,
    std::span<const wchar_t, MAX_PATH> source) noexcept {
    const auto end = std::ranges::find(source, L'\0');
    if (end == source.end()) {
        return false;
    }
    auto length = static_cast<std::size_t>(end - source.begin());
    while (length > 0U) {
        const auto written = WideCharToMultiByte(CP_ACP,
            0U,
            source.data(),
            static_cast<int>(length),
            destination.data(),
            MAX_PATH - 1,
            nullptr,
            nullptr);
        if (written > 0) {
            destination[static_cast<std::size_t>(written)] = '\0';
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
        --length;
        if (length > 0U && IS_LOW_SURROGATE(source[length]) &&
            IS_HIGH_SURROGATE(source[length - 1U])) {
            --length;
        }
    }
    destination.front() = '\0';
    return true;
}

template <typename Instance>
[[nodiscard]] Instance object_fields(const object_descriptor& object) noexcept {
    Instance result{};
    result.dwSize = sizeof(result);
    result.guidType = object.guid;
    result.dwOfs = object.native_offset;
    result.dwType = object.id;
    result.dwFlags = object.kind == object_kind::axis ? DIDOI_ASPECTPOSITION : 0U;
    result.wUsagePage = object.usage_page;
    result.wUsage = object.usage;
    return result;
}

} // namespace

HRESULT copy_instance_ansi(const DIDEVICEINSTANCEW& source,
    DIDEVICEINSTANCEA& destination) noexcept {
    DIDEVICEINSTANCEA value{};
    value.dwSize = sizeof(value);
    value.guidInstance = source.guidInstance;
    value.guidProduct = source.guidProduct;
    value.dwDevType = source.dwDevType;
    value.guidFFDriver = source.guidFFDriver;
    value.wUsagePage = source.wUsagePage;
    value.wUsage = source.wUsage;
    if (!copy_ansi(value.tszInstanceName, source.tszInstanceName) ||
        !copy_ansi(value.tszProductName, source.tszProductName)) {
        return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
    }
    destination = value;
    return DI_OK;
}

DIDEVICEOBJECTINSTANCEW describe_object(const object_descriptor& object) noexcept {
    auto result = object_fields<DIDEVICEOBJECTINSTANCEW>(object);
    const auto length =
        std::min<std::size_t>(std::char_traits<wchar_t>::length(object.name), MAX_PATH - 1U);
    std::copy_n(object.name, length, result.tszName);
    return result;
}

DIDEVICEOBJECTINSTANCEA describe_object_ansi(const object_descriptor& object) noexcept {
    const auto wide = describe_object(object);
    auto result = object_fields<DIDEVICEOBJECTINSTANCEA>(object);
    static_cast<void>(copy_ansi(result.tszName, wide.tszName));
    return result;
}

bool valid_object_filter(DWORD flags) noexcept {
    constexpr DWORD supported = DIDFT_AXIS | DIDFT_BUTTON | DIDFT_POV | DIDFT_OUTPUT |
                                DIDFT_COLLECTION | DIDFT_NODATA | DIDFT_VENDORDEFINED |
                                DIDFT_ALIAS | DIDFT_INSTANCEMASK | force_feedback_filters;
    return (flags & ~supported) == 0U;
}

bool matches_object_filter(const object_descriptor& object, DWORD flags) noexcept {
    if ((flags & force_feedback_filters) != 0U) {
        return false;
    }
    return flags == DIDFT_ALL || (flags & DIDFT_INSTANCEMASK) == DIDFT_NOCOLLECTION ||
           (DIDFT_GETTYPE(object.id) & flags) != 0U;
}

} // namespace inputnext::dinput
