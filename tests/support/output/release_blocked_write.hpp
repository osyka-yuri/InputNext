#pragma once

#include "output/controlled_native_sink.hpp"

#include <memory>
#include <utility>

namespace inputnext::test::output {

class release_blocked_write final {
public:
    explicit release_blocked_write(std::shared_ptr<controlled_native_state> state) noexcept
        : state_(std::move(state)) {}

    ~release_blocked_write() noexcept { state_->release_write(); }

    release_blocked_write(const release_blocked_write&) = delete;
    release_blocked_write& operator=(const release_blocked_write&) = delete;
    release_blocked_write(release_blocked_write&&) = delete;
    release_blocked_write& operator=(release_blocked_write&&) = delete;

private:
    std::shared_ptr<controlled_native_state> state_;
};

} // namespace inputnext::test::output
