#include "identity.hpp"

#include <inputnext/device_identity.hpp>

#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace inputnext::dinput::catalogue_identity {
namespace {

class bcrypt_algorithm final {
public:
    bcrypt_algorithm() noexcept
        : status_(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0U)) {}

    ~bcrypt_algorithm() {
        if (handle_ != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(handle_, 0U));
        }
    }

    bcrypt_algorithm(const bcrypt_algorithm&) = delete;
    bcrypt_algorithm& operator=(const bcrypt_algorithm&) = delete;
    bcrypt_algorithm(bcrypt_algorithm&&) = delete;
    bcrypt_algorithm& operator=(bcrypt_algorithm&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return BCRYPT_SUCCESS(status_); }

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
    NTSTATUS status_{};
};

class bcrypt_hash final {
public:
    bcrypt_hash(BCRYPT_ALG_HANDLE algorithm, PUCHAR object, const ULONG object_size) noexcept
        : status_(BCryptCreateHash(algorithm, &handle_, object, object_size, nullptr, 0U, 0U)) {}

    ~bcrypt_hash() {
        if (handle_ != nullptr) {
            static_cast<void>(BCryptDestroyHash(handle_));
        }
    }

    bcrypt_hash(const bcrypt_hash&) = delete;
    bcrypt_hash& operator=(const bcrypt_hash&) = delete;
    bcrypt_hash(bcrypt_hash&&) = delete;
    bcrypt_hash& operator=(bcrypt_hash&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return BCRYPT_SUCCESS(status_); }

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_{};
    NTSTATUS status_{};
};

[[nodiscard]] bool namespaced_guid(const std::string_view namespace_name,
    const std::span<const std::uint8_t> value,
    GUID& result) {
    bcrypt_algorithm const algorithm{};
    if (!algorithm.valid()) {
        return false;
    }

    ULONG object_size{};
    ULONG result_size{};
    const auto property_status = BCryptGetProperty(algorithm.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        static_cast<ULONG>(sizeof(object_size)),
        &result_size,
        0U);
    if (!BCRYPT_SUCCESS(property_status) ||
        result_size != static_cast<ULONG>(sizeof(object_size)) || object_size == 0U) {
        return false;
    }

    std::vector<UCHAR> object(object_size);
    bcrypt_hash const hash{algorithm.get(), object.data(), object_size};
    if (!hash.valid()) {
        return false;
    }

    std::vector<UCHAR> input(namespace_name.size() + value.size());
    std::memcpy(input.data(), namespace_name.data(), namespace_name.size());
    std::memcpy(input.data() + namespace_name.size(), value.data(), value.size());
    auto status = BCryptHashData(hash.get(), input.data(), static_cast<ULONG>(input.size()), 0U);

    std::array<UCHAR, 32U> digest{};
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0U);
    }
    if (!BCRYPT_SUCCESS(status)) {
        return false;
    }

    std::memcpy(&result, digest.data(), sizeof(result));
    result.Data3 = static_cast<WORD>((result.Data3 & 0x0fffU) | 0x8000U);
    result.Data4[0U] = static_cast<BYTE>((result.Data4[0U] & 0x3fU) | 0x80U);
    return true;
}

} // namespace

bool fingerprint_guid(const std::array<std::uint8_t, 32U>& fingerprint, GUID& result) {
    return namespaced_guid("InputNext.DirectInput.platform.v1", fingerprint, result);
}

bool connection_guid(const connection_target& target, GUID& result) {
    std::array<std::uint8_t, 32U> identity{};
    for (std::size_t index = 0U; index < target.runtime.bytes.size(); ++index) {
        identity[index] = std::to_integer<std::uint8_t>(target.runtime.bytes[index]);
    }
    const auto append_u64 = [&identity](const std::uint64_t value, const std::size_t offset) {
        for (std::size_t index = 0U; index < sizeof(value); ++index) {
            identity[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
        }
    };
    append_u64(target.device.value, 16U);
    append_u64(target.connection.value, 24U);
    return namespaced_guid("InputNext.DirectInput.connection.v1", identity, result);
}

} // namespace inputnext::dinput::catalogue_identity
