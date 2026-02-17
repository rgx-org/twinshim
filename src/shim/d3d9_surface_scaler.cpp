#include "shim/d3d9_surface_scaler.h"

#include "shim/minhook_runtime.h"
#include "shim/surface_scale_config.h"

#include <MinHook.h>

#include <windows.h>

#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <cstdio>

#include <tlhelp32.h>

namespace hklmwrap {
namespace {

struct DeviceState {
  bool scalingEnabled = false;
  double scaleFactor = 1.0;
  SurfaceScaleMethod scaleMethod = SurfaceScaleMethod::kPoint;
  HWND hwnd = nullptr;
  UINT srcW = 0;
  UINT srcH = 0;
  UINT dstW = 0;
  UINT dstH = 0;
  IDirect3DSwapChain9* swapchain = nullptr;
  bool loggedCreate = false;
  bool loggedFirstPresent = false;
};

template <typename T>
static void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

static std::atomic<bool> g_loggedConfig{false};

static std::atomic<bool> g_active{false};
static std::atomic<bool> g_hooksInstalled{false};
static std::atomic<bool> g_stopInitThread{false};
static HANDLE g_initThread = nullptr;

static std::atomic<bool> g_loggedFullscreenSkip{false};
static std::atomic<bool> g_loggedSnapshot{false};
static std::atomic<bool> g_seenD3D9{false};
static std::atomic<bool> g_seenD3D8{false};
static std::atomic<bool> g_seenDDraw{false};
static std::atomic<bool> g_seenDXGI{false};
static std::atomic<bool> g_seenD3D11{false};
static std::atomic<bool> g_seenOpenGL{false};
static std::atomic<bool> g_seenVulkan{false};

static void D3D9TraceWrite(const char* text) {
  if (!text || !*text) {
    return;
  }

  // Always mirror to debugger output so DebugView can capture it.
  OutputDebugStringA(text);

  wchar_t pipeBuf[512] = {};
  DWORD pipeLen = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  }
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }
  pipeBuf[pipeLen] = L'\0';

