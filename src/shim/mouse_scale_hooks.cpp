#include "shim/mouse_scale_hooks.h"

#include "shim/minhook_runtime.h"
#include "shim/surface_scale_config.h"
#include "shim/window_scale_registry.h"

#include <MinHook.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace twinshim {
namespace {

static std::atomic<bool> g_installed{false};

static bool IsMouseDebugEnabled() {
  static std::atomic<int> cached{-1};
  int v = cached.load(std::memory_order_acquire);
  if (v != -1) {
    return v == 1;
  }
  wchar_t buf[16] = {};
  DWORD n = GetEnvironmentVariableW(L"TWINSHIM_MOUSE_DEBUG", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (!n || n >= (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
    n = GetEnvironmentVariableW(L"HKLM_WRAPPER_MOUSE_DEBUG", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  }
  bool on = false;
  if (n && n < (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
    buf[n] = L'\0';
    if (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y') {
      on = true;
    }
  }
  cached.store(on ? 1 : 0, std::memory_order_release);
  return on;
}

static bool IsTwinShimDebugEnabled() {
  wchar_t dummy[2] = {};
  DWORD n = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  if (n) {
    return true;
  }
  n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  return n != 0;
}

static void TraceWrite(const char* text) {
  if (!IsTwinShimDebugEnabled()) {
    return;
  }
  if (!text || !*text) {
    return;
  }

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

static void Tracef(const char* fmt, ...) {
  if (!IsTwinShimDebugEnabled()) {
    return;
  }
  if (!fmt || !*fmt) {
    return;
  }
  char buf[1024];
  buf[0] = '\0';

  int used = std::snprintf(buf, sizeof(buf), "[shim:mouse-map] ");
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
  size_t len = std::strlen(buf);
  if (len == 0 || buf[len - 1] != '\n') {
    if (len + 1 < sizeof(buf)) {
      buf[len] = '\n';
      buf[len + 1] = '\0';
    }
  }
  TraceWrite(buf);
}

using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);

static GetCursorPos_t g_fpGetCursorPos = nullptr;

// Forward declarations.
static bool TryGetScaledInfoForHwnd(HWND hwnd, ScaledWindowInfo* out);

static LONG ScaleCoordRound(LONG v, int num, int denom) {
  if (denom <= 0 || num <= 0) {
    return v;
  }
  const double scaled = ((double)v * (double)num) / (double)denom;
  long long r = (long long)std::llround(scaled);
  if (r < (long long)LONG_MIN) {
    return LONG_MIN;
  }
  if (r > (long long)LONG_MAX) {
    return LONG_MAX;
  }
  return (LONG)r;
}

static bool GetClientSize(HWND hwnd, int* outW, int* outH) {
  if (outW) {
    *outW = 0;
  }
  if (outH) {
    *outH = 0;
  }
  if (!hwnd || !outW || !outH) {
    return false;
  }
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) {
    return false;
  }
  const int w = (int)(rc.right - rc.left);
  const int h = (int)(rc.bottom - rc.top);
  if (w <= 0 || h <= 0) {
    return false;
  }
  *outW = w;
  *outH = h;
  return true;
}

static bool GetClientRectInScreen(HWND hwnd, RECT* outRc) {
  if (!outRc) {
    return false;
  }
  *outRc = RECT{};
  if (!hwnd) {
    return false;
  }
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) {
    return false;
  }
  POINT pt{rc.left, rc.top};
  // NOTE: We intentionally call the real Win32 API here (we don't hook it by default).
  if (!::ClientToScreen(hwnd, &pt)) {
    return false;
  }
  const int w = (int)(rc.right - rc.left);
  const int h = (int)(rc.bottom - rc.top);
  outRc->left = pt.x;
  outRc->top = pt.y;
  outRc->right = pt.x + w;
  outRc->bottom = pt.y + h;
  return w > 0 && h > 0;
}

struct FindWindowCtx {
  DWORD pid = 0;
  HWND best = nullptr;
  long long bestArea = 0;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  FindWindowCtx* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
  if (!ctx) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) {
    return TRUE;
  }
  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }
  int cw = 0, ch = 0;
  if (!GetClientSize(hwnd, &cw, &ch)) {
    return TRUE;
  }
  const long long area = (long long)cw * (long long)ch;
  if (area > ctx->bestArea) {
    ctx->bestArea = area;
    ctx->best = hwnd;
  }
  return TRUE;
}

static HWND FindBestTopLevelWindowForCurrentProcess() {
  FindWindowCtx ctx;
  ctx.pid = GetCurrentProcessId();
  (void)EnumWindows(&EnumWindowsProc, (LPARAM)&ctx);
  return ctx.best;
}

static bool TryGetPrimaryScaledWindowInfo(ScaledWindowInfo* outInfo, RECT* outClientInScreen) {
  if (outInfo) {
    *outInfo = ScaledWindowInfo{};
  }
  if (outClientInScreen) {
    *outClientInScreen = RECT{};
  }
  if (!outInfo || !outClientInScreen) {
    return false;
  }

  static std::atomic<HWND> cached{nullptr};
  HWND hwnd = cached.load(std::memory_order_acquire);
  if (!hwnd || !IsWindow(hwnd)) {
    hwnd = FindBestTopLevelWindowForCurrentProcess();
    cached.store(hwnd, std::memory_order_release);
  }
  if (!hwnd) {
    return false;
  }

  if (!TryGetScaledInfoForHwnd(hwnd, outInfo)) {
    return false;
  }
  // Ensure client rect query uses the same HWND we chose.
  if (!GetClientRectInScreen(hwnd, outClientInScreen)) {
    return false;
  }
  return true;
}

// Fallback for scaling implementations that resize the window but don't explicitly
// register src/dst sizes with the shim (e.g. dgVoodoo AddOn path).
//
// This inference divides the current client size by the configured scale factor to
// derive the logical (pre-scale) dimensions.  It is only valid when an external
// scaler (dgVoodoo) has actually enlarged the window.  We gate on the addon-ready
// signal so that inference only engages after the addon has confirmed it is loaded
// and actively scaling; without this, missing dgVoodoo DLLs would cause incorrect
// cursor mapping against an unscaled window.
static bool TryInferScaledWindow(HWND hwnd, ScaledWindowInfo* out) {
  if (out) {
    *out = ScaledWindowInfo{};
  }
  if (!hwnd || !out) {
    return false;
  }

  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!cfg.enabled || !cfg.scaleValid || !(cfg.factor > 1.0)) {
    return false;
  }

