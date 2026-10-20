#pragma once

#include <inputnext/device_identity.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::focus {

namespace detail {
class observer_test_access;
}

struct process_instance final {
    // PID plus creation time distinguishes reuse of a process ID.
    std::uint32_t pid{};
    std::uint64_t creation_time_100ns{};

    friend bool operator==(const process_instance&, const process_instance&) = default;
};

enum class evidence : std::uint8_t {
    unavailable,
    background,
    foreground,
};

struct reading final {
    process_instance process{};
    evidence state{evidence::unavailable};
    std::uint32_t native_code{};
};

struct snapshot final {
    runtime_id observer_incarnation{};
    process_instance process{};
    std::uint64_t revision{};
    evidence state{evidence::unavailable};
    // Monotonic start of the latest accepted foreground/background observation.
    std::chrono::steady_clock::time_point observation_started;
    bool open{};
    bool exhausted{};
    // These fields are diagnostic and do not affect the evidence revision.
    std::uint64_t completed_observations{};
    std::uint32_t native_code{};
};

// Identifies one foreground evidence revision without retaining that observation's short lease.
struct foreground_epoch final {
    runtime_id observer_incarnation{};
    process_instance process{};
    std::uint64_t revision{};

    friend bool operator==(const foreground_epoch&, const foreground_epoch&) = default;
};

class source {
public:
    source() = default;
    virtual ~source() = default;

    source(const source&) = delete;
    source& operator=(const source&) = delete;
    source(source&&) = delete;
    source& operator=(source&&) = delete;

    [[nodiscard]] virtual process_instance process() const noexcept = 0;
    [[nodiscard]] virtual reading observe() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<source> make_current_process_source();

class observer final {
public:
    // Owns the immutable source for its lifetime. Borrowers and guards must be quiesced before destruction.
    observer(runtime_id incarnation, std::unique_ptr<source> owned_source);
    ~observer();

    observer(const observer&) = delete;
    observer& operator=(const observer&) = delete;
    observer(observer&&) = delete;
    observer& operator=(observer&&) = delete;

    class presentation_guard;

    // Holds the presentation gate; guard operations re-evaluate freshness while it is held.
    [[nodiscard]] presentation_guard lock_presentation() const;
    [[nodiscard]] snapshot published() const;
    // Calls must be externally serialized. May wait for a blocked source call to finish.
    void stop();

private:
    friend class detail::observer_test_access;

    using clock_callback = std::chrono::steady_clock::time_point (*)(const void*) noexcept;

    observer(runtime_id incarnation,
        std::unique_ptr<source> owned_source,
        clock_callback now,
        const void* clock_context);

    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

class observer::presentation_guard final {
public:
    ~presentation_guard() = default;

    // Moved-from guards return an empty publication and never match foreground evidence.
    presentation_guard(presentation_guard&& other) noexcept;
    presentation_guard& operator=(presentation_guard&& other) noexcept;
    presentation_guard(const presentation_guard&) = delete;
    presentation_guard& operator=(const presentation_guard&) = delete;

    [[nodiscard]] snapshot published() const noexcept;
    // Captured and current evidence must both be fresh, with identity, evidence, revision, and
    // openness matching.
    [[nodiscard]] bool matches_foreground(const snapshot& captured) const noexcept;
    // Current foreground evidence can remain the same epoch across timestamp refreshes.
    [[nodiscard]] std::optional<foreground_epoch> current_foreground_epoch() const noexcept;
    [[nodiscard]] bool matches_foreground_epoch(const foreground_epoch& epoch) const noexcept;

private:
    friend class observer;

    presentation_guard(std::unique_lock<std::mutex>&& lock, implementation* state) noexcept;

    std::unique_lock<std::mutex> lock_;
    implementation* implementation_{};
};

} // namespace inputnext::focus
