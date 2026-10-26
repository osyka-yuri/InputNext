#pragma once

#include "audio/native_port.hpp"

#include <memory>
#include <new>

namespace inputnext::test::audio {

class unavailable_native_port_factory final : public audio_detail::native_port_factory {
public:
    [[nodiscard]] audio_detail::native_result<std::unique_ptr<audio_detail::native_port>>
    create() noexcept override {
        return std::unexpected{audio_detail::native_error{
            .operation = audio_detail::native_operation::create_native_port,
            .native_code = 0U}};
    }
};

[[nodiscard]] inline std::unique_ptr<audio_detail::native_port_factory>
make_unavailable_native_port_factory() noexcept {
    return std::unique_ptr<audio_detail::native_port_factory>{
        new (std::nothrow) unavailable_native_port_factory{}};
}

} // namespace inputnext::test::audio