  // Only infer when an external addon (dgVoodoo) has signalled that it is
  // loaded and actively performing scaling.  Without this gate, a missing
  // dgVoodoo installation would cause us to divide an unscaled client size
  // by the scale factor, producing wrong source dimensions and broken
  // cursor positioning.
  if (!IsAddonReady()) {
    return false;
  }

  static std::atomic<HWND> cached{nullptr};
  HWND best = cached.load(std::memory_order_acquire);
  if (!best || !IsWindow(best)) {
    best = FindBestTopLevelWindowForCurrentProcess();
    cached.store(best, std::memory_order_release);
  }

  // Only apply to the main/best top-level window (or its children).
  HWND root = GetAncestor(hwnd, GA_ROOT);
  if (!root) {
    root = hwnd;
  }
  if (best && root != best) {
    return false;
  }

  int dstW = 0, dstH = 0;
  if (!GetClientSize(hwnd, &dstW, &dstH)) {
    return false;
  }

  const int srcW = (int)std::llround((double)dstW / cfg.factor);
  const int srcH = (int)std::llround((double)dstH / cfg.factor);
  if (srcW <= 0 || srcH <= 0) {
    return false;
  }

  out->hwnd = hwnd;
  out->srcW = srcW;
  out->srcH = srcH;
  out->dstW = dstW;
  out->dstH = dstH;
  out->scaleFactor = cfg.factor;
  return true;
}

static bool TryGetScaledInfoForHwnd(HWND hwnd, ScaledWindowInfo* out) {
  if (!hwnd || !out) {
    return false;
  }

  // Try exact hwnd first.
  if (TryGetScaledWindow(hwnd, out)) {
    int cw = 0, ch = 0;
    if (GetClientSize(hwnd, &cw, &ch) && cw > 0 && ch > 0) {
      out->dstW = cw;
      out->dstH = ch;
    }
    return true;
  }

  // Common case: messages/calls target a child window; treat the root as the scaled window.
  HWND root = GetAncestor(hwnd, GA_ROOT);
  if (root && root != hwnd) {
    if (TryGetScaledWindow(root, out)) {
      int cw = 0, ch = 0;
      if (GetClientSize(root, &cw, &ch) && cw > 0 && ch > 0) {
        out->dstW = cw;
        out->dstH = ch;
      }
      out->hwnd = hwnd; // preserve original hwnd for callers
      return true;
    }
  }

  // Fall back to inference (dgVoodoo AddOn path / unknown scaler).
  return TryInferScaledWindow(hwnd, out);
}

