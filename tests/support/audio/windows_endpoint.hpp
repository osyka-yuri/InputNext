#pragma once

#include <Windows.h>
#include <propidlbase.h>
#include <propkeydef.h>

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace inputnext::test::audio {

class controlled_property_store final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IPropertyStore> {
public:
    GUID container{.Data1 = 0x11223344U,
        .Data2 = 0x5566U,
        .Data3 = 0x7788U,
        .Data4 = {0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0U}};
    HRESULT value_result{S_OK};
    VARTYPE value_type{VT_CLSID};
    bool null_container{};
    std::size_t reads{};

    [[nodiscard]] ULONG references() const noexcept { return GetRefCount(); }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) noexcept override {
        *count = 0U;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetAt([[maybe_unused]] DWORD index,
        PROPERTYKEY* key) noexcept override {
        *key = {};
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetValue([[maybe_unused]] REFPROPERTYKEY key,
        [[maybe_unused]] REFPROPVARIANT incoming) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit() noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key,
        PROPVARIANT* property) noexcept override {
        ++reads;
        PropVariantInit(property);
        if (IsEqualGUID(key.fmtid, PKEY_Device_ContainerId.fmtid) == FALSE ||
            key.pid != PKEY_Device_ContainerId.pid) {
            return E_INVALIDARG;
        }
        property->vt = value_type;
        if (value_type == VT_CLSID && !null_container) {
            property->puuid = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
            if (property->puuid == nullptr) {
                return E_OUTOFMEMORY;
            }
            *property->puuid = container;
        }
        return value_result;
    }
};

class controlled_endpoint final : public Microsoft::WRL::RuntimeClass<
                                      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                      IMMDevice,
                                      IMMEndpoint> {
public:
    explicit controlled_endpoint(Microsoft::WRL::ComPtr<controlled_property_store> incoming)
        : properties(std::move(incoming)) {}

    Microsoft::WRL::ComPtr<controlled_property_store> properties;
    std::wstring id{L"endpoint-A"};
    DWORD state{DEVICE_STATE_ACTIVE};
    EDataFlow flow{eRender};
    HRESULT id_result{S_OK};
    HRESULT state_result{S_OK};
    HRESULT flow_result{S_OK};
    HRESULT store_result{S_OK};
    bool null_id{};
    bool null_store{};
    std::size_t store_opens{};
    ULONG references_at_store_open{};

    [[nodiscard]] ULONG references() const noexcept { return GetRefCount(); }

    HRESULT STDMETHODCALLTYPE Activate([[maybe_unused]] REFIID iid,
        [[maybe_unused]] DWORD context,
        [[maybe_unused]] PROPVARIANT* parameters,
        void** result) noexcept override {
        *result = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access,
        IPropertyStore** result) noexcept override {
        ++store_opens;
        references_at_store_open = GetRefCount();
        *result = nullptr;
        if (access != STGM_READ) {
            return E_INVALIDARG;
        }
        if (!null_store) {
            *result = properties.Get();
            (*result)->AddRef();
        }
        return store_result;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* result) noexcept override {
        *result = nullptr;
        if (!null_id) {
            const auto size = (id.size() + 1U) * sizeof(wchar_t);
            *result = static_cast<wchar_t*>(CoTaskMemAlloc(size));
            if (*result == nullptr) {
                return E_OUTOFMEMORY;
            }
            std::memcpy(*result, id.c_str(), size);
        }
        return id_result;
    }

    HRESULT STDMETHODCALLTYPE GetState(DWORD* result) noexcept override {
        *result = state;
        return state_result;
    }

    HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* result) noexcept override {
        *result = flow;
        return flow_result;
    }
};

} // namespace inputnext::test::audio
