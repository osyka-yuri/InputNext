#pragma once

#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/xinput.hpp>

#include <cstdint>

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/error.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput_events.hpp>

namespace inputnext {

// Binds GameInput's live connection inventory to the four XInput presentation positions.
// Assignment tokens are owned by this object; callers must not pass them to another instance.
// The runtime and focus observer must outlive this object and its service callers.
class gameinput_xinput final : private xinput::metadata_source, private xinput::event_source {
public:
    gameinput_xinput(gameinput_runtime& runtime, focus::observer& focus);
    // Uses the source history clock frequency, provided by deterministic composition fixtures.
    gameinput_xinput(gameinput_runtime& runtime,
        focus::observer& focus,
        std::uint64_t qpc_frequency);
    ~gameinput_xinput() override;

    gameinput_xinput(const gameinput_xinput&) = delete;
    gameinput_xinput& operator=(const gameinput_xinput&) = delete;
    gameinput_xinput(gameinput_xinput&&) = delete;
    gameinput_xinput& operator=(gameinput_xinput&&) = delete;

    [[nodiscard]] result<void> synchronize(const connection_inventory& inventory);
    void maintain_events(history_time now);
    [[nodiscard]] xinput::service& presentation() noexcept;
    void close();

private:
    [[nodiscard]] metadata_result<metadata_snapshot> query(connection_target target) override;
    [[nodiscard]] history_result<history_reader_handle> open_history(std::uint32_t position,
        connection_target target,
        history_time now) noexcept override;
    [[nodiscard]] history_batch read_history(history_reader_handle reader,
        history_time now) noexcept override;
    [[nodiscard]] history_result<void> close_history(
        history_reader_handle reader) noexcept override;

    gameinput_runtime& runtime_;
    xinput::service presentation_;
};

} // namespace inputnext
