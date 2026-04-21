#include "wrapper/ddraw_devices.h"

#if defined(_WIN32)

#include <objbase.h>

#include <cstdio>
#include <cstring>

namespace twinshim {
namespace {

// =========================================================================
// GUID constants (defined inline to avoid linking dxguid.lib)
// =========================================================================

// IID_IDirectDraw7 = {15E65EC0-3B9C-11D2-B92F-00609797EA5B}
static const GUID kIID_IDirectDraw7 =
    {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

// IID_IDirect3D7 = {F5049E77-4861-11D2-A407-00A0C90629A8}
static const GUID kIID_IDirect3D7 =
    {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

// Well-known D3D7 device type GUIDs (used by SelectBestDevice to classify
// software-only renderers that should be deprioritised).
//
// IID_IDirect3DRGBDevice = {A4665C60-2673-11CF-A31A-00AA00B93356}
static const GUID kGUID_RGBDevice =
    {0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};

// IID_IDirect3DRefDevice = {50936643-13E9-11D1-89AA-00A0C9054129}
static const GUID kGUID_RefDevice =
    {0x50936643, 0x13e9, 0x11d1, {0x89, 0xaa, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};

// =========================================================================
// Minimal D3D7 structure stubs (avoids a hard dependency on <d3d.h> which
// may not ship in every MinGW / Windows SDK configuration)
// =========================================================================

// D3DPRIMCAPS -- 14 DWORDs (56 bytes)
struct D3DPrimCaps_Stub {
  DWORD dwSize;
  DWORD dwMiscCaps;
  DWORD dwRasterCaps;
  DWORD dwZCmpCaps;
  DWORD dwSrcBlendCaps;
  DWORD dwDestBlendCaps;
  DWORD dwAlphaCmpCaps;
  DWORD dwShadeCaps;
  DWORD dwTextureCaps;
  DWORD dwTextureFilterCaps;
  DWORD dwTextureBlendCaps;
  DWORD dwTextureAddressCaps;
  DWORD dwStippleWidth;
  DWORD dwStippleHeight;
};

// D3DDEVICEDESC7
struct D3DDeviceDesc7_Stub {
  DWORD              dwDevCaps;
  D3DPrimCaps_Stub   dpcLineCaps;
  D3DPrimCaps_Stub   dpcTriCaps;
  DWORD              dwDeviceRenderBitDepth;
  DWORD              dwDeviceZBufferBitDepth;
  DWORD              dwMinTextureWidth;
  DWORD              dwMinTextureHeight;
  DWORD              dwMaxTextureWidth;
  DWORD              dwMaxTextureHeight;
  DWORD              dwMaxTextureRepeat;
  DWORD              dwMaxTextureAspectRatio;
  DWORD              dwMaxAnisotropy;
  float              dvMaxVertexW;
  GUID               deviceGUID;
  WORD               wMaxUserClipPlanes;
  WORD               wMaxVertexBlendMatrices;
  DWORD              dwVertexProcessingCaps;
};

// =========================================================================
// Function-pointer typedefs
// =========================================================================

// DirectDrawCreateEx loaded from ddraw.dll
typedef HRESULT(WINAPI* PFN_DirectDrawCreateEx)(
    GUID*     lpGuid,
    void**    lplpDD,
    REFIID    iid,
    IUnknown* pUnkOuter);

// IDirect3D7::EnumDevices callback
typedef HRESULT(CALLBACK* D3DEnumDevicesCallback7)(
    LPSTR                lpDeviceDescription,
    LPSTR                lpDeviceName,
    D3DDeviceDesc7_Stub* lpD3DDeviceDesc,
    LPVOID               lpContext);

// Vtable signature for IDirect3D7::EnumDevices (slot 3)
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D7_EnumDevices)(
    IUnknown*               pThis,
    D3DEnumDevicesCallback7 callback,
    LPVOID                  context);

// =========================================================================
// Helpers
// =========================================================================

static std::wstring AnsiToWide(const char* ansi) {
  if (!ansi || !*ansi) {
    return {};
  }
  int needed = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_ACP, 0, ansi, -1, out.data(), needed);
  return out;
}

// =========================================================================
// EnumDevices callback
// =========================================================================

// Strip capability qualifiers like "T&L" from device names so that e.g.
// "Direct3D T&L HAL" becomes "Direct3D HAL".
static std::wstring SimplifyDeviceName(const std::wstring& name) {
  std::wstring out = name;

  // Remove common capability prefixes/infixes.
  const wchar_t* patterns[] = {L"T&L ", L"TnL ", L"T&L", L"TnL"};
  for (const auto* pat : patterns) {
    size_t pos = out.find(pat);
    if (pos != std::wstring::npos) {
      out.erase(pos, wcslen(pat));
    }
  }

  // Collapse any resulting double spaces.
  size_t pos;
  while ((pos = out.find(L"  ")) != std::wstring::npos) {
    out.erase(pos, 1);
  }

  return out;
}

static HRESULT CALLBACK DeviceEnumCallback(
    LPSTR                lpDeviceDescription,
    LPSTR                lpDeviceName,
    D3DDeviceDesc7_Stub* lpD3DDeviceDesc,
    LPVOID               lpContext) {
  auto* devices = static_cast<std::vector<D3DDeviceInfo>*>(lpContext);

  D3DDeviceInfo info;
  info.description = AnsiToWide(lpDeviceDescription);

  // Build a display name that includes the driver description when it adds
  // useful information beyond the generic device-type name (e.g. "Direct3D
  // HAL (NVIDIA GeForce RTX 3080)" instead of just "Direct3D HAL").
  std::wstring simpleName = SimplifyDeviceName(AnsiToWide(lpDeviceName));
  if (!info.description.empty() && info.description != simpleName) {
    info.name = simpleName + L" (" + info.description + L")";
  } else {
    info.name = simpleName;
  }

  if (lpD3DDeviceDesc) {
    info.deviceGuid = lpD3DDeviceDesc->deviceGUID;
  } else {
    ZeroMemory(&info.deviceGuid, sizeof(info.deviceGuid));
  }

  // Deduplicate: skip if we already have an entry with this GUID.
  for (const auto& existing : *devices) {
    if (std::memcmp(&existing.deviceGuid, &info.deviceGuid, sizeof(GUID)) == 0) {
      return 1; // already recorded -- continue enumeration
    }
  }

  devices->push_back(std::move(info));
  return 1; // DDENUMRET_OK -- continue enumeration
}

} // anonymous namespace

// =========================================================================
// Public API
// =========================================================================

std::vector<D3DDeviceInfo> EnumerateD3DDevices() {
  std::vector<D3DDeviceInfo> devices;

  // --- load ddraw.dll dynamically ----------------------------------------
  HMODULE hDDraw = LoadLibraryW(L"ddraw.dll");
  if (!hDDraw) {
    return devices;
  }

  auto pfnCreateEx = reinterpret_cast<PFN_DirectDrawCreateEx>(
      GetProcAddress(hDDraw, "DirectDrawCreateEx"));
  if (!pfnCreateEx) {
    FreeLibrary(hDDraw);
    return devices;
  }

  // --- create IDirectDraw7 for the primary display adapter ---------------
  IUnknown* pDD7 = nullptr;
  HRESULT hr = pfnCreateEx(
      nullptr,                          // primary display
      reinterpret_cast<void**>(&pDD7),
      kIID_IDirectDraw7,
      nullptr);
  if (FAILED(hr) || !pDD7) {
    FreeLibrary(hDDraw);
    return devices;
  }

  // --- obtain IDirect3D7 ------------------------------------------------
  IUnknown* pD3D7 = nullptr;
  hr = pDD7->QueryInterface(kIID_IDirect3D7, reinterpret_cast<void**>(&pD3D7));
  if (FAILED(hr) || !pD3D7) {
    pDD7->Release();
    FreeLibrary(hDDraw);
    return devices;
  }

  // --- IDirect3D7::EnumDevices (vtable slot 3) ---------------------------
  auto vtable        = *reinterpret_cast<void***>(pD3D7);
  auto pfnEnumDevices = reinterpret_cast<PFN_D3D7_EnumDevices>(vtable[3]);
  pfnEnumDevices(pD3D7, DeviceEnumCallback, &devices);

  // --- cleanup -----------------------------------------------------------
  pD3D7->Release();
  pDD7->Release();
  FreeLibrary(hDDraw);

  return devices;
}

std::wstring FormatGuid(const GUID& guid) {
  wchar_t buf[64];
  swprintf_s(buf, _countof(buf),
             L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             guid.Data1, guid.Data2, guid.Data3,
             guid.Data4[0], guid.Data4[1],
             guid.Data4[2], guid.Data4[3], guid.Data4[4],
             guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return buf;
}

std::wstring FormatGuidAsRegHex(const GUID& guid) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&guid);
  static const wchar_t* hex = L"0123456789abcdef";
  std::wstring out;
  out.reserve(16 * 3 - 1);
  for (int i = 0; i < 16; i++) {
    if (i > 0) {
      out += L',';
    }
    out += hex[(bytes[i] >> 4) & 0xF];
    out += hex[bytes[i] & 0xF];
  }
  return out;
}

const D3DDeviceInfo* SelectBestDevice(const std::vector<D3DDeviceInfo>& devices) {
  if (devices.empty()) {
    return nullptr;
  }

  // 1. Prefer a hardware device (anything that is not the software RGB
  //    or reference rasteriser).
  for (const auto& dev : devices) {
    if (std::memcmp(&dev.deviceGuid, &kGUID_RGBDevice, sizeof(GUID)) != 0 &&
        std::memcmp(&dev.deviceGuid, &kGUID_RefDevice, sizeof(GUID)) != 0) {
      return &dev;
    }
  }

  // 2. Last resort: first device in the list.
  return &devices[0];
}

} // namespace twinshim

#endif // _WIN32
