#include "ownership.hpp"
#include "actor.hpp"
#include "lease.hpp"
#include "lease_state.hpp"

#include "core/core.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/system_input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <inputnext/focus.hpp>

namespace inputnext::dinput {
namespace {

inline constexpr std::size_t actor_capacity = 16U;
inline constexpr DWORD shutdown_warning_ms = 5'000U;

void join_worker_for_shutdown(std::thread& worker) noexcept {
    if (!worker.joinable()) {
        return;
    }
    const HANDLE handle = worker.native_handle();
    if (WaitForSingleObject(handle, shutdown_warning_ms) == WAIT_TIMEOUT) {
        std::array<char, 160U> message{};
        const auto length = std::snprintf(message.data(),
            message.size(),
            "InputNext: shutdown is still waiting for DirectInput exclusive worker %lu "
            "after %lu ms.\n",
            GetThreadId(handle),
            shutdown_warning_ms);
        if (length > 0) {
            OutputDebugStringA(message.data());
        }
    }
    worker.join();
}

[[nodiscard]] HRESULT last_error_result() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

[[nodiscard]] std::optional<std::size_t> bounded_path_length(
    const DIPROPGUIDANDPATH& path) noexcept {
    const auto& value = path.wszPath;
    constexpr std::size_t capacity = std::size(value);
    for (std::size_t index = 0U; index < capacity; ++index) {
        if (value[index] == L'\0') {
            return index == 0U ? std::nullopt : std::optional<std::size_t>{index};
        }
    }
    return std::nullopt;
}

[[nodiscard]] HRESULT validate_request(const exclusive_request& request) noexcept {
    const bool window_valid =
        request.background_window.handle != nullptr
            ? request.window == focus::window_epoch{} &&
                  request.background_window.owner_thread != 0U
            : request.background_window.owner_thread == 0U && request.window.window != 0U &&
                  request.window.revision != 0U && request.window.process.revision != 0U &&
                  request.window.process.observer_incarnation.valid() &&
                  request.window.process.process.pid != 0U;
    if (!request.target.valid() || IsEqualGUID(request.native_instance, GUID_NULL) != 0 ||
        !window_valid) {
        return DIERR_INPUTLOST;
    }
    return detail::valid_exclusive_path(request.native_path) && request.factory.supported()
               ? DI_OK
               : DIERR_UNSUPPORTED;
}

[[nodiscard]] bool same_identity(const exclusive_request& left,
    const exclusive_request& right) noexcept {
    return IsEqualGUID(left.native_instance, right.native_instance) != 0 ||
           detail::same_exclusive_path(left.native_path, right.native_path);
}

} // namespace

bool detail::valid_exclusive_path(const DIPROPGUIDANDPATH& path) noexcept {
    return path.diph.dwSize == static_cast<DWORD>(sizeof(DIPROPGUIDANDPATH)) &&
           path.diph.dwHeaderSize == static_cast<DWORD>(sizeof(DIPROPHEADER)) &&
           path.diph.dwHow == DIPH_DEVICE && path.diph.dwObj == 0U &&
           bounded_path_length(path).has_value();
}

bool detail::same_exclusive_path(const DIPROPGUIDANDPATH& left,
    const DIPROPGUIDANDPATH& right) noexcept {
    const auto left_length = bounded_path_length(left);
    const auto right_length = bounded_path_length(right);
    if (!left_length.has_value() || !right_length.has_value() ||
        *left_length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        *right_length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left.wszPath,
               static_cast<int>(*left_length),
               right.wszPath,
               static_cast<int>(*right_length),
               TRUE) == CSTR_EQUAL;
}

bool detail::exclusive_target_eligible(const input_access::call& call,
    const connection_target target,
    const focus::window_epoch& window) noexcept {
    if (!call) {
        return false;
    }
    const auto latest = call.core().read_latest(target);
    if (!latest.has_value() && latest.error() != core::status::no_data) {
        return false;
    }
    if (latest.has_value() && latest->target != target) {
        return false;
    }
    if (window == focus::window_epoch{}) {
        return true;
    }
    const auto presentation = call.focus().lock_presentation();
    return presentation.matches_foreground_window_epoch(window);
}

bool native_input_factory::supported() const noexcept {
    if (create == nullptr || instance == nullptr) {
        return false;
    }
    if (create == &create_system_input) {
        return context == nullptr;
    }
    return lifetime != nullptr;
}

struct exclusive_input::implementation final {
    enum class slot_phase : std::uint8_t { free, reserving, joinable, joining };

    struct slot final {
        slot_phase phase{slot_phase::free};
        exclusive_request request{};
        std::shared_ptr<exclusive_lease::state> receipt;
        std::thread worker;
    };

    struct receipt_bundle final {
        std::shared_ptr<exclusive_lease::state> receipt;
        std::shared_ptr<exclusive_lease> lease;
    };

    explicit implementation(input_access& access_value) noexcept : access(access_value) {}