  HANDLE h = CreateFileW(pipeBuf, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(h, text, (DWORD)lstrlenA(text), &written, nullptr);
  CloseHandle(h);
}

static void D3D9Tracef(const char* fmt, ...) {
  if (!fmt || !*fmt) {
    return;
  }
  char buf[1024];
  buf[0] = '\0';

  int used = std::snprintf(buf, sizeof(buf), "[shim:d3d9] ");
  if (used < 0) {
    used = 0;
    buf[0] = '\0';
  }

  va_list ap;
  va_start(ap, fmt);
  if ((size_t)used < sizeof(buf)) {
    (void)std::vsnprintf(buf + used, sizeof(buf) - (size_t)used, fmt, ap);
  }
  va_end(ap);

  buf[sizeof(buf) - 1] = '\0';

  // Ensure newline.
  size_t len = std::strlen(buf);
  if (len == 0 || buf[len - 1] != '\n') {
    if (len + 1 < sizeof(buf)) {
      buf[len] = '\n';
      buf[len + 1] = '\0';
    }
  }
  D3D9TraceWrite(buf);
}

static void ProbeLogModuleIfPresent(const wchar_t* moduleName, std::atomic<bool>& seenFlag) {
  if (seenFlag.load(std::memory_order_acquire)) {
    return;
  }
  HMODULE h = GetModuleHandleW(moduleName);
  if (!h) {
    return;
  }
  bool expected = false;
  if (!seenFlag.compare_exchange_strong(expected, true)) {
    return;
  }
  D3D9Tracef("module loaded: %ls @ %p", moduleName, (void*)h);
}

static void ProbeLogCommonGraphicsModules() {
  ProbeLogModuleIfPresent(L"d3d9.dll", g_seenD3D9);
  ProbeLogModuleIfPresent(L"d3d8.dll", g_seenD3D8);
  const bool wasDDraw = g_seenDDraw.load(std::memory_order_acquire);
  ProbeLogModuleIfPresent(L"ddraw.dll", g_seenDDraw);
  if (!wasDDraw && g_seenDDraw.load(std::memory_order_acquire)) {
    D3D9Tracef("ddraw.dll detected (DirectDraw in use)");
  }
  ProbeLogModuleIfPresent(L"dxgi.dll", g_seenDXGI);
  ProbeLogModuleIfPresent(L"d3d11.dll", g_seenD3D11);
  ProbeLogModuleIfPresent(L"opengl32.dll", g_seenOpenGL);
  ProbeLogModuleIfPresent(L"vulkan-1.dll", g_seenVulkan);
}

static bool ContainsNoCase(const wchar_t* haystack, const wchar_t* needle) {
  if (!haystack || !needle || !*needle) {
    return false;
  }
  const size_t hlen = wcslen(haystack);
  const size_t nlen = wcslen(needle);
  if (nlen > hlen) {
    return false;
  }
  for (size_t i = 0; i + nlen <= hlen; i++) {
    bool match = true;
    for (size_t j = 0; j < nlen; j++) {
      const wchar_t a = (wchar_t)towlower(haystack[i + j]);
      const wchar_t b = (wchar_t)towlower(needle[j]);
      if (a != b) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

static void ProbeDumpInterestingModulesOnce() {
  bool expected = false;
  if (!g_loggedSnapshot.compare_exchange_strong(expected, true)) {
    return;
  }

  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  if (snap == INVALID_HANDLE_VALUE) {
    D3D9Tracef("module snapshot failed (CreateToolhelp32Snapshot error=%lu)", GetLastError());
    return;
  }

  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  if (!Module32FirstW(snap, &me)) {
    D3D9Tracef("module snapshot empty (Module32FirstW error=%lu)", GetLastError());
    CloseHandle(snap);
    return;
  }

  D3D9Tracef("module snapshot (filtered):");
  int count = 0;
  do {
    const wchar_t* name = me.szModule;
    if (ContainsNoCase(name, L"d3d") ||
        ContainsNoCase(name, L"ddraw") ||
        ContainsNoCase(name, L"dxgi") ||
        ContainsNoCase(name, L"opengl") ||
        ContainsNoCase(name, L"vulkan") ||
        ContainsNoCase(name, L"glide") ||
        ContainsNoCase(name, L"dgvoodoo")) {
      D3D9Tracef("  %ls @ %p", name, (void*)me.modBaseAddr);
      count++;
      if (count >= 80) {
        D3D9Tracef("  ... (truncated)");
        break;
      }
    }
  } while (Module32NextW(snap, &me));

  CloseHandle(snap);
}

static bool IsDgVoodooPresent() {
  static std::atomic<int> cached{-1};
  const int v = cached.load(std::memory_order_acquire);
  if (v == 0) {
    return false;
  }
  if (v == 1) {
    return true;
  }

  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  if (snap == INVALID_HANDLE_VALUE) {
    cached.store(0, std::memory_order_release);
    return false;
  }

  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  bool found = false;
  if (Module32FirstW(snap, &me)) {
    do {
      if (ContainsNoCase(me.szModule, L"dgvoodoo") || ContainsNoCase(me.szExePath, L"dgvoodoo")) {
        found = true;
        break;
      }
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);

  cached.store(found ? 1 : 0, std::memory_order_release);
  return found;
}

static std::mutex g_stateMutex;
static std::unordered_map<IDirect3DDevice9*, DeviceState> g_deviceStates;

// Hook targets / originals
using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

static Direct3DCreate9_t g_fpDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t g_fpDirect3DCreate9Ex = nullptr;

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);

static CreateDevice_t g_fpCreateDevice = nullptr;
static CreateDeviceEx_t g_fpCreateDeviceEx = nullptr;
static Reset_t g_fpReset = nullptr;
static Present_t g_fpPresent = nullptr;

static std::atomic<void*> g_targetCreateDevice{nullptr};
static std::atomic<void*> g_targetCreateDeviceEx{nullptr};
static std::atomic<void*> g_targetReset{nullptr};
static std::atomic<void*> g_targetPresent{nullptr};

static constexpr double kMinScale = 1.1;
static constexpr double kMaxScale = 100.0;

static bool IsScalingEnabled() {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  return cfg.enabled && cfg.scaleValid && cfg.factor >= kMinScale && cfg.factor <= kMaxScale;
}

static void LogConfigOnceIfNeeded() {
  bool expected = false;
  if (!g_loggedConfig.compare_exchange_strong(expected, true)) {
    return;
  }
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled()) {
    if (cfg.scaleSpecified && !cfg.scaleValid) {
      D3D9Tracef("surface scaling disabled (--scale invalid; raw='%ls')", cfg.scaleRaw.c_str());
    } else {
      D3D9Tracef("surface scaling disabled (no valid --scale provided)");
    }
    return;
  }
  if (cfg.methodSpecified && !cfg.methodValid) {
    D3D9Tracef("surface scaling: invalid --scale-method '%ls' -> defaulting to point", cfg.methodRaw.c_str());
  }
  D3D9Tracef("surface scaling enabled (scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
}

static UINT CalcScaledUInt(UINT base, double factor) {
  if (base == 0) {
    return 0;
  }
  const double scaled = (double)base * factor;
  const double rounded = scaled + 0.5;
  if (rounded <= 0.0) {
    return 0;
  }
  if (rounded > (double)UINT32_MAX) {
    return UINT32_MAX;
  }
  return (UINT)rounded;
}

static D3DTEXTUREFILTERTYPE FilterForMethod(SurfaceScaleMethod method) {
  switch (method) {
    case SurfaceScaleMethod::kPoint:
      return D3DTEXF_POINT;
    case SurfaceScaleMethod::kBilinear:
      return D3DTEXF_LINEAR;
    case SurfaceScaleMethod::kBicubic:
    case SurfaceScaleMethod::kCatmullRom:
    case SurfaceScaleMethod::kLanczos:
    case SurfaceScaleMethod::kLanczos3:
      return D3DTEXF_GAUSSIANQUAD;
    case SurfaceScaleMethod::kPixelFast:
      return D3DTEXF_LINEAR;
    default:
      return D3DTEXF_POINT;
  }
}

static bool GetClientSize(HWND hwnd, UINT* outW, UINT* outH) {
  if (!outW || !outH) {
    return false;
  }
  *outW = 0;
  *outH = 0;
  if (!hwnd) {
    return false;
  }
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) {
    return false;
  }
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  *outW = (UINT)w;
  *outH = (UINT)h;
  return true;
}

static bool SetWindowClientSize(HWND hwnd, UINT clientW, UINT clientH) {
  if (!hwnd || clientW == 0 || clientH == 0) {
    return false;
  }
  LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
  RECT rc{0, 0, (LONG)clientW, (LONG)clientH};
  if (!AdjustWindowRectEx(&rc, (DWORD)style, FALSE, (DWORD)exStyle)) {
    return false;
  }
  const int outerW = rc.right - rc.left;
  const int outerH = rc.bottom - rc.top;
  return SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

// Forward declarations for detours.
static IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk);
static HRESULT WINAPI Hook_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out);

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self,
                                                   UINT Adapter,
                                                   D3DDEVTYPE DeviceType,
                                                   HWND hFocusWindow,
                                                   DWORD BehaviorFlags,
                                                   D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   IDirect3DDevice9** ppReturnedDeviceInterface);

static HRESULT STDMETHODCALLTYPE Hook_CreateDeviceEx(IDirect3D9Ex* self,
                                                     UINT Adapter,
                                                     D3DDEVTYPE DeviceType,
                                                     HWND hFocusWindow,
                                                     DWORD BehaviorFlags,
                                                     D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                     D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                     IDirect3DDevice9Ex** ppReturnedDeviceInterface);

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);
static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* device,
                                              CONST RECT* pSourceRect,
                                              CONST RECT* pDestRect,
                                              HWND hDestWindowOverride,
                                              CONST RGNDATA* pDirtyRegion);

