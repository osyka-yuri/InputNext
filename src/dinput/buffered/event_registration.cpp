#include "event_registration.hpp"

#include <dinput.h>
#include <winternl.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>

namespace inputnext::dinput {
namespace {

[[nodiscard]] HRESULT last_error_result() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : error);
}

[[nodiscard]] HRESULT validate_event_handle(HANDLE event) noexcept {
    auto* const module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
        return DIERR_UNSUPPORTED;
    }
    const auto query =
        reinterpret_cast<decltype(&NtQueryObject)>(GetProcAddress(module, "NtQueryObject"));
    if (query == nullptr) {
        return DIERR_UNSUPPORTED;
    }
    alignas(PUBLIC_OBJECT_TYPE_INFORMATION) std::array<std::byte, 1024U> storage{};
    ULONG returned{};
    if (query(event,
            ObjectTypeInformation,
            storage.data(),
            static_cast<ULONG>(storage.size()),
            &returned) < 0 ||
        returned < sizeof(PUBLIC_OBJECT_TYPE_INFORMATION)) {
        return E_HANDLE;
    }
    PUBLIC_OBJECT_TYPE_INFORMATION information{};
    std::memcpy(&information, storage.data(), sizeof(information));
    if (information.TypeName.Buffer == nullptr ||
        information.TypeName.Length % sizeof(wchar_t) != 0U) {
        return E_HANDLE;
    }
    const std::wstring_view name{information.TypeName.Buffer,
        information.TypeName.Length / sizeof(wchar_t)};
    return name == L"Event" ? DI_OK : E_HANDLE;
}

} // namespace

std::expected<std::shared_ptr<event_registration>, HRESULT> event_registration::duplicate(
    HANDLE event) {
    if (event == nullptr) {
        return std::shared_ptr<event_registration>{};
    }
    if (event == INVALID_HANDLE_VALUE || event == GetCurrentProcess() ||
        event == GetCurrentThread()) {
        return std::unexpected(E_HANDLE);
    }

    HANDLE duplicate{};
    if (DuplicateHandle(GetCurrentProcess(),
            event,
            GetCurrentProcess(),
            &duplicate,
            EVENT_MODIFY_STATE,
            FALSE,
            0U) == FALSE) {
        return std::unexpected(last_error_result());
    }

    const auto validated = validate_event_handle(duplicate);
    if (FAILED(validated)) {
        CloseHandle(duplicate);
        return std::unexpected(validated);
    }

    event_registration* registration{};
    try {
        registration = new event_registration{duplicate};
    } catch (const std::bad_alloc&) {
        CloseHandle(duplicate);
        return std::unexpected(DIERR_OUTOFMEMORY);
    } catch (...) {
        CloseHandle(duplicate);
        return std::unexpected(DIERR_GENERIC);
    }
    try {
        return std::shared_ptr<event_registration>{registration};
    } catch (const std::bad_alloc&) {
        // The shared_ptr constructor deletes its raw pointer on allocation failure.
        return std::unexpected(DIERR_OUTOFMEMORY);
    } catch (...) {
        return std::unexpected(DIERR_GENERIC);
    }
}

event_registration::~event_registration() {
    disable();
}

void event_registration::signal() noexcept {
    std::scoped_lock lock{signal_gate_};
    if (!disabled_ && event_ != nullptr) {
        static_cast<void>(SetEvent(event_));
    }
}

void event_registration::disable() noexcept {
    std::scoped_lock lock{signal_gate_};
    if (disabled_) {
        return;
    }
    disabled_ = true;
    if (event_ != nullptr) {
        static_cast<void>(CloseHandle(event_));
        event_ = nullptr;
    }
}

} // namespace inputnext::dinput
