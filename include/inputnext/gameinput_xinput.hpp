#pragma once

#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/xinput.hpp>

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/error.hpp>

namespace inputnext {

// Binds GameInput's live connection inventory to the four XInput presentation positions.
// Assignment tokens are owned by this object; callers must not pass them to another instance.
class gameinput_xinput final : private xinput::metadata_source {
public:
    gameinput_xinput(gameinput_runtime& runtime, focus::observer& focus);
    ~gameinput_xinput() override;

    gameinput_xinput(const gameinput_xinput&) = delete;
    gameinput_xinput& operator=(const gameinput_xinput&) = delete;
    gameinput_xinput(gameinput_xinput&&) = delete;
    gameinput_xinput& operator=(gameinput_xinput&&) = delete;

    [[nodiscard]] result<void> synchronize(const connection_inventory& inventory);
    [[nodiscard]] xinput::service& presentation() noexcept;
    void close();

private:
    [[nodiscard]] metadata_result<metadata_snapshot> query(connection_target target) override;

    gameinput_runtime& runtime_;
    xinput::service presentation_;
};

} // namespace inputnext
