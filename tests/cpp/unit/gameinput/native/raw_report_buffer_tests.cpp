#include "gameinput/native/windows_raw_output_report.hpp"

#include <Windows.h>

#include <GameInput.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <optional>
#include <span>

namespace {

namespace gameinput = GameInput::v4;
using inputnext::gameinput_detail::fill_windows_raw_output_report;

class controlled_raw_report final : public gameinput::IGameInputRawDeviceReport {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) noexcept override {
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1U; }

    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1U; }

    [[noreturn]] void STDMETHODCALLTYPE GetDevice(
        [[maybe_unused]] gameinput::IGameInputDevice** device) noexcept override {
        std::terminate();
    }

    void STDMETHODCALLTYPE GetReportInfo(
        gameinput::GameInputRawDeviceReportInfo* info) noexcept override {
        ++descriptor_queries;
        *info = {.kind = gameinput::GameInputRawOutputReport, .id = 2U, .size = 547U};
    }

    std::size_t STDMETHODCALLTYPE GetRawDataSize() noexcept override { return size; }

    std::size_t STDMETHODCALLTYPE GetRawData(const std::size_t count,
        void* bytes) noexcept override {
        const auto copied = std::min(count, payload.size());
        std::memcpy(bytes, payload.data(), copied);
        return copied;
    }

    bool STDMETHODCALLTYPE SetRawData(const std::size_t count,
        const void* bytes) noexcept override {
        ++fill_calls;
        if (!accept_fill || count != size || count > payload.size()) {
            return false;
        }
        std::memcpy(payload.data(), bytes, count);
        if (size_after_fill.has_value()) {
            size = *size_after_fill;
        }
        return true;
    }

    std::size_t size{64U};
    std::optional<std::size_t> size_after_fill;
    bool accept_fill{true};
    std::size_t fill_calls{};
    std::size_t descriptor_queries{};
    std::array<std::byte, 64U> payload{};
};

} // namespace

TEST_CASE("gameinput.mapping.raw_report_fill_uses_the_actual_buffer_size",
    "[unit][gameinput][native][raw_output]") {
    std::array<std::byte, 64U> bytes{};
    bytes.fill(std::byte{0xA5});
    constexpr std::array sizes{48U, 63U, 64U};
    for (const auto size : sizes) {
        CAPTURE(size);
        controlled_raw_report report;
        report.size = size;
        const auto payload = std::span{bytes}.first(size);
        REQUIRE(fill_windows_raw_output_report(report, payload));
        CHECK(report.fill_calls == 1U);
        CHECK(report.descriptor_queries == 0U);
        CHECK(std::equal(payload.begin(), payload.end(), report.payload.begin()));
        CHECK(report.GetRawDataSize() == size);
    }
}

TEST_CASE("gameinput.mapping.raw_report_fill_rejects_resizing_and_failed_fills",
    "[unit][gameinput][native][raw_output]") {
    controlled_raw_report report;
    const std::array<std::byte, 64U> bytes{};
    SECTION("shorter source does not resize the report") {
        CHECK_FALSE(fill_windows_raw_output_report(report, std::span{bytes}.first(48U)));
        CHECK(report.fill_calls == 0U);
    }
    SECTION("larger source does not resize the report") {
        report.size = 48U;
        CHECK_FALSE(fill_windows_raw_output_report(report, bytes));
        CHECK(report.fill_calls == 0U);
    }
    SECTION("empty report is rejected") {
        report.size = 0U;
        CHECK_FALSE(fill_windows_raw_output_report(report, {}));
        CHECK(report.fill_calls == 0U);
    }
    SECTION("native refusal is preserved") {
        report.accept_fill = false;
        CHECK_FALSE(fill_windows_raw_output_report(report, bytes));
        CHECK(report.fill_calls == 1U);
    }
    SECTION("unexpected native size change is rejected") {
        report.size_after_fill = 63U;
        CHECK_FALSE(fill_windows_raw_output_report(report, bytes));
        CHECK(report.fill_calls == 1U);
    }
    CHECK(report.descriptor_queries == 0U);
}