static BOOL WINAPI Hook_GetCursorPos(LPPOINT lpPoint) {
  if (!g_fpGetCursorPos) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }
  if (!lpPoint) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const BOOL ok = g_fpGetCursorPos(lpPoint);
  if (!ok) {
    return ok;
  }

  ScaledWindowInfo info;
  RECT rcScreen{};
  if (!TryGetPrimaryScaledWindowInfo(&info, &rcScreen)) {
    return ok;
  }

  // Only remap while cursor is inside the scaled window client.
  const LONG x = lpPoint->x;
  const LONG y = lpPoint->y;
  if (x < rcScreen.left || y < rcScreen.top || x >= rcScreen.right || y >= rcScreen.bottom) {
    return ok;
  }

  const LONG physX = x - rcScreen.left;
  const LONG physY = y - rcScreen.top;
  const LONG logX = ScaleCoordRound(physX, info.srcW, info.dstW);
  const LONG logY = ScaleCoordRound(physY, info.srcH, info.dstH);

  lpPoint->x = rcScreen.left + logX;
  lpPoint->y = rcScreen.top + logY;

  if (IsMouseDebugEnabled()) {
    static std::atomic<uint32_t> n{0};
    const uint32_t c = n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 40 || (c % 5000) == 0) {
      Tracef(
          "GetCursorPos map screen=(%ld,%ld) phys=(%ld,%ld)->log=(%ld,%ld) src=%dx%d dst=%dx%d clientScreen=[%ld,%ld-%ld,%ld]",
          (long)x,
          (long)y,
          (long)physX,
          (long)physY,
          (long)logX,
          (long)logY,
          info.srcW,
          info.srcH,
          info.dstW,
          info.dstH,
          (long)rcScreen.left,
          (long)rcScreen.top,
          (long)rcScreen.right,
          (long)rcScreen.bottom);
    }
  }

  return ok;
}

static bool HookOne(void* target, void* detour, void** originalOut) {
  if (!target || !detour || !originalOut) {
    return false;
  }
  if (MH_CreateHook(target, detour, originalOut) != MH_OK) {
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    return false;
  }
  return true;
}

} // namespace

bool InstallMouseScaleHooks() {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!cfg.enabled || !cfg.scaleValid || !(cfg.factor > 1.0)) {
    return true;
  }

  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) {
    return true;
  }

  if (!AcquireMinHook()) {
    g_installed.store(false, std::memory_order_release);
    return false;
  }

  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    user32 = LoadLibraryW(L"user32.dll");
  }
  if (!user32) {
    g_installed.store(false, std::memory_order_release);
    ReleaseMinHook();
    return false;
  }

  void* pGetCursorPos = (void*)GetProcAddress(user32, "GetCursorPos");

  bool ok = true;
  // Enable GetCursorPos remapping (common input polling path).
  ok = ok && HookOne(pGetCursorPos, (void*)&Hook_GetCursorPos, (void**)&g_fpGetCursorPos);

  if (!ok) {
    // Best-effort disable anything we enabled.
    if (pGetCursorPos) (void)MH_DisableHook(pGetCursorPos);
    g_fpGetCursorPos = nullptr;
    g_installed.store(false, std::memory_order_release);
    ReleaseMinHook();
    return false;
  }

  if (IsMouseDebugEnabled()) {
    Tracef("mouse mapping hooks installed (GetCursorPos=on)");
  }

  return true;
}

void RemoveMouseScaleHooks() {
  if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32) {
    void* pGetCursorPos = (void*)GetProcAddress(user32, "GetCursorPos");
    if (g_fpGetCursorPos && pGetCursorPos) (void)MH_DisableHook(pGetCursorPos);
  }

  g_fpGetCursorPos = nullptr;

  ClearScaledWindows();

  if (IsMouseDebugEnabled()) {
    Tracef("mouse mapping hooks removed");
  }
  ReleaseMinHook();
}

} // namespace twinshim
