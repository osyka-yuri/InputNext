#pragma once

#include <Windows.h>
#include <dinput.h>

#ifdef __cplusplus
extern "C" {
#endif

// DirectInput's predefined properties are pointer-sized tokens. Call through
// the SDK's C interface so these tokens never bind to a C++ GUID reference.
HRESULT inputnext_dinput_get_property_w(IDirectInputDevice8W* device,
    const GUID* property,
    DIPROPHEADER* header);
HRESULT inputnext_dinput_set_property_w(IDirectInputDevice8W* device,
    const GUID* property,
    const DIPROPHEADER* header);

#ifdef __cplusplus
}
#endif
