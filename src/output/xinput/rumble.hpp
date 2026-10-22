#pragma once

#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/xinput.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {

class xinput_rumble_transaction;

struct xinput_service_id final {
    authority_incarnation authority{};
    context_id id{};

    friend bool operator==(const xinput_service_id&, const xinput_service_id&) noexcept = default;
};

struct xinput_rumble_key final {
    xinput_service_id service{};
    xinput::assignment occupant{};
    std::uint64_t enable_generation{};
    focus::foreground_epoch epoch{};

    friend bool operator==(const xinput_rumble_key&, const xinput_rumble_key&) noexcept = default;
};

class xinput_rumble final {
public:
    using transaction = xinput_rumble_transaction;

    ~xinput_rumble() = default;
    xinput_rumble(const xinput_rumble&) = delete;
    xinput_rumble& operator=(const xinput_rumble&) = delete;
    xinput_rumble(xinput_rumble&&) = delete;
    xinput_rumble& operator=(xinput_rumble&&) = delete;

    [[nodiscard]] xinput_service_id service_id() const noexcept { return service_; }

    [[nodiscard]] bool matches_focus(const focus::observer& observer) const noexcept;
    [[nodiscard]] transaction begin();

private:
    friend class composition;
    friend class xinput_rumble_transaction;
    explicit xinput_rumble(authority& owner, xinput_service_id service) noexcept;
    [[nodiscard]] static result<std::unique_ptr<xinput_rumble>> create(authority& owner);

    authority& owner_;
    const xinput_service_id service_;
};

class xinput_rumble_transaction final {
public:
    ~xinput_rumble_transaction() = default;

    xinput_rumble_transaction(xinput_rumble_transaction&&) noexcept = default;
    xinput_rumble_transaction& operator=(xinput_rumble_transaction&&) noexcept = delete;
    xinput_rumble_transaction(const xinput_rumble_transaction&) = delete;
    xinput_rumble_transaction& operator=(const xinput_rumble_transaction&) = delete;

    [[nodiscard]] focus::observer::presentation_guard& presentation() noexcept;
    [[nodiscard]] bool owns_key(const xinput_rumble_key& key,
        const std::optional<internal_context>& current) const noexcept;
    [[nodiscard]] bool target_is_qualified(const xinput_rumble_key& key) const noexcept;
    [[nodiscard]] result<void> qualify(const xinput_rumble_key& key);
    [[nodiscard]] result<std::uint64_t> submit(const xinput_rumble_key& key,
        std::optional<internal_context>& current,
        rumble_value value,
        std::uint64_t sequence);
    void revoke_and_ack(std::optional<internal_context>& current) noexcept;

private:
    friend class xinput_rumble;
    explicit xinput_rumble_transaction(xinput_rumble& owner);
    [[nodiscard]] result<void> preflight_existing(const xinput_rumble_key& key,
        const internal_context& current,
        std::uint64_t sequence) const noexcept;
    [[nodiscard]] result<std::uint64_t> reserve_new_context_id() noexcept;

    xinput_rumble& owner_;
    // Destruction order releases presentation and authority guards before the registry pins.
    std::array<std::optional<read_pin>, 4U> target_pins{};
    std::unique_lock<std::mutex> authority_lock_;
    focus::observer::presentation_guard presentation_;
};

} // namespace inputnext::output::detail
