#pragma once

#include "device_identity.hpp"
#include "error.hpp"
#include "input_sample.hpp"

#include <cstdint>

namespace inputnext {

namespace detail {
struct connection_record;
}

struct connection_snapshot final {
    connection_target target{};
    published_sample sample{};

    friend constexpr bool operator==(const connection_snapshot&,
        const connection_snapshot&) noexcept = default;
};

// A pin keeps its source and sample resident but borrows registry storage. Keep the registry alive
// while using or releasing the pin; stale identity rejection cannot make post-destruction access safe.
// Shutdown may overlap pin release; stop external calls and join workers before registry destruction.
class read_pin final {
public:
    read_pin() noexcept = default;
    ~read_pin();

    read_pin(const read_pin&) = delete;
    read_pin& operator=(const read_pin&) = delete;
    read_pin(read_pin&& other) noexcept;
    read_pin& operator=(read_pin&& other) noexcept;

    [[nodiscard]] result<connection_snapshot> read() const;
    [[nodiscard]] connection_target target() const noexcept;
    void release() noexcept;

private:
    friend class read_binding;
    friend class device_registry;

    read_pin(detail::connection_record* record, connection_target target) noexcept;

    detail::connection_record* record_{};
    connection_target target_{};
};

// This binding borrows registry storage; identity checks do not keep that storage alive.
// Stop external calls and join workers before destroying the registry.
class read_binding final {
public:
    read_binding() noexcept = default;
    ~read_binding() = default;

    read_binding(const read_binding&) noexcept = default;
    read_binding& operator=(const read_binding&) noexcept = default;
    read_binding(read_binding&&) noexcept = default;
    read_binding& operator=(read_binding&&) noexcept = default;

    [[nodiscard]] result<read_pin> capture() const;
    [[nodiscard]] result<connection_snapshot> read() const;

private:
    friend class device_registry;

    read_binding(detail::connection_record* record, connection_target target) noexcept;

    detail::connection_record* record_{};
    connection_target target_{};
};

// This binding borrows registry storage; rejected stale publications do not make post-destruction
// access safe. Stop external calls and join workers before destroying the registry.
class producer_binding final {
public:
    producer_binding() noexcept = default;
    ~producer_binding() = default;

    producer_binding(const producer_binding&) noexcept = default;
    producer_binding& operator=(const producer_binding&) noexcept = default;
    producer_binding(producer_binding&&) noexcept = default;
    producer_binding& operator=(producer_binding&&) noexcept = default;

    [[nodiscard]] result<std::uint64_t> publish(const input_update& update) const;

private:
    friend class device_registry;

    producer_binding(detail::connection_record* record, connection_target target) noexcept;

    detail::connection_record* record_{};
    connection_target target_{};
};

struct admitted_connection final {
    connection_target target{};
    read_binding reader{};
    producer_binding producer;
};

} // namespace inputnext
