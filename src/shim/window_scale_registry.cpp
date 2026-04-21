#include "shim/window_scale_registry.h"

#include <mutex>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace twinshim {
namespace {

static std::mutex g_mu;
static std::unordered_map<HWND, ScaledWindowInfo> g_scaled;
static std::atomic<uint64_t> g_epoch{1};

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
  // Only set by the wrapper when launched with --debug.
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

static bool IsValidDims(int w, int h) {
  return w > 0 && h > 0;
}

} // namespace

void RegisterScaledWindow(HWND hwnd, int srcW, int srcH, int dstW, int dstH, double scaleFactor) {
  if (!hwnd) {
    return;
  }
  if (!IsValidDims(srcW, srcH) || !IsValidDims(dstW, dstH)) {
    return;
  }
  if (!(scaleFactor > 1.0)) {
    return;
  }

  ScaledWindowInfo info;
  info.hwnd = hwnd;
  info.srcW = srcW;
  info.srcH = srcH;
  info.dstW = dstW;
  info.dstH = dstH;
  info.scaleFactor = scaleFactor;

  std::lock_guard<std::mutex> lock(g_mu);
  g_scaled[hwnd] = info;
  g_epoch.fetch_add(1, std::memory_order_release);

  if (IsMouseDebugEnabled()) {
    Tracef("RegisterScaledWindow hwnd=%p src=%dx%d dst=%dx%d scale=%.3f", (void*)hwnd, srcW, srcH, dstW, dstH, scaleFactor);
  }
}

void UnregisterScaledWindow(HWND hwnd) {
  if (!hwnd) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const size_t n = g_scaled.erase(hwnd);
  if (n) {
    g_epoch.fetch_add(1, std::memory_order_release);
    if (IsMouseDebugEnabled()) {
      Tracef("UnregisterScaledWindow hwnd=%p", (void*)hwnd);
    }
  }
}

bool TryGetScaledWindow(HWND hwnd, ScaledWindowInfo* out) {
  if (out) {
    *out = ScaledWindowInfo{};
  }
  if (!hwnd || !out) {
    return false;
  }

  // Fast path: thread-local cache keyed by HWND + registry epoch.
  thread_local uint64_t tl_epoch = 0;
  thread_local HWND tl_hwnd = nullptr;
  thread_local ScaledWindowInfo tl_info{};
  const uint64_t curEpoch = g_epoch.load(std::memory_order_acquire);
  if (tl_epoch == curEpoch && tl_hwnd == hwnd) {
    *out = tl_info;
    return IsValidDims(out->srcW, out->srcH) && IsValidDims(out->dstW, out->dstH) && (out->scaleFactor > 1.0);
  }

  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_scaled.find(hwnd);
  if (it == g_scaled.end()) {
    return false;
  }
  *out = it->second;
  tl_epoch = curEpoch;
  tl_hwnd = hwnd;
  tl_info = *out;
  return IsValidDims(out->srcW, out->srcH) && IsValidDims(out->dstW, out->dstH) && (out->scaleFactor > 1.0);
}

void ClearScaledWindows() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_scaled.clear();
  g_epoch.fetch_add(1, std::memory_order_release);
  if (IsMouseDebugEnabled()) {
    Tracef("ClearScaledWindows");
  }
}

static std::atomic<bool> g_addonReady{false};

void NotifyAddonReady() {
  bool expected = false;
  if (g_addonReady.compare_exchange_strong(expected, true)) {
    if (IsMouseDebugEnabled()) {
      Tracef("addon ready signalled");
    }
  }
}

bool IsAddonReady() {
  return g_addonReady.load(std::memory_order_acquire);
}

extern "C" {

// NOTE: These exports intentionally use __cdecl (no WINAPI) to avoid stdcall
// name decoration on 32-bit builds (_Name@nn).  The addon DLL resolves them
// by undecorated name via GetProcAddress.
__declspec(dllexport) void TwinShim_RegisterScaledWindow(HWND hwnd, int srcW, int srcH, int dstW, int dstH, double scaleFactor) {
  twinshim::RegisterScaledWindow(hwnd, srcW, srcH, dstW, dstH, scaleFactor);
}

__declspec(dllexport) void TwinShim_UnregisterScaledWindow(HWND hwnd) {
  twinshim::UnregisterScaledWindow(hwnd);
}

__declspec(dllexport) void TwinShim_NotifyAddonReady() {
  twinshim::NotifyAddonReady();
}

}

} // namespace twinshim
