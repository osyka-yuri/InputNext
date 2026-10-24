#pragma once

#include "model.hpp"
#include "output/composition/internal_producer.hpp"
#include <inputnext/output.hpp>
#include <inputnext/xinput.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace inputnext::profiles {

enum class application_operation : std::uint8_t { validate, open, submit, close, acknowledge };

struct application_failure final {
    application_operation operation{};
    output::error code{};
    xinput::assignment assignment{};
    output::internal_context context{};
};

struct application_report final {
    std::size_t pending_contexts{};
    std::optional<application_failure> failure;
    bool terminal{};
};

// Control calls require external serialization. Quiesce callers before destruction and keep
// the borrowed presentation and producer's composition alive until cleanup returns.
class application final {
public:
    application(output::detail::internal_producer producer, xinput::service& presentation);
    ~application() noexcept;

    application(const application&) = delete;
    application& operator=(const application&) = delete;
    application(application&&) = delete;
    application& operator=(application&&) = delete;

    [[nodiscard]] application_report replace_profile(const profile& desired);
    [[nodiscard]] application_report maintain();
    // Terminal and idempotent; never reads assignments or admits another context.
    [[nodiscard]] application_report close();

private:
    struct obligation final {
        output::internal_context context{};
        std::optional<output::close_receipt> receipt;
        bool published{};
    };

    struct position final {
        std::optional<xinput::assignment> assignment;
        std::optional<obligation> current;
        std::optional<obligation> retiring;
    };

    [[nodiscard]] application_report finish(application_report report = {}) const noexcept;
    void record_failure(application_report& report,
        application_operation operation,
        output::error error,
        const xinput::assignment& assignment,
        output::internal_context context = {}) noexcept;
    [[nodiscard]] bool retire(std::optional<obligation>& owned,
        const xinput::assignment& assignment,
        application_report& report);
    [[nodiscard]] bool drain(position& entry, application_report& report);
    void drain_all(application_report& report);
    void reconcile(position& entry,
        const std::optional<xinput::assignment>& desired,
        application_report& report);
    void publish(position& entry, application_report& report);

    output::detail::internal_producer producer_;
    xinput::service& presentation_;
    profile desired_;
    std::array<position, 4U> positions_;
    bool replacement_pending_{};
    bool closed_{};
    std::optional<application_failure> terminal_failure_;
};

} // namespace inputnext::profiles
