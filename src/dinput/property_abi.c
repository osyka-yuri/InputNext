#include "property_abi.h"

HRESULT inputnext_dinput_get_property_w(IDirectInputDevice8W* device,
    const GUID* property,
    DIPROPHEADER* header) {
    return device->lpVtbl->GetProperty(device, property, header);
}

HRESULT inputnext_dinput_set_property_w(IDirectInputDevice8W* device,
    const GUID* property,
    const DIPROPHEADER* header) {
    return device->lpVtbl->SetProperty(device, property, header);
}
