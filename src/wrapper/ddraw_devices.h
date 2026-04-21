#pragma once

// Direct3D 7 device enumeration via ddraw.dll (loaded dynamically).
// Used by --list-devices and --device wrapper options.

#if defined(_WIN32)

#include <windows.h>

#include <string>
#include <vector>

namespace twinshim {

struct D3DDeviceInfo {
  std::wstring name;
  std::wstring description; // driver-provided description from D3D7 EnumDevices
  GUID deviceGuid;
};

// Enumerate D3D7 devices by dynamically loading ddraw.dll and walking
// IDirectDraw7 -> IDirect3D7 -> EnumDevices.
// Returns an empty vector when ddraw.dll is unavailable or D3D7 is not supported.
std::vector<D3DDeviceInfo> EnumerateD3DDevices();

// Format a GUID as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
std::wstring FormatGuid(const GUID& guid);

// Format a GUID's raw bytes as a comma-separated hex string matching .reg
// hex: notation (e.g. "78,9e,04,f5,61,48,d2,11,a4,07,00,a0,c9,06,29,a8").
std::wstring FormatGuidAsRegHex(const GUID& guid);

// Pick the most appropriate device from the enumerated list.
// Preference order: hardware (non-RGB, non-Reference) > first available.
// Returns nullptr when the list is empty.
const D3DDeviceInfo* SelectBestDevice(const std::vector<D3DDeviceInfo>& devices);

} // namespace twinshim

#endif // _WIN32
