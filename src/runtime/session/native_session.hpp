#pragma once

#include <inputnext/gameinput_runtime.hpp>

#include "native_session_dependencies.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace inputnext::xinput {
class service;
} // namespace inputnext::xinput

namespace inputnext::runtime_detail {

enum class native_session_drain_phase : std::uint8_t {
    not_started,
    closing_admission,
    stopping_connections,
    stopping_runtime,
    drained,
};

struct native_session_drain_progress final {
    native_session_drain_phase phase{native_session_drain_phase::not_started};
    std::chrono::steady_clock::duration elapsed{};
    std::uint64_t stop_attempts{};
    std::uint64_t exception_count{};
    bool has_last_stop_report{};
    gameinput_stop_report last_stop_report{};
};

// Owns one prepared native graph. The borrowed presentation service remains valid until
// shutdown(), which requires external callers to be quiescent.
class native_session final {
public:
    [[nodiscard]] static std::unique_ptr<native_session> prepare();
    [[nodiscard]] static std::unique_ptr<native_session> prepare(
        native_session_dependencies dependencies);
    ~native_session() noexcept;

    native_session(const native_session&) = delete;
    native_session& operator=(const native_session&) = delete;
    native_session(native_session&&) = delete;
    native_session& operator=(native_session&&) = delete;

    // Performs the session's single synchronous native startup attempt. The returned failure
    // owns its complete diagnostic value and remains valid after this call returns.
    [[nodiscard]] gameinput_result<void> start() noexcept;

    // Releases the parked maintenance worker after the session's terminal result is published.
    void activate() noexcept;

    // Returns a stable borrow; callers must stop using it before shutdown().
    [[nodiscard]] xinput::service& presentation() noexcept;

    // Requires external service callers to be quiescent. May wait for native calls to finish.
    void shutdown() noexcept;

    // Returns an atomic observational snapshot; it never waits for drain work.
    [[nodiscard]] native_session_drain_progress drain_progress() const noexcept;

private:
    struct implementation;

    explicit native_session(std::unique_ptr<implementation> state) noexcept;

    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::runtime_detail