static bool EnsureCreateDeviceHookInstalled(IDirect3D9* d3d9) {
  if (!d3d9) {
    return false;
  }
  void** vtbl = *reinterpret_cast<void***>(d3d9);
  if (!vtbl) {
    return false;
  }
  void* target = vtbl[16]; // CreateDevice

  void* expected = nullptr;
  if (!g_targetCreateDevice.compare_exchange_strong(expected, target)) {
    return true;
  }

  if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_CreateDevice), reinterpret_cast<LPVOID*>(&g_fpCreateDevice)) != MH_OK) {
    g_targetCreateDevice.store(nullptr);
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    (void)MH_RemoveHook(target);
    g_targetCreateDevice.store(nullptr);
    g_fpCreateDevice = nullptr;
    return false;
  }
  return true;
}

static bool EnsureCreateDeviceExHookInstalled(IDirect3D9Ex* d3d9ex) {
  if (!d3d9ex) {
    return false;
  }
  void** vtbl = *reinterpret_cast<void***>(d3d9ex);
  if (!vtbl) {
    return false;
  }
  // Assumption: IDirect3D9Ex appends methods after IDirect3D9; CreateDeviceEx is next after CreateDevice.
  void* target = vtbl[17];
  void* expected = nullptr;
  if (!g_targetCreateDeviceEx.compare_exchange_strong(expected, target)) {
    return true;
  }

  if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_CreateDeviceEx), reinterpret_cast<LPVOID*>(&g_fpCreateDeviceEx)) != MH_OK) {
    g_targetCreateDeviceEx.store(nullptr);
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    (void)MH_RemoveHook(target);
    g_targetCreateDeviceEx.store(nullptr);
    g_fpCreateDeviceEx = nullptr;
    return false;
  }
  return true;
}

