#pragma once

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "profiles/application.hpp"
#include "profiles/model.hpp"
#include "support/xinput/test_ports.hpp"
#include "values/required_value.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/source_lifetime.hpp>
#include <inputnext/xinput.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace inputnext::profiles_test_support {

using inputnext::test::required_value;
using inputnext::xinput_test_support::test_connection;
using profiles::application;
using profiles::profile;

inline constexpr output::internal_context_options application_options{
    .owner = output::internal_owner_key{1501U},
    .priority = output::internal_priority::game_profile,
    .ttl = output::ttl_permission::finite_or_indefinite,
    .foreground = output::foreground_policy::foreground_bound,
};

[[nodiscard]] inline output::trigger_value feedback_trigger(const float strength) noexcept {
    return output::trigger_value{.mode = output::trigger_mode::feedback,
        .layout = output::trigger_layout::uniform,
        .start = 0.0F,
        .end = 1.0F,
        .strength = strength};
}

[[nodiscard]] inline profile left_profile(const float strength) noexcept {
    return profile{.left_trigger = feedback_trigger(strength), .right_trigger = std::nullopt};
}

class application_fixture final {
public:
    application_fixture()
        : metadata_(),
          xinput_(focus_.observer(), metadata_),
          composition_(xinput_test_support::make_runtime_id(0x41U),
              devices_.registry(),
              focus_.observer()),
          producer_(
              inputnext::test::output::make_internal_producer(composition_, application_options)),
          app_(producer_, xinput_) {
        output::detail::authority_test_access::use_clock(authority(), clock_ticks_);
    }

    application_fixture(const application_fixture&) = delete;
    application_fixture& operator=(const application_fixture&) = delete;
    application_fixture(application_fixture&&) = delete;
    application_fixture& operator=(application_fixture&&) = delete;
    ~application_fixture() = default;

    [[nodiscard]] test_connection add_connection(gamepad_state state = {}) {
        auto connection = devices_.add(state);
        metadata_.set_snapshot(xinput_test_support::metadata_for(connection.connection.target));
        return connection;
    }

    [[nodiscard]] test_connection replace_connection(const connection_target expected) {
        std::unique_ptr<source_lifetime> candidate =
            std::make_unique<xinput_test_support::test_source_lifetime>();
        auto replaced = devices_.registry().replace(expected, candidate);
        if (!replaced.has_value()) {
            throw std::runtime_error{"profile test could not replace registry connection"};
        }
        auto connection = test_connection{.connection = replaced.value()};
        metadata_.set_snapshot(xinput_test_support::metadata_for(connection.connection.target));
        return connection;
    }

    [[nodiscard]] xinput::assignment assign(const test_connection& connection) {
        auto assigned = xinput_.assign(connection.connection.target, connection.connection.reader);
        if (!assigned.has_value()) {
            throw std::runtime_error{"profile test could not assign XInput position"};
        }
        return assigned.value();
    }

    [[nodiscard]] output::logical_snapshot snapshot(connection_target target) {
        auto observed = authority().read_snapshot(target);
        if (!observed.has_value()) {
            throw std::runtime_error{"profile test could not read authority snapshot"};
        }
        return observed.value();
    }

    [[nodiscard]] std::optional<output::detail::authority_test_access::record_observation> record(
        output::context_identity identity) {
        return output::detail::authority_test_access::observe_record(authority(), identity);
    }

    [[nodiscard]] xinput_test_support::focus_fixture& focus() noexcept { return focus_; }

    [[nodiscard]] xinput::service& xinput() noexcept { return xinput_; }

    [[nodiscard]] output::authority& authority() noexcept {
        return composition_.public_authority();
    }

    [[nodiscard]] output::detail::composition& composition() noexcept { return composition_; }

    [[nodiscard]] output::detail::internal_producer& producer() noexcept { return producer_; }

    [[nodiscard]] application& app() noexcept { return app_; }

    void advance_clock(const std::chrono::steady_clock::duration amount) noexcept {
        clock_ticks_.fetch_add(amount.count(), std::memory_order_release);
    }

private:
    xinput_test_support::registry_fixture devices_;
    xinput_test_support::focus_fixture focus_;
    xinput_test_support::metadata_source metadata_;
    std::atomic<std::int64_t> clock_ticks_{0};
    xinput::service xinput_;
    output::detail::composition composition_;
    output::detail::internal_producer producer_;
    application app_;
};

} // namespace inputnext::profiles_test_support
