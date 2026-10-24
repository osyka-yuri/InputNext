#include "gameinput/native/windows_raw_output_report.hpp"

#include <inputnext/output_execution.hpp>

#include <Windows.h>

#include <GameInput.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE("gameinput.mapping.raw_output_results_preserve_native_errors",
    "[gameinput][native][unit][raw_output]") {
    namespace gameinput = GameInput::v4;
    using output::native_status;
    const std::array cases{
        std::pair{S_OK, native_status::accepted},
        std::pair{S_FALSE, native_status::accepted},
        std::pair{gameinput::GAMEINPUT_E_DEVICE_DISCONNECTED, native_status::disconnected},
        std::pair{gameinput::GAMEINPUT_E_DEVICE_NOT_FOUND, native_status::disconnected},
        std::pair{gameinput::GAMEINPUT_E_OBJECT_NO_LONGER_EXISTS, native_status::disconnected},
        std::pair{E_NOTIMPL, native_status::unsupported},
        std::pair{HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), native_status::unsupported},
        std::pair{HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED), native_status::unsupported},
        std::pair{gameinput::GAMEINPUT_E_FEEDBACK_NOT_SUPPORTED, native_status::unsupported},
        std::pair{gameinput::GAMEINPUT_E_AGGREGATE_OPERATION_NOT_SUPPORTED,
            native_status::unsupported},
        std::pair{E_INVALIDARG, native_status::failed},
        std::pair{E_POINTER, native_status::failed},
        std::pair{E_ACCESSDENIED, native_status::failed},
        std::pair{E_FAIL, native_status::retryable_failure},
        std::pair{E_OUTOFMEMORY, native_status::retryable_failure},
    };
    for (const auto& [result, expected] : cases) {
        CAPTURE(result);
        const auto classified = classify_raw_output_result(result);
        CHECK(classified.status == expected);
        CHECK(std::cmp_equal(classified.native_code, static_cast<std::uint32_t>(result)));
    }
}

} // namespace
} // namespace inputnext::gameinput_detail