    [[nodiscard]] std::expected<std::shared_ptr<exclusive_lease>, HRESULT> reserve(
        exclusive_request request,
        proof_clock now,
        std::shared_ptr<const void> context) noexcept;
    [[nodiscard]] HRESULT reserve_slot(exclusive_request&& request,
        std::size_t& reserved_index) noexcept;
    [[nodiscard]] static std::expected<receipt_bundle, HRESULT> make_receipt(
        connection_target target,
        std::optional<focus::window_epoch> window,
        proof_clock now,
        std::shared_ptr<const void> context) noexcept;
    [[nodiscard]] HRESULT launch_actor(std::size_t reserved_index,
        const std::shared_ptr<exclusive_lease::state>& receipt) noexcept;
    void dispose_slot(std::size_t index) noexcept;
    void close_admission() noexcept;
    void drain() noexcept;
    void refresh_eligibility(const input_access::call& call) noexcept;
    void reap_completed() noexcept;

    input_access& access;
    std::mutex gate;
    std::condition_variable changed;
    std::mutex reaper_gate;
    std::mutex drain_gate;
    std::array<slot, actor_capacity> slots{};
    std::atomic<bool> admission_closed;
    bool drained{};
};

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::implementation::reserve(
    exclusive_request request,
    const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected{DIERR_INPUTLOST};
    }
    if (native_control_reentrant()) {
        return std::unexpected{HRESULT_FROM_WIN32(ERROR_BUSY)};
    }
    HRESULT status = validate_request(request);
    if (FAILED(status)) {
        return std::unexpected{status};
    }

    reap_completed();
    std::size_t reserved_index = actor_capacity;
    const auto target = request.target;
    const auto window =
        request.background_window.handle == nullptr ? std::optional{request.window} : std::nullopt;
    status = reserve_slot(std::move(request), reserved_index);
    if (FAILED(status)) {
        return std::unexpected{status};
    }

    auto created = make_receipt(target, window, now, std::move(context));
    if (!created.has_value()) {
        dispose_slot(reserved_index);
        return std::unexpected{created.error()};
    }

    status = launch_actor(reserved_index, created->receipt);
    if (FAILED(status)) {
        dispose_slot(reserved_index);
        return std::unexpected{status};
    }
    return created->lease;
}

HRESULT exclusive_input::implementation::reserve_slot(exclusive_request&& request,
    std::size_t& reserved_index) noexcept {
    std::scoped_lock lock{gate};
    if (admission_closed.load(std::memory_order_acquire)) {
        return DIERR_INPUTLOST;
    }
    for (const auto& current : slots) {
        if (current.phase != slot_phase::free && same_identity(current.request, request)) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
    }
    for (std::size_t index = 0U; index < slots.size(); ++index) {
        if (slots[index].phase == slot_phase::free) {
            reserved_index = index;
            slots[index].phase = slot_phase::reserving;
            slots[index].request = std::move(request);
            return DI_OK;
        }
    }
    return DIERR_OUTOFMEMORY;
}

