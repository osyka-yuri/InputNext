#pragma once

#include <Windows.h>
#include <Xinput.h>

#include <cstddef>
#include <cstdint>

namespace inputnext::xinput {

enum class flavor : std::uint8_t {
    v1_3 = 0U,
    v1_4 = 1U,
    v9_1_0 = 2U,
};

#pragma pack(push, 1)

struct capabilities_ex final {
    XINPUT_CAPABILITIES capabilities{};
    WORD vendor_id{};
    WORD product_id{};
    WORD product_version{};
    WORD unknown1{};
};

#pragma pack(pop)

static_assert(sizeof(capabilities_ex) == 28U);
static_assert(offsetof(capabilities_ex, vendor_id) == 20U);
static_assert(offsetof(capabilities_ex, unknown1) == 26U);

} // namespace inputnext::xinput