static bool EnsureDeviceHooksInstalledFromVTable(IDirect3DDevice9* dev) {
  if (!dev) {
    return false;
  }
  void** vtbl = *reinterpret_cast<void***>(dev);
  if (!vtbl) {
    return false;
  }
  void* targetReset = vtbl[16];
  void* targetPresent = vtbl[17];

  if (g_fpReset == nullptr) {
    void* expected = nullptr;
    if (g_targetReset.compare_exchange_strong(expected, targetReset)) {
      if (MH_CreateHook(targetReset, reinterpret_cast<LPVOID>(&Hook_Reset), reinterpret_cast<LPVOID*>(&g_fpReset)) != MH_OK) {
        g_targetReset.store(nullptr);
      } else {
        (void)MH_EnableHook(targetReset);
      }
    }
  }

  if (g_fpPresent == nullptr) {
    void* expected = nullptr;
    if (g_targetPresent.compare_exchange_strong(expected, targetPresent)) {
      if (MH_CreateHook(targetPresent, reinterpret_cast<LPVOID>(&Hook_Present), reinterpret_cast<LPVOID*>(&g_fpPresent)) != MH_OK) {
        g_targetPresent.store(nullptr);
      } else {
        (void)MH_EnableHook(targetPresent);
      }
    }
  }
  return g_fpPresent != nullptr;
}

static bool CreateOrResizeSwapChain(IDirect3DDevice9* device, DeviceState& st) {
  SafeRelease(st.swapchain);
  if (!device || !st.hwnd || st.dstW == 0 || st.dstH == 0) {
    return false;
  }

  IDirect3DSurface9* src = nullptr;
  HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src);
  if (FAILED(hr) || !src) {
    SafeRelease(src);
    return false;
  }
  D3DSURFACE_DESC desc{};
  hr = src->GetDesc(&desc);
  SafeRelease(src);
  if (FAILED(hr)) {
    return false;
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = st.hwnd;
  pp.BackBufferWidth = st.dstW;
  pp.BackBufferHeight = st.dstH;
  pp.BackBufferFormat = desc.Format;
  pp.BackBufferCount = 1;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DSwapChain9* sc = nullptr;
  hr = device->CreateAdditionalSwapChain(&pp, &sc);
  if (FAILED(hr) || !sc) {
    SafeRelease(sc);
    return false;
  }
  st.swapchain = sc;
  return true;
}

static bool UpdateStateForDevice(IDirect3DDevice9* dev,
                                 bool scalingEnabled,
                                 double scaleFactor,
                                 SurfaceScaleMethod scaleMethod,
                                 HWND hwnd,
                                 UINT srcW,
                                 UINT srcH,
                                 UINT dstW,
                                 UINT dstH) {
  if (!dev || !scalingEnabled) {
    return false;
  }
  DeviceState st;
  st.scalingEnabled = true;
  st.scaleFactor = scaleFactor;
  st.scaleMethod = scaleMethod;
  st.hwnd = hwnd;
  st.srcW = srcW;
  st.srcH = srcH;
  st.dstW = dstW;
  st.dstH = dstH;

  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto& slot = g_deviceStates[dev];
  SafeRelease(slot.swapchain);
  // Preserve log throttles across resize/reset.
  const bool preserveCreate = slot.loggedCreate;
  const bool preserveFirstPresent = slot.loggedFirstPresent;
  slot = st;
  slot.loggedCreate = preserveCreate;
  slot.loggedFirstPresent = preserveFirstPresent;
  return true;
}

static void MarkLoggedCreate(IDirect3DDevice9* dev) {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(dev);
  if (it != g_deviceStates.end()) {
    it->second.loggedCreate = true;
  }
}

static bool TryMarkLoggedFirstPresent(IDirect3DDevice9* dev) {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(dev);
  if (it == g_deviceStates.end()) {
    return false;
  }
  if (it->second.loggedFirstPresent) {
    return false;
  }
  it->second.loggedFirstPresent = true;
  return true;
}