std::expected<exclusive_input::implementation::receipt_bundle, HRESULT>
exclusive_input::implementation::make_receipt(const connection_target target,
    std::optional<focus::window_epoch> window,
    const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    try {
        auto receipt =
            std::make_shared<exclusive_lease::state>(now, std::move(context), target, window);
        receipt->start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->start_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->prepared_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->prepared_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->cancellation_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->cancellation_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        receipt->closed_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (receipt->closed_event == nullptr) {
            return std::unexpected{last_error_result()};
        }
        auto* raw_lease = new (std::nothrow) exclusive_lease{receipt};
        if (raw_lease == nullptr) {
            return std::unexpected{DIERR_OUTOFMEMORY};
        }
        return receipt_bundle{.receipt = std::move(receipt),
            .lease = std::shared_ptr<exclusive_lease>{raw_lease}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{DIERR_OUTOFMEMORY};
    } catch (...) {
        return std::unexpected{DIERR_GENERIC};
    }
}

HRESULT exclusive_input::implementation::launch_actor(const std::size_t reserved_index,
    const std::shared_ptr<exclusive_lease::state>& receipt) noexcept {
    if (admission_closed.load(std::memory_order_acquire)) {
        return DIERR_INPUTLOST;
    }
    try {
        std::scoped_lock lock{gate};
        auto& reserved = slots[reserved_index];
        if (admission_closed.load(std::memory_order_acquire) ||
            reserved.phase != slot_phase::reserving) {
            return DIERR_INPUTLOST;
        }
        reserved.receipt = receipt;
        // The slot retains the request until its worker has joined.
        reserved.worker = std::thread{[this, reserved_index, receipt] {
            actor{.access = access,
                .admission_closed = admission_closed,
                .request = slots[reserved_index].request,
                .receipt = *receipt}
                .run();
        }};
        reserved.phase = slot_phase::joinable;
        return DI_OK;
    } catch (const std::bad_alloc&) {
        return DIERR_OUTOFMEMORY;
    } catch (const std::system_error& error) {
        const auto& code = error.code();
        if (code == std::errc::resource_unavailable_try_again ||
            code == std::errc::not_enough_memory) {
            return DIERR_OUTOFMEMORY;
        }
        if (code.category() == std::system_category() && code.value() > 0) {
            return HRESULT_FROM_WIN32(static_cast<DWORD>(code.value()));
        }
        return DIERR_GENERIC;
    } catch (...) {
        return DIERR_GENERIC;
    }
}

void exclusive_input::implementation::dispose_slot(const std::size_t index) noexcept {
    // A reservation or join claim keeps the identity occupied through provider disposal.
    auto& current = slots[index];
    std::shared_ptr<void> lifetime;
    {
        std::scoped_lock lock{gate};
        lifetime = std::move(current.request.factory.lifetime);
    }
    lifetime.reset();
    {
        std::scoped_lock lock{gate};
        current.request = {};
        current.receipt.reset();
        current.phase = slot_phase::free;
    }
    changed.notify_all();
}

void exclusive_input::implementation::refresh_eligibility(const input_access::call& call) noexcept {
    std::array<std::shared_ptr<exclusive_lease::state>, actor_capacity> receipts{};
    std::array<connection_target, actor_capacity> targets{};
    std::array<focus::window_epoch, actor_capacity> windows{};
    std::size_t count{};
    {
        std::scoped_lock lock{gate};
        for (const auto& current : slots) {
            if (current.phase != slot_phase::free && current.receipt != nullptr) {
                receipts[count] = current.receipt;
                targets[count] = current.request.target;
                windows[count] = current.request.window;
                ++count;
            }
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (receipts[index]->cancelled() ||
            !detail::exclusive_target_eligible(call, targets[index], windows[index])) {
            receipts[index]->request_cancel();
        }
    }
}

void exclusive_input::implementation::close_admission() noexcept {
    admission_closed.store(true, std::memory_order_release);
    std::array<std::shared_ptr<exclusive_lease::state>, actor_capacity> receipts{};
    std::size_t count{};
    {
        std::scoped_lock lock{gate};
        for (const auto& current : slots) {
            if (current.phase != slot_phase::free && current.receipt != nullptr) {
                receipts[count++] = current.receipt;
            }
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        receipts[index]->request_cancel();
    }
    changed.notify_all();
}

void exclusive_input::implementation::reap_completed() noexcept {
    for (;;) {
        std::size_t index = actor_capacity;
        {
            std::scoped_lock reaper_lock{reaper_gate};
            {
                std::scoped_lock lock{gate};
                for (std::size_t candidate = 0U; candidate < slots.size(); ++candidate) {
                    auto& current = slots[candidate];
                    if (current.phase == slot_phase::joinable && current.receipt->is_closed()) {
                        index = candidate;
                        current.phase = slot_phase::joining;
                        break;
                    }
                }
            }
            if (index == actor_capacity) {
                return;
            }
            slots[index].worker.join();
        }
        dispose_slot(index);
    }
}

void exclusive_input::implementation::drain() noexcept {
    close_admission();
    std::scoped_lock drain_lock{drain_gate};
    if (drained) {
        return;
    }
    for (;;) {
        std::size_t index = actor_capacity;
        bool joining_elsewhere{};
        {
            std::scoped_lock reaper_lock{reaper_gate};
            {
                std::unique_lock lock{gate};
                changed.wait(lock, [this] {
                    return std::ranges::none_of(slots,
                        [](const slot& current) { return current.phase == slot_phase::reserving; });
                });
                joining_elsewhere = std::ranges::any_of(slots,
                    [](const slot& current) { return current.phase == slot_phase::joining; });
                if (!joining_elsewhere) {
                    for (std::size_t candidate = 0U; candidate < slots.size(); ++candidate) {
                        if (slots[candidate].phase != slot_phase::free) {
                            index = candidate;
                            slots[candidate].phase = slot_phase::joining;
                            break;
                        }
                    }
                }
            }
            if (!joining_elsewhere && index != actor_capacity) {
                join_worker_for_shutdown(slots[index].worker);
            }
        }

        if (joining_elsewhere) {
            std::unique_lock wait_lock{gate};
            changed.wait(wait_lock, [this] {
                return std::ranges::none_of(slots,
                    [](const slot& current) { return current.phase == slot_phase::joining; });
            });
            continue;
        }
        if (index == actor_capacity) {
            drained = true;
            return;
        }
        dispose_slot(index);
    }
}

exclusive_input::exclusive_input(input_access& access)
    : implementation_(std::make_unique<implementation>(access)) {
}

exclusive_input::~exclusive_input() {
    drain();
}

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::reserve(
    exclusive_request request) noexcept {
    return reserve(std::move(request), nullptr, nullptr);
}

std::expected<std::shared_ptr<exclusive_lease>, HRESULT> exclusive_input::reserve(
    exclusive_request request,
    const proof_clock now,
    std::shared_ptr<const void> context) noexcept {
    return implementation_->reserve(std::move(request), now, std::move(context));
}

void exclusive_input::refresh_eligibility(const input_access::call& call) noexcept {
    implementation_->refresh_eligibility(call);
}

void exclusive_input::close_admission() noexcept {
    implementation_->close_admission();
}

void exclusive_input::drain() noexcept {
    implementation_->drain();
}

} // namespace inputnext::dinput
