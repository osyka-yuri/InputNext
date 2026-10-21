#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include "output/authority/state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>

TEST_CASE("output.focus_pair_requires_one_immutable_epoch_for_every_bound_record",
    "[unit][output]") {
    const inputnext::runtime_id observer{.bytes = {std::byte{0x66U}}};
    const inputnext::focus::process_instance process{.pid = 66U, .creation_time_100ns = 791U};
    const inputnext::focus::foreground_epoch epoch_one{.observer_incarnation = observer,
        .process = process,
        .revision = 4U};
    const inputnext::focus::foreground_epoch epoch_two{.observer_incarnation = observer,
        .process = process,
        .revision = 6U};
    const std::array opening_epochs{std::optional{epoch_one}, std::optional{epoch_one}};

    CHECK(
        inputnext::output::detail::foreground_pair_qualifies(epoch_one, epoch_one, opening_epochs));
    CHECK_FALSE(
        inputnext::output::detail::foreground_pair_qualifies(epoch_one, epoch_two, opening_epochs));
    CHECK_FALSE(
        inputnext::output::detail::foreground_pair_qualifies(epoch_two, epoch_two, opening_epochs));
    CHECK_FALSE(inputnext::output::detail::foreground_pair_qualifies(std::nullopt,
        epoch_one,
        opening_epochs));
}
