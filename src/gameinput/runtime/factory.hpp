#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "metadata/provider.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <cstdint>
#include <memory>

namespace inputnext::gameinput_detail {

struct facts_generation_seed final {
    std::uint64_t requested{};
    std::uint64_t installed{};
};

class runtime_factory final {
public:
    [[nodiscard]] static std::unique_ptr<gameinput_runtime> create(device_registry& registry,
        gameinput_options options,
        std::unique_ptr<native_gameinput> native,
        std::unique_ptr<metadata_provider> metadata);
    [[nodiscard]] static bool seed_facts_generation(gameinput_runtime& runtime,
        connection_target target,
        facts_generation_seed seed);
};

} // namespace inputnext::gameinput_detail
