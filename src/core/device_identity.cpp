#include "device_identity.hpp"

#include "core.hpp"
#include <inputnext/device_metadata.hpp>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace inputnext::core::detail {
namespace {

class algorithm_provider final {
public:
    algorithm_provider() noexcept
        : status_(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0U)) {}

    ~algorithm_provider() {
        if (handle_ != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(handle_, 0U));
        }
    }

    algorithm_provider(const algorithm_provider&) = delete;
    algorithm_provider& operator=(const algorithm_provider&) = delete;
    algorithm_provider(algorithm_provider&&) = delete;
    algorithm_provider& operator=(algorithm_provider&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return BCRYPT_SUCCESS(status_); }

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
    NTSTATUS status_{};
};

[[nodiscard]] bool is_zero(const opaque_device_id& value) noexcept {
    return std::ranges::all_of(value.bytes,
        [](const std::byte byte) { return byte == std::byte{}; });
}

} // namespace

projected_identity fingerprint_device(const metadata_snapshot& metadata) {
    const auto& device_id = metadata.native.gameinput_device_id;
    const auto& root_id = metadata.native.gameinput_root_id;
    const auto device_bytes = device_id.has_value() ? device_id.value : opaque_device_id{};
    const auto root_bytes = root_id.has_value() ? root_id.value : opaque_device_id{};

    if (is_zero(device_bytes) && is_zero(root_bytes)) {
        return {};
    }

    const algorithm_provider algorithm{};
    if (!algorithm.valid()) {
        return {};
    }

    ULONG object_size{};
    ULONG result_size{};
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm.get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &result_size,
            0U)) ||
        result_size != static_cast<ULONG>(sizeof(object_size)) || object_size == 0U) {
        return {};
    }

    std::vector<UCHAR> hash_object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.get(),
            &hash,
            hash_object.data(),
            object_size,
            nullptr,
            0U,
            0U))) {
        return {};
    }

    const auto close_hash = [&hash] {
        if (hash != nullptr) {
            static_cast<void>(BCryptDestroyHash(hash));
        }
    };
    constexpr auto input_size = static_cast<ULONG>(gameinput_device_id_bytes) * 2UL;
    std::array<UCHAR, input_size> input{};
    for (std::size_t index = 0U; index < gameinput_device_id_bytes; ++index) {
        input[index] = std::to_integer<UCHAR>(device_bytes.bytes[index]);
        input[gameinput_device_id_bytes + index] = std::to_integer<UCHAR>(root_bytes.bytes[index]);
    }

    if (!BCRYPT_SUCCESS(BCryptHashData(hash, input.data(), input_size, 0U))) {
        close_hash();
        return {};
    }

    projected_identity result{.quality = identity_quality::platform};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash,
            result.fingerprint.data(),
            static_cast<ULONG>(result.fingerprint.size()),
            0U))) {
        close_hash();
        return {};
    }
    close_hash();
    return result;
}

} // namespace inputnext::core::detail
