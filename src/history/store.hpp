#pragma once

#include "cursor.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>

namespace inputnext::history_detail {

struct history_store_test_access;

class history_store final {
public:
    explicit history_store(history_provider& provider, identity_seed seed = {});
    ~history_store() noexcept;

    history_store(const history_store&) = delete;
    history_store& operator=(const history_store&) = delete;
    history_store(history_store&&) = delete;
    history_store& operator=(history_store&&) = delete;

    [[nodiscard]] history_result<history_reader_handle> open(connection_target target,
        history_time now);
    [[nodiscard]] history_result<history_reader_handle>
    open_xinput_reader(std::uint32_t position, connection_target target, history_time now);
    [[nodiscard]] history_batch read(history_reader_handle reader, history_time now);
    [[nodiscard]] history_result<void> close(history_reader_handle reader);

    void retire(connection_target target) noexcept;
    void close_all() noexcept;
    [[nodiscard]] std::size_t drain_retired() noexcept;
    [[nodiscard]] std::size_t resident_bank_count() const noexcept;

private:
    friend struct history_store_test_access;

    history_store_state state_;
};

} // namespace inputnext::history_detail