static bool TryGetState(IDirect3DDevice9* dev, DeviceState* out) {
  if (!out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(dev);
  if (it == g_deviceStates.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

static void UpdateSwapChainPointer(IDirect3DDevice9* dev, IDirect3DSwapChain9* newSc) {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(dev);
  if (it == g_deviceStates.end()) {
    SafeRelease(newSc);
    return;
  }
  SafeRelease(it->second.swapchain);
  it->second.swapchain = newSc;
}

static void RemoveDeviceState(IDirect3DDevice9* dev) {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(dev);
  if (it == g_deviceStates.end()) {
    return;
  }
  SafeRelease(it->second.swapchain);
  g_deviceStates.erase(it);
}

static IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
  IDirect3D9* d3d9 = g_fpDirect3DCreate9 ? g_fpDirect3DCreate9(sdk) : nullptr;
  if (d3d9) {
    (void)EnsureCreateDeviceHookInstalled(d3d9);
  }
  return d3d9;
}

static HRESULT WINAPI Hook_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
  if (!g_fpDirect3DCreate9Ex) {
    return E_FAIL;
  }
  HRESULT hr = g_fpDirect3DCreate9Ex(sdk, out);
  if (SUCCEEDED(hr) && out && *out) {
    (void)EnsureCreateDeviceHookInstalled(*out);
    (void)EnsureCreateDeviceExHookInstalled(*out);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self,
                                                   UINT Adapter,
                                                   D3DDEVTYPE DeviceType,
                                                   HWND hFocusWindow,
                                                   DWORD BehaviorFlags,
                                                   D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   IDirect3DDevice9** ppReturnedDeviceInterface) {
  LogConfigOnceIfNeeded();
  if (!g_fpCreateDevice) {
    return E_FAIL;
  }

  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !pPresentationParameters || !ppReturnedDeviceInterface) {
    return g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  }

  // Only apply in windowed mode.
  if (!pPresentationParameters->Windowed) {
    bool expected = false;
    if (g_loggedFullscreenSkip.compare_exchange_strong(expected, true)) {
      D3D9Tracef("CreateDevice: fullscreen detected -> surface scaling disabled (windowed-only)");
    }
    return g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  }

  HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;

  D3DPRESENT_PARAMETERS ppCopy = *pPresentationParameters;

  UINT srcW = ppCopy.BackBufferWidth;
  UINT srcH = ppCopy.BackBufferHeight;
  if (srcW == 0 || srcH == 0) {
    // In windowed mode, some apps pass 0 and rely on implicit sizing.
    (void)GetClientSize(hwnd, &srcW, &srcH);
  }

  UINT dstW = CalcScaledUInt(srcW, cfg.factor);
  UINT dstH = CalcScaledUInt(srcH, cfg.factor);

  HRESULT hr = g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, &ppCopy, ppReturnedDeviceInterface);
  if (FAILED(hr) || !*ppReturnedDeviceInterface) {
    return hr;
  }

  IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
  (void)EnsureDeviceHooksInstalledFromVTable(dev);

  const bool resized = SetWindowClientSize(hwnd, dstW, dstH);
  D3D9Tracef("CreateDevice: scale resize window client -> %ux%u (scale=%.3f, %s)", dstW, dstH, cfg.factor, resized ? "ok" : "failed");

  (void)UpdateStateForDevice(dev, true, cfg.factor, cfg.method, hwnd, srcW, srcH, dstW, dstH);

  {
    UINT winW = 0, winH = 0;
    (void)GetClientSize(hwnd, &winW, &winH);
    D3D9Tracef("CreateDevice: scaling=1 window=%p client=%ux%u src=%ux%u dst=%ux%u scale=%.3f method=%ls bb=%ux%u windowed=1",
               (void*)hwnd,
               winW,
               winH,
               srcW,
               srcH,
               dstW,
               dstH,
               cfg.factor,
               SurfaceScaleMethodToString(cfg.method),
               (unsigned)ppCopy.BackBufferWidth,
               (unsigned)ppCopy.BackBufferHeight);
    MarkLoggedCreate(dev);
  }

  // Build swapchain now (best effort).
  DeviceState snapshot;
  if (TryGetState(dev, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(dev, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(dev, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDeviceEx(IDirect3D9Ex* self,
                                                     UINT Adapter,
                                                     D3DDEVTYPE DeviceType,
                                                     HWND hFocusWindow,
                                                     DWORD BehaviorFlags,
                                                     D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                     D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                     IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
  LogConfigOnceIfNeeded();
  if (!g_fpCreateDeviceEx) {
    return E_FAIL;
  }
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !pPresentationParameters || !ppReturnedDeviceInterface) {
    return g_fpCreateDeviceEx(self,
                              Adapter,
                              DeviceType,
                              hFocusWindow,
                              BehaviorFlags,
                              pPresentationParameters,
                              pFullscreenDisplayMode,
                              ppReturnedDeviceInterface);
  }
  if (!pPresentationParameters->Windowed) {
    bool expected = false;
    if (g_loggedFullscreenSkip.compare_exchange_strong(expected, true)) {
      D3D9Tracef("CreateDeviceEx: fullscreen detected -> surface scaling disabled (windowed-only)");
    }
    return g_fpCreateDeviceEx(self,
                              Adapter,
                              DeviceType,
                              hFocusWindow,
                              BehaviorFlags,
                              pPresentationParameters,
                              pFullscreenDisplayMode,
                              ppReturnedDeviceInterface);
  }

  HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;

  D3DPRESENT_PARAMETERS ppCopy = *pPresentationParameters;

  UINT srcW = ppCopy.BackBufferWidth;
  UINT srcH = ppCopy.BackBufferHeight;
  if (srcW == 0 || srcH == 0) {
    (void)GetClientSize(hwnd, &srcW, &srcH);
  }

  UINT dstW = CalcScaledUInt(srcW, cfg.factor);
  UINT dstH = CalcScaledUInt(srcH, cfg.factor);

  HRESULT hr = g_fpCreateDeviceEx(self,
                                  Adapter,
                                  DeviceType,
                                  hFocusWindow,
                                  BehaviorFlags,
                                  &ppCopy,
                                  pFullscreenDisplayMode,
                                  ppReturnedDeviceInterface);
  if (FAILED(hr) || !*ppReturnedDeviceInterface) {
    return hr;
  }

  IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
  (void)EnsureDeviceHooksInstalledFromVTable(dev);

  const bool resized = SetWindowClientSize(hwnd, dstW, dstH);
  D3D9Tracef("CreateDeviceEx: scale resize window client -> %ux%u (scale=%.3f, %s)", dstW, dstH, cfg.factor, resized ? "ok" : "failed");

  (void)UpdateStateForDevice(dev, true, cfg.factor, cfg.method, hwnd, srcW, srcH, dstW, dstH);
  {
    UINT winW = 0, winH = 0;
    (void)GetClientSize(hwnd, &winW, &winH);
    D3D9Tracef("CreateDeviceEx: scaling=1 window=%p client=%ux%u src=%ux%u dst=%ux%u scale=%.3f method=%ls bb=%ux%u windowed=1",
               (void*)hwnd,
               winW,
               winH,
               srcW,
               srcH,
               dstW,
               dstH,
               cfg.factor,
               SurfaceScaleMethodToString(cfg.method),
               (unsigned)ppCopy.BackBufferWidth,
               (unsigned)ppCopy.BackBufferHeight);
    MarkLoggedCreate(dev);
  }
  DeviceState snapshot;
  if (TryGetState(dev, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(dev, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(dev, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters) {
  if (!g_fpReset) {
    return D3DERR_INVALIDCALL;
  }
  LogConfigOnceIfNeeded();

  DeviceState st;
  const bool tracked = TryGetState(device, &st);
  if (!tracked || !st.scalingEnabled || !IsScalingEnabled()) {
    return g_fpReset(device, pPresentationParameters);
  }

  // Drop swapchain before reset.
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_deviceStates.find(device);
    if (it != g_deviceStates.end()) {
      SafeRelease(it->second.swapchain);
    }
  }

  HRESULT hr = g_fpReset(device, pPresentationParameters);
  if (FAILED(hr)) {
    return hr;
  }

  // Rebuild swapchain sizes.
  UINT srcW = 0, srcH = 0;
  if (pPresentationParameters) {
    srcW = pPresentationParameters->BackBufferWidth;
    srcH = pPresentationParameters->BackBufferHeight;
  }
  if (srcW == 0 || srcH == 0) {
    (void)GetClientSize(st.hwnd, &srcW, &srcH);
  }
  if (srcW == 0 || srcH == 0) {
    return hr;
  }

  UINT dstW = CalcScaledUInt(srcW, st.scaleFactor);
  UINT dstH = CalcScaledUInt(srcH, st.scaleFactor);

  (void)SetWindowClientSize(st.hwnd, dstW, dstH);

  (void)UpdateStateForDevice(device, true, st.scaleFactor, st.scaleMethod, st.hwnd, srcW, srcH, dstW, dstH);
  DeviceState snapshot;
  if (TryGetState(device, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(device, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(device, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* device,
                                              CONST RECT* pSourceRect,
                                              CONST RECT* pDestRect,
                                              HWND hDestWindowOverride,
                                              CONST RGNDATA* pDirtyRegion) {
  if (!g_fpPresent) {
    return D3DERR_INVALIDCALL;
  }
  LogConfigOnceIfNeeded();

  if (!IsScalingEnabled()) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  DeviceState st;
  if (!TryGetState(device, &st) || !st.scalingEnabled) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // If the app is presenting to a different window, don't interfere.
  if (hDestWindowOverride && st.hwnd && hDestWindowOverride != st.hwnd) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // Grab current swapchain pointer.
  IDirect3DSwapChain9* sc = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_deviceStates.find(device);
    if (it != g_deviceStates.end() && it->second.swapchain) {
      sc = it->second.swapchain;
      sc->AddRef();
    }
  }

  if (!sc) {
    // Best-effort create (device might have just been reset).
    DeviceState snapshot;
    if (TryGetState(device, &snapshot)) {
      DeviceState local = snapshot;
      if (CreateOrResizeSwapChain(device, local)) {
        sc = local.swapchain;
        local.swapchain = nullptr;
        if (sc) {
          sc->AddRef();
          UpdateSwapChainPointer(device, sc);
        }
      }
    }
  }

  if (!sc) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  if (TryMarkLoggedFirstPresent(device)) {
    D3D9Tracef("Present: scaling active (%ls) src=%ux%u -> dst=%ux%u", SurfaceScaleMethodToString(st.scaleMethod), st.srcW, st.srcH, st.dstW, st.dstH);
  }

  IDirect3DSurface9* src = nullptr;
  HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src);
  if (FAILED(hr) || !src) {
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  IDirect3DSurface9* dst = nullptr;
  hr = sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &dst);
  if (FAILED(hr) || !dst) {
    SafeRelease(dst);
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // Filtered upscale.
  D3DTEXTUREFILTERTYPE filter = FilterForMethod(st.scaleMethod);
  hr = device->StretchRect(src, pSourceRect, dst, nullptr, filter);
  if (FAILED(hr) && filter == D3DTEXF_GAUSSIANQUAD) {
    // Fallback: many drivers reject GAUSSIANQUAD for StretchRect.
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        D3D9Tracef("Present: high-quality filter requested but GAUSSIANQUAD rejected; falling back to linear");
      }
    }
    hr = device->StretchRect(src, pSourceRect, dst, nullptr, D3DTEXF_LINEAR);
  }
  if (FAILED(hr) && st.scaleMethod != SurfaceScaleMethod::kPoint) {
    // Last-chance fallback.
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        D3D9Tracef("Present: filtered scaling rejected; falling back to point");
      }
    }
    hr = device->StretchRect(src, pSourceRect, dst, nullptr, D3DTEXF_POINT);
  }
  if (FAILED(hr)) {
    SafeRelease(dst);
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  HRESULT hrPresent = sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
  SafeRelease(dst);
  SafeRelease(src);
  SafeRelease(sc);

  if (FAILED(hrPresent)) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }
  return D3D_OK;
}

static bool CreateHookApiTyped(LPCWSTR moduleName, LPCSTR procName, LPVOID detour, LPVOID* original) {
  return MH_CreateHookApi(moduleName, procName, detour, original) == MH_OK;
}

static bool InstallD3D9ExportsHooksOnce() {
  LogConfigOnceIfNeeded();
  if (!IsScalingEnabled()) {
    return true;
  }

  if (IsDgVoodooPresent()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      D3D9Tracef("dgVoodoo detected; shim D3D9 surface scaling hooks disabled (use dgVoodoo AddOn)");
    }
    return true;
  }

  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  D3D9Tracef("surface scaling hooks enabled (scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));

  if (!AcquireMinHook()) {
    D3D9Tracef("AcquireMinHook failed");
    return false;
  }

  bool ok = false;
  // Try both module spellings.
  ok |= CreateHookApiTyped(L"d3d9", "Direct3DCreate9", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9));
  ok |= CreateHookApiTyped(L"d3d9.dll", "Direct3DCreate9", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9));

  // Optional (Vista+).
  (void)CreateHookApiTyped(L"d3d9", "Direct3DCreate9Ex", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9Ex), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9Ex));
  (void)CreateHookApiTyped(L"d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9Ex), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9Ex));

  if (!ok) {
    D3D9Tracef("failed to hook Direct3DCreate9 exports (d3d9.dll not hookable yet?)");
    ReleaseMinHook();
    return false;
  }

  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    D3D9Tracef("MH_EnableHook(MH_ALL_HOOKS) failed");
    ReleaseMinHook();
    return false;
  }
  g_hooksInstalled.store(true, std::memory_order_release);
  D3D9Tracef("Direct3DCreate9 export hooks installed");
  return true;
}

static DWORD WINAPI D3D9InitThreadProc(LPVOID) {
  // Wait until d3d9 is present in the process; then install export hooks.
  // 50ms * 12000 ~= 10 minutes.
  for (int i = 0; i < 12000 && !g_stopInitThread.load(std::memory_order_acquire); i++) {
    if ((i % 20) == 0) {
      ProbeLogCommonGraphicsModules();
      // After ~5 seconds, if we still haven't observed d3d9.dll, dump a filtered module snapshot.
      if (i == 100 && !g_seenD3D9.load(std::memory_order_acquire)) {
        ProbeDumpInterestingModulesOnce();
      }
    }
    if (GetModuleHandleW(L"d3d9.dll") != nullptr || GetModuleHandleW(L"d3d9") != nullptr) {
      break;
    }
    Sleep(50);
  }

  if (!g_stopInitThread.load(std::memory_order_acquire)) {
    const bool ok = InstallD3D9ExportsHooksOnce();
    D3D9Tracef("init thread finished (ok=%d)", ok ? 1 : 0);
    if (!ok && !g_seenD3D9.load(std::memory_order_acquire)) {
      D3D9Tracef("d3d9.dll not detected; likely not a D3D9 path (check snapshot above)");
    }
  }

  return 0;
}

}

bool InstallD3D9SurfaceScalerHooks() {
  LogConfigOnceIfNeeded();
  if (!IsScalingEnabled()) {
    g_active.store(false, std::memory_order_release);
    g_hooksInstalled.store(false, std::memory_order_release);
    return true;
  }

  // dgVoodoo (and similar wrappers) can route D3D9 through other backends.
  // The shim's present/backbuffer scaling hooks are fragile there; prefer a
  // dgVoodoo AddOn that can see the real backend resources.
  if (IsDgVoodooPresent()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      D3D9Tracef("dgVoodoo detected; shim D3D9 surface scaling disabled (use dgVoodoo AddOn)");
    }
    g_active.store(false, std::memory_order_release);
    g_hooksInstalled.store(false, std::memory_order_release);
    return true;
  }

  bool expected = false;
  if (!g_active.compare_exchange_strong(expected, true)) {
    return true;
  }

  g_stopInitThread.store(false, std::memory_order_release);
  g_initThread = CreateThread(nullptr, 0, &D3D9InitThreadProc, nullptr, 0, nullptr);
  if (!g_initThread) {
    D3D9Tracef("failed to start init thread");
    g_active.store(false, std::memory_order_release);
    return false;
  }
  {
    const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
    D3D9Tracef("install requested (waiting for d3d9.dll; scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
  }
  return true;
}

bool AreD3D9SurfaceScalerHooksActive() {
  return g_hooksInstalled.load(std::memory_order_acquire);
}

void RemoveD3D9SurfaceScalerHooks() {
  if (!g_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  g_hooksInstalled.store(false, std::memory_order_release);

  g_stopInitThread.store(true, std::memory_order_release);
  HANDLE th = g_initThread;
  g_initThread = nullptr;
  if (th) {
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
  }

  // Release swapchains.
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    for (auto& kv : g_deviceStates) {
      SafeRelease(kv.second.swapchain);
    }
    g_deviceStates.clear();
  }

  // Disable hooks we installed.
  void* tgt = g_targetPresent.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetReset.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetCreateDeviceEx.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetCreateDevice.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }

  // Note: export hooks are disabled via MH_ALL_HOOKS during registry hook teardown,
  // but in case registry hooks are disabled we still release MinHook here.
  g_fpDirect3DCreate9 = nullptr;
  g_fpDirect3DCreate9Ex = nullptr;

  g_fpCreateDevice = nullptr;
  g_fpCreateDeviceEx = nullptr;
  g_fpReset = nullptr;
  g_fpPresent = nullptr;

  ReleaseMinHook();
}

}
