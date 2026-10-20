#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput_events.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>

namespace inputnext::xinput_test_support {

class controlled_event_source final : public xinput::event_source {
public:
    [[nodiscard]] history_result<history_reader_handle> open_history(std::uint32_t position,
        connection_target target,
        history_time /*now*/) noexcept override {
        std::unique_lock lock{mutex_};
        ++open_calls_;
        if (block_open_) {
            block_open_ = false;
            open_entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return open_released_; });
        }
        if (position >= readers_.size()) {
            return std::unexpected{history_failure{.code = history_error::invalid_argument}};
        }
        if (open_failure_.has_value()) {
            const auto failure = *open_failure_;
            open_failure_.reset();
            return std::unexpected{failure};
        }
        if (readers_[position].open) {
            return std::unexpected{history_failure{.code = history_error::busy}};
        }
        const history_reader_handle handle{.target = target, .value = next_handle_++};
        readers_[position] = reader_record{.open = true, .handle = handle};
        return handle;
    }

    [[nodiscard]] history_batch read_history(history_reader_handle reader,
        history_time /*now*/) noexcept override {
        std::unique_lock lock{mutex_};
        ++read_calls_;
        if (block_read_) {
            block_read_ = false;
            read_entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return read_released_; });
        }
        const auto position = find_reader(reader);
        if (!position.has_value()) {
            return failed_batch(reader, history_error::invalid_reader);
        }
        auto& pending = batches_[*position];
        if (pending.empty()) {
            return failed_batch(reader, history_error::no_data, streams_[*position]);
        }
        auto batch = pending.front();
        pending.pop_front();
        return batch;
    }

    [[nodiscard]] history_result<void> close_history(
        history_reader_handle reader) noexcept override {
        std::unique_lock lock{mutex_};
        ++close_calls_;
        last_closed_ = reader;
        if (block_close_) {
            block_close_ = false;
            close_entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return close_released_; });
        }
        const auto position = find_reader(reader);
        if (!position.has_value()) {
            return std::unexpected{history_failure{.code = history_error::invalid_reader}};
        }
        if (failed_close_attempts_ > 0U) {
            --failed_close_attempts_;
            return std::unexpected{history_failure{.code = history_error::provider_failure}};
        }
        readers_[*position] = {};
        return {};
    }

    void enqueue(std::uint32_t position, history_batch batch) {
        std::scoped_lock lock{mutex_};
        if (position < batches_.size()) {
            streams_[position] = batch.domain.stream;
            batches_[position].push_back(batch);
        }
    }

    void fail_next_open(history_failure failure) {
        std::scoped_lock lock{mutex_};
        open_failure_ = failure;
    }

    void fail_next_close() noexcept { fail_close_attempts(1U); }

    void fail_close_attempts(std::size_t count) noexcept {
        std::scoped_lock lock{mutex_};
        failed_close_attempts_ = count;
    }

    void block_next_open() noexcept {
        std::scoped_lock lock{mutex_};
        block_open_ = true;
        open_entered_ = false;
        open_released_ = false;
    }

    void block_next_read() noexcept {
        std::scoped_lock lock{mutex_};
        block_read_ = true;
        read_entered_ = false;
        read_released_ = false;
    }

    void block_next_close() noexcept {
        std::scoped_lock lock{mutex_};
        block_close_ = true;
        close_entered_ = false;
        close_released_ = false;
    }

    [[nodiscard]] bool wait_for_open(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return open_entered_; });
    }

    [[nodiscard]] bool wait_for_read(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return read_entered_; });
    }

    [[nodiscard]] bool wait_for_close(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return close_entered_; });
    }

    void release_open() noexcept {
        std::scoped_lock lock{mutex_};
        open_released_ = true;
        changed_.notify_all();
    }

    void release_read() noexcept {
        std::scoped_lock lock{mutex_};
        read_released_ = true;
        changed_.notify_all();
    }

    void release_close() noexcept {
        std::scoped_lock lock{mutex_};
        close_released_ = true;
        changed_.notify_all();
    }

    void release_all() noexcept {
        std::scoped_lock lock{mutex_};
        open_released_ = true;
        read_released_ = true;
        close_released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t open_calls() const noexcept {
        std::scoped_lock lock{mutex_};
        return open_calls_;
    }

    [[nodiscard]] std::size_t read_calls() const noexcept {
        std::scoped_lock lock{mutex_};
        return read_calls_;
    }

    [[nodiscard]] std::size_t close_calls() const noexcept {
        std::scoped_lock lock{mutex_};
        return close_calls_;
    }

    [[nodiscard]] std::optional<history_reader_handle> last_closed() const noexcept {
        std::scoped_lock lock{mutex_};
        return last_closed_;
    }

    [[nodiscard]] bool is_open(std::uint32_t position) const noexcept {
        std::scoped_lock lock{mutex_};
        return position < readers_.size() && readers_[position].open;
    }

private:
    struct reader_record final {
        bool open{};
        history_reader_handle handle{};
    };

    [[nodiscard]] std::optional<std::size_t> find_reader(
        history_reader_handle handle) const noexcept {
        for (std::size_t index = 0U; index < readers_.size(); ++index) {
            if (readers_[index].open && readers_[index].handle == handle) {
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static history_batch failed_batch(history_reader_handle handle,
        history_error error,
        std::uint64_t stream = 0U) noexcept {
        history_batch result{};
        result.domain = history_domain{.target = handle.target, .stream = stream};
        result.failure = history_failure{.code = error};
        return result;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<reader_record, 4U> readers_{};
    std::array<std::deque<history_batch>, 4U> batches_{};
    std::array<std::uint64_t, 4U> streams_{};
    std::optional<history_failure> open_failure_;
    std::optional<history_reader_handle> last_closed_;
    std::uint64_t next_handle_{1U};
    std::size_t open_calls_{};
    std::size_t read_calls_{};
    std::size_t close_calls_{};
    bool block_open_{};
    bool open_entered_{};
    bool open_released_{};
    bool block_read_{};
    bool read_entered_{};
    bool read_released_{};
    bool block_close_{};
    bool close_entered_{};
    bool close_released_{};
    std::size_t failed_close_attempts_{};
};

class release_event_source_on_exit final {
public:
    explicit release_event_source_on_exit(controlled_event_source& source) noexcept
        : source_(source) {}

    ~release_event_source_on_exit() { source_.release_all(); }

    release_event_source_on_exit(const release_event_source_on_exit&) = delete;
    release_event_source_on_exit& operator=(const release_event_source_on_exit&) = delete;
    release_event_source_on_exit(release_event_source_on_exit&&) = delete;
    release_event_source_on_exit& operator=(release_event_source_on_exit&&) = delete;

private:
    controlled_event_source& source_;
};

} // namespace inputnext::xinput_test_support
