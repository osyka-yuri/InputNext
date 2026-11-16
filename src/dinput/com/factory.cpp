#include "factory.hpp"

#include "dinput/native_types.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace inputnext::dinput {
namespace {

std::atomic<std::uint32_t> live_objects{};
std::atomic<std::uint32_t> server_locks{};

class direct_input_class_factory final : public IClassFactory {
public:
    direct_input_class_factory(const direct_input_class_factory&) = delete;
    direct_input_class_factory& operator=(const direct_input_class_factory&) = delete;
    direct_input_class_factory(direct_input_class_factory&&) = delete;
    direct_input_class_factory& operator=(direct_input_class_factory&&) = delete;

    explicit direct_input_class_factory(host supplied_host) noexcept
        : host_(std::move(supplied_host)) {
        object_created();
    }

    ~direct_input_class_factory() { object_destroyed(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IClassFactory)) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
        REFIID iid,
        void** out) noexcept override {
        return detail::create_uninitialized_root(host_, outer, iid, out);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override {
        if (lock != FALSE) {
            const auto previous = server_locks.fetch_add(1U, std::memory_order_acq_rel);
            if (previous == std::numeric_limits<std::uint32_t>::max()) {
                server_locks.fetch_sub(1U, std::memory_order_release);
                return E_UNEXPECTED;
            }
            return S_OK;
        }

        auto observed = server_locks.load(std::memory_order_acquire);
        while (observed != 0U) {
            if (server_locks.compare_exchange_weak(observed,
                    observed - 1U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return S_OK;
            }
        }
        return E_UNEXPECTED;
    }

private:
    std::atomic<ULONG> references_{1U};
    host host_{};
};

} // namespace

// DllGetClassObject fixes the class-ID/interface-ID parameter order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
HRESULT get_class_object(const host& root_host, REFCLSID clsid, REFIID iid, void** out) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (!IsEqualCLSID(clsid, CLSID_DirectInput8)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) direct_input_class_factory{root_host};
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = factory->QueryInterface(iid, out);
    factory->Release();
    return status;
}

HRESULT can_unload() noexcept {
    return live_objects.load(std::memory_order_acquire) == 0U &&
                   server_locks.load(std::memory_order_acquire) == 0U
               ? S_OK
               : S_FALSE;
}

void object_created() noexcept {
    live_objects.fetch_add(1U, std::memory_order_relaxed);
}

void object_destroyed() noexcept {
    live_objects.fetch_sub(1U, std::memory_order_release);
}

} // namespace inputnext::dinput
