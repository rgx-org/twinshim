#include "shim/ddraw_surface_doubling.h"

#include "shim/surface_scale_config.h"

#include "shim/minhook_runtime.h"

#include <MinHook.h>

#include <windows.h>
#include <ddraw.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <mutex>
#include <string>

namespace hklmwrap {
namespace {

static constexpr double kMinScale = 1.1;
static constexpr double kMaxScale = 100.0;

static std::atomic<bool> g_active{false};

static std::atomic<bool> g_stopInitThread{false};
static HANDLE g_initThread = nullptr;

static std::atomic<bool> g_seenDDraw{false};
  static std::atomic<bool> g_loggedFirstCreateSurface{false};
  static std::atomic<uint32_t> g_flipCalls{0};
  static std::atomic<uint32_t> g_bltCalls{0};
  static std::atomic<uint32_t> g_bltFastCalls{0};
  static std::atomic<bool> g_loggedScaleViaFlip{false};
  static std::atomic<bool> g_loggedScaleViaBlt{false};
  static std::atomic<bool> g_loggedFilteredFallback{false};
  static std::atomic<bool> g_loggedLockScale{false};

static std::mutex g_stateMutex;
static HWND g_hwnd = nullptr;
static DWORD g_coopFlags = 0;
static bool g_resizedOnce = false;

static LPDIRECTDRAWSURFACE7 g_primary = nullptr;
static LPDIRECTDRAWSURFACE7 g_cachedBackbuffer = nullptr;
static DWORD g_cachedBackW = 0;
static DWORD g_cachedBackH = 0;

template <typename T>
static void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

static void* GetVtableEntry(void* obj, size_t index) {
  if (!obj) {
    return nullptr;
  }
  void** vtbl = *reinterpret_cast<void***>(obj);
  if (!vtbl) {
    return nullptr;
  }
  return vtbl[index];
}

static void TraceWrite(const char* text) {
  if (!text || !*text) {
    return;
  }
  OutputDebugStringA(text);

  wchar_t pipeBuf[512] = {};
  DWORD pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
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
  if (!fmt || !*fmt) {
    return;
  }

  char buf[1024];
  buf[0] = '\0';
  int used = std::snprintf(buf, sizeof(buf), "[shim:ddraw] ");
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

static bool IsScalingEnabled() {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  return cfg.enabled && cfg.scaleValid && cfg.factor >= kMinScale && cfg.factor <= kMaxScale;
}

static int CalcScaledInt(int base, double factor) {
  if (base <= 0) {
    return 0;
  }
  const double scaled = (double)base * factor;
  const double rounded = scaled + 0.5;
  if (rounded <= 0.0) {
    return 0;
  }
  if (rounded > (double)INT32_MAX) {
    return INT32_MAX;
  }
  return (int)rounded;
}

static bool GetClientSize(HWND hwnd, int* outW, int* outH) {
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
  if (!ClientToScreen(hwnd, &pt)) {
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

static bool SetWindowClientSize(HWND hwnd, int clientW, int clientH) {
  if (!hwnd || clientW <= 0 || clientH <= 0) {
    return false;
  }
  LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
  RECT rc{0, 0, clientW, clientH};
  if (!AdjustWindowRectEx(&rc, (DWORD)style, FALSE, (DWORD)exStyle)) {
    return false;
  }
  const int outerW = rc.right - rc.left;
  const int outerH = rc.bottom - rc.top;
  return SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

static bool IsFullscreenCoopFlags(DWORD flags) {
  return (flags & DDSCL_FULLSCREEN) != 0 || (flags & DDSCL_EXCLUSIVE) != 0;
}

static RECT MakeRectFromXYWH(int x, int y, int w, int h) {
  RECT rc{};
  rc.left = x;
  rc.top = y;
  rc.right = x + w;
  rc.bottom = y + h;
  return rc;
}

static bool RectIsOriginSize(const RECT* rc, LONG w, LONG h) {
  if (!rc) {
    return true;
  }
  return rc->left == 0 && rc->top == 0 && (rc->right - rc->left) == w && (rc->bottom - rc->top) == h;
}

static void TraceRectInline(const char* label, const RECT* rc) {
  if (!label) {
    label = "rc";
  }
  if (!rc) {
    Tracef("%s=<null>", label);
    return;
  }
  Tracef("%s=[%ld,%ld,%ld,%ld] (w=%ld h=%ld)",
         label,
         (long)rc->left,
         (long)rc->top,
         (long)rc->right,
         (long)rc->bottom,
         (long)(rc->right - rc->left),
         (long)(rc->bottom - rc->top));
}

struct PixelFormatInfo {
  uint32_t rMask = 0;
  uint32_t gMask = 0;
  uint32_t bMask = 0;
  uint32_t aMask = 0;
  int rShift = 0;
  int gShift = 0;
  int bShift = 0;
  int aShift = 0;
  int rBits = 0;
  int gBits = 0;
  int bBits = 0;
  int aBits = 0;
  int bytesPerPixel = 0;
};

static int CountBits(uint32_t v) {
  int c = 0;
  while (v) {
    c += (int)(v & 1u);
    v >>= 1;
  }
  return c;
}

static int CountTrailingZeros(uint32_t v) {
  if (v == 0) {
    return 0;
  }
  int c = 0;
  while ((v & 1u) == 0) {
    c++;
    v >>= 1;
  }
  return c;
}

static bool GetPixelFormatInfoFromSurface(LPDIRECTDRAWSURFACE7 surf, PixelFormatInfo* out) {
  if (!surf || !out) {
    return false;
  }
  DDSURFACEDESC2 sd{};
  sd.dwSize = sizeof(sd);
  if (FAILED(surf->GetSurfaceDesc(&sd))) {
    return false;
  }
  if ((sd.ddpfPixelFormat.dwFlags & DDPF_RGB) == 0) {
    return false;
  }

  PixelFormatInfo info;
  info.rMask = sd.ddpfPixelFormat.dwRBitMask;
  info.gMask = sd.ddpfPixelFormat.dwGBitMask;
  info.bMask = sd.ddpfPixelFormat.dwBBitMask;
  info.aMask = (sd.ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) ? sd.ddpfPixelFormat.dwRGBAlphaBitMask : 0;
  info.rShift = CountTrailingZeros(info.rMask);
  info.gShift = CountTrailingZeros(info.gMask);
  info.bShift = CountTrailingZeros(info.bMask);
  info.aShift = CountTrailingZeros(info.aMask);
  info.rBits = CountBits(info.rMask);
  info.gBits = CountBits(info.gMask);
  info.bBits = CountBits(info.bMask);
  info.aBits = CountBits(info.aMask);

  const uint32_t bpp = sd.ddpfPixelFormat.dwRGBBitCount;
  if (bpp == 16) {
    info.bytesPerPixel = 2;
  } else if (bpp == 32) {
    info.bytesPerPixel = 4;
  } else {
    return false;
  }

  *out = info;
  return true;
}

static uint8_t ExpandTo8(uint32_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 8) {
    return (uint8_t)std::min<uint32_t>(255u, v);
  }
  const uint32_t maxv = (1u << (uint32_t)bits) - 1u;
  return (uint8_t)((v * 255u + (maxv / 2u)) / maxv);
}

static uint32_t CompressFrom8(uint8_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 8) {
    return (uint32_t)v;
  }
  const uint32_t maxv = (1u << (uint32_t)bits) - 1u;
  return (uint32_t)((uint32_t)v * maxv + 127u) / 255u;
}

static void UnpackRGBA(const PixelFormatInfo& fmt, uint32_t px, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
  const uint32_t rv = (fmt.rMask ? ((px & fmt.rMask) >> (uint32_t)fmt.rShift) : 0);
  const uint32_t gv = (fmt.gMask ? ((px & fmt.gMask) >> (uint32_t)fmt.gShift) : 0);
  const uint32_t bv = (fmt.bMask ? ((px & fmt.bMask) >> (uint32_t)fmt.bShift) : 0);
  const uint32_t av = (fmt.aMask ? ((px & fmt.aMask) >> (uint32_t)fmt.aShift) : 255u);
  if (r) {
    *r = ExpandTo8(rv, fmt.rBits);
  }
  if (g) {
    *g = ExpandTo8(gv, fmt.gBits);
  }
  if (b) {
    *b = ExpandTo8(bv, fmt.bBits);
  }
  if (a) {
    *a = fmt.aMask ? ExpandTo8(av, fmt.aBits) : 255;
  }
}

static uint32_t PackRGBA(const PixelFormatInfo& fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  uint32_t out = 0;
  out |= (CompressFrom8(r, fmt.rBits) << (uint32_t)fmt.rShift) & fmt.rMask;
  out |= (CompressFrom8(g, fmt.gBits) << (uint32_t)fmt.gShift) & fmt.gMask;
  out |= (CompressFrom8(b, fmt.bBits) << (uint32_t)fmt.bShift) & fmt.bMask;
  if (fmt.aMask) {
    out |= (CompressFrom8(a, fmt.aBits) << (uint32_t)fmt.aShift) & fmt.aMask;
  }
  return out;
}

static uint32_t ReadPixel(const uint8_t* base, int pitch, int x, int y, int bpp) {
  const uint8_t* p = base + (ptrdiff_t)y * pitch + (ptrdiff_t)x * bpp;
  if (bpp == 4) {
    return *(const uint32_t*)p;
  }
  return (uint32_t)(*(const uint16_t*)p);
}

static void WritePixel(uint8_t* base, int pitch, int x, int y, int bpp, uint32_t px) {
  uint8_t* p = base + (ptrdiff_t)y * pitch + (ptrdiff_t)x * bpp;
  if (bpp == 4) {
    *(uint32_t*)p = px;
  } else {
    *(uint16_t*)p = (uint16_t)px;
  }
}

static HRESULT TryScaleViaLockBilinear(LPDIRECTDRAWSURFACE7 dstSurf,
                                      const RECT& dstRc,
                                      LPDIRECTDRAWSURFACE7 srcSurf,
                                      const RECT& srcRc) {
  if (!dstSurf || !srcSurf) {
    return E_INVALIDARG;
  }
  // Clamp destination to the primary surface bounds (window can be partially off-screen).
  DDSURFACEDESC2 dBounds{};
  dBounds.dwSize = sizeof(dBounds);
  if (FAILED(dstSurf->GetSurfaceDesc(&dBounds)) || dBounds.dwWidth == 0 || dBounds.dwHeight == 0) {
    return E_FAIL;
  }
  RECT clampedDst = dstRc;
  clampedDst.left = std::max<LONG>(0, clampedDst.left);
  clampedDst.top = std::max<LONG>(0, clampedDst.top);
  clampedDst.right = std::min<LONG>((LONG)dBounds.dwWidth, clampedDst.right);
  clampedDst.bottom = std::min<LONG>((LONG)dBounds.dwHeight, clampedDst.bottom);

  const int dstW = (int)(clampedDst.right - clampedDst.left);
  const int dstH = (int)(clampedDst.bottom - clampedDst.top);
  const int srcW = (int)(srcRc.right - srcRc.left);
  const int srcH = (int)(srcRc.bottom - srcRc.top);
  if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) {
    return E_INVALIDARG;
  }

  PixelFormatInfo srcFmt;
  PixelFormatInfo dstFmt;
  if (!GetPixelFormatInfoFromSurface(srcSurf, &srcFmt) || !GetPixelFormatInfoFromSurface(dstSurf, &dstFmt)) {
    return E_FAIL;
  }
  if (srcFmt.bytesPerPixel != dstFmt.bytesPerPixel ||
      srcFmt.rMask != dstFmt.rMask ||
      srcFmt.gMask != dstFmt.gMask ||
      srcFmt.bMask != dstFmt.bMask ||
      srcFmt.aMask != dstFmt.aMask) {
    return E_FAIL;
  }

  DDSURFACEDESC2 ssd{};
  ssd.dwSize = sizeof(ssd);
  DDSURFACEDESC2 dsd{};
  dsd.dwSize = sizeof(dsd);

  HRESULT hrS = srcSurf->Lock(nullptr, &ssd, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
  if (FAILED(hrS) || !ssd.lpSurface || ssd.lPitch <= 0) {
    if (SUCCEEDED(hrS)) {
      srcSurf->Unlock(nullptr);
    }
    return FAILED(hrS) ? hrS : E_FAIL;
  }

  HRESULT hrD = dstSurf->Lock(nullptr, &dsd, DDLOCK_WAIT, nullptr);
  if (FAILED(hrD) || !dsd.lpSurface || dsd.lPitch <= 0) {
    srcSurf->Unlock(nullptr);
    if (SUCCEEDED(hrD)) {
      dstSurf->Unlock(nullptr);
    }
    return FAILED(hrD) ? hrD : E_FAIL;
  }

  const int bpp = srcFmt.bytesPerPixel;
  const uint8_t* sBase = (const uint8_t*)ssd.lpSurface;
  uint8_t* dBase = (uint8_t*)dsd.lpSurface;
  const int sPitch = (int)ssd.lPitch;
  const int dPitch = (int)dsd.lPitch;

  // Fixed-point mapping: clamped dst -> src in 16.16.
  const int32_t xStep = (int32_t)(((int64_t)srcW << 16) / dstW);
  const int32_t yStep = (int32_t)(((int64_t)srcH << 16) / dstH);

  for (int y = 0; y < dstH; y++) {
    const int32_t sy16 = (int32_t)((int64_t)y * yStep);
    int sy = (sy16 >> 16);
    int fy = sy16 & 0xFFFF;
    if (sy < 0) {
      sy = 0;
      fy = 0;
    }
    if (sy >= srcH - 1) {
      sy = std::max(0, srcH - 2);
      fy = 0xFFFF;
    }

    for (int x = 0; x < dstW; x++) {
      const int32_t sx16 = (int32_t)((int64_t)x * xStep);
      int sx = (sx16 >> 16);
      int fx = sx16 & 0xFFFF;
      if (sx < 0) {
        sx = 0;
        fx = 0;
      }
      if (sx >= srcW - 1) {
        sx = std::max(0, srcW - 2);
        fx = 0xFFFF;
      }

      const int sX0 = srcRc.left + sx;
      const int sY0 = srcRc.top + sy;

      const uint32_t p00 = ReadPixel(sBase, sPitch, sX0, sY0, bpp);
      const uint32_t p10 = ReadPixel(sBase, sPitch, sX0 + 1, sY0, bpp);
      const uint32_t p01 = ReadPixel(sBase, sPitch, sX0, sY0 + 1, bpp);
      const uint32_t p11 = ReadPixel(sBase, sPitch, sX0 + 1, sY0 + 1, bpp);

      uint8_t r00, g00, b00, a00;
      uint8_t r10, g10, b10, a10;
      uint8_t r01, g01, b01, a01;
      uint8_t r11, g11, b11, a11;
      UnpackRGBA(srcFmt, p00, &r00, &g00, &b00, &a00);
      UnpackRGBA(srcFmt, p10, &r10, &g10, &b10, &a10);
      UnpackRGBA(srcFmt, p01, &r01, &g01, &b01, &a01);
      UnpackRGBA(srcFmt, p11, &r11, &g11, &b11, &a11);

      const int invFx = 0x10000 - fx;
      const int invFy = 0x10000 - fy;

      auto lerp2 = [&](int c00, int c10, int c01, int c11) -> uint8_t {
        const int top = (c00 * invFx + c10 * fx) >> 16;
        const int bot = (c01 * invFx + c11 * fx) >> 16;
        const int out = (top * invFy + bot * fy) >> 16;
        return (uint8_t)std::clamp(out, 0, 255);
      };

      const uint8_t r = lerp2(r00, r10, r01, r11);
      const uint8_t g = lerp2(g00, g10, g01, g11);
      const uint8_t b = lerp2(b00, b10, b01, b11);
      const uint8_t a = lerp2(a00, a10, a01, a11);
      const uint32_t outPx = PackRGBA(dstFmt, r, g, b, a);

      const int dX = clampedDst.left + x;
      const int dY = clampedDst.top + y;
      WritePixel(dBase, dPitch, dX, dY, bpp, outPx);
    }
  }

  dstSurf->Unlock(nullptr);
  srcSurf->Unlock(nullptr);
  return DD_OK;
}

// --- Originals / hook targets ---
using DirectDrawCreate_t = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
using DirectDrawCreateEx_t = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);

static DirectDrawCreate_t g_fpDirectDrawCreate = nullptr;
static DirectDrawCreateEx_t g_fpDirectDrawCreateEx = nullptr;

using DD7_SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAW7, HWND, DWORD);
using DD7_CreateSurface_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAW7, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*, IUnknown*);

static DD7_SetCooperativeLevel_t g_fpDD7_SetCooperativeLevel = nullptr;
static DD7_CreateSurface_t g_fpDD7_CreateSurface = nullptr;

using DDS7_Flip_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7, DWORD);
static DDS7_Flip_t g_fpDDS7_Flip = nullptr;
  using DDS7_Blt_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAWSURFACE7, LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDBLTFX);
  static DDS7_Blt_t g_fpDDS7_Blt = nullptr;
  using DDS7_BltFast_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAWSURFACE7, DWORD, DWORD, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);
  static DDS7_BltFast_t g_fpDDS7_BltFast = nullptr;

static std::atomic<void*> g_targetDD7_SetCooperativeLevel{nullptr};
static std::atomic<void*> g_targetDD7_CreateSurface{nullptr};
static std::atomic<void*> g_targetDDS7_Flip{nullptr};
  static std::atomic<void*> g_targetDDS7_Blt{nullptr};
  static std::atomic<void*> g_targetDDS7_BltFast{nullptr};

static HRESULT WINAPI Hook_DirectDrawCreateEx(GUID* guid, LPVOID* dd, REFIID iid, IUnknown* unk);
static HRESULT WINAPI Hook_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* unk);

static bool EnsureDD7MethodHooksInstalled(LPDIRECTDRAW7 dd7);

static bool CreateHookApiTypedWithFallback(LPCSTR procName, LPVOID detour, LPVOID* original) {
  static constexpr LPCWSTR kModules[] = {
      L"ddraw",
      L"ddraw.dll",
  };

  bool hookedAny = false;
  for (LPCWSTR moduleName : kModules) {
    if (*original == nullptr) {
      hookedAny |= (MH_CreateHookApi(moduleName, procName, detour, original) == MH_OK);
    } else {
      LPVOID tmp = nullptr;
      hookedAny |= (MH_CreateHookApi(moduleName, procName, detour, &tmp) == MH_OK);
    }
  }
  return hookedAny;
}

static HRESULT WINAPI Hook_DirectDrawCreateEx(GUID* guid, LPVOID* dd, REFIID iid, IUnknown* unk) {
  if (!g_fpDirectDrawCreateEx) {
    return DDERR_GENERIC;
  }

  HRESULT hr = g_fpDirectDrawCreateEx(guid, dd, iid, unk);
  if (FAILED(hr) || !dd || !*dd) {
    return hr;
  }

  LPDIRECTDRAW7 dd7 = nullptr;
  if (IsEqualGUID(iid, IID_IDirectDraw7)) {
    dd7 = (LPDIRECTDRAW7)(*dd);
    if (dd7) {
      dd7->AddRef();
    }
  } else {
    IUnknown* unkIf = (IUnknown*)(*dd);
    if (unkIf) {
      (void)unkIf->QueryInterface(IID_IDirectDraw7, (void**)&dd7);
    }
  }

  if (dd7) {
    g_seenDDraw.store(true, std::memory_order_release);
    Tracef("DirectDrawCreateEx -> IDirectDraw7=%p", (void*)dd7);
    (void)EnsureDD7MethodHooksInstalled(dd7);
    dd7->Release();
  }

  return hr;
}

static HRESULT WINAPI Hook_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* unk) {
  if (!g_fpDirectDrawCreate) {
    return DDERR_GENERIC;
  }

  HRESULT hr = g_fpDirectDrawCreate(guid, out, unk);
  if (FAILED(hr) || !out || !*out) {
    return hr;
  }

  IUnknown* unkIf = (IUnknown*)(*out);
  LPDIRECTDRAW7 dd7 = nullptr;
  if (unkIf) {
    (void)unkIf->QueryInterface(IID_IDirectDraw7, (void**)&dd7);
  }
  if (dd7) {
    g_seenDDraw.store(true, std::memory_order_release);
    Tracef("DirectDrawCreate -> IDirectDraw7=%p", (void*)dd7);
    (void)EnsureDD7MethodHooksInstalled(dd7);
    dd7->Release();
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DD7_SetCooperativeLevel(LPDIRECTDRAW7 self, HWND hwnd, DWORD flags) {
  if (g_fpDD7_SetCooperativeLevel) {
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      g_hwnd = hwnd;
      g_coopFlags = flags;
    }
    Tracef("SetCooperativeLevel hwnd=%p flags=0x%08lX fullscreen=%d", (void*)hwnd, (unsigned long)flags, IsFullscreenCoopFlags(flags) ? 1 : 0);
    return g_fpDD7_SetCooperativeLevel(self, hwnd, flags);
  }
  return DDERR_GENERIC;
}

static void MaybeResizeAfterPrimaryCreated(LPDIRECTDRAWSURFACE7 primary) {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !primary) {
    return;
  }

  HWND hwnd = nullptr;
  DWORD coop = 0;
  bool doResize = false;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    hwnd = g_hwnd;
    coop = g_coopFlags;
    doResize = !g_resizedOnce;
    if (doResize) {
      g_resizedOnce = true;
    }
  }

  if (!doResize || !hwnd || IsFullscreenCoopFlags(coop)) {
    return;
  }

  // Prefer the attached backbuffer size as the true "render size" baseline.
  DWORD baseW = 0;
  DWORD baseH = 0;
  {
    DDSCAPS2 caps{};
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    LPDIRECTDRAWSURFACE7 back = nullptr;
    HRESULT hr = primary->GetAttachedSurface(&caps, &back);
    if (SUCCEEDED(hr) && back) {
      DDSURFACEDESC2 sd{};
      sd.dwSize = sizeof(sd);
      if (SUCCEEDED(back->GetSurfaceDesc(&sd))) {
        baseW = sd.dwWidth;
        baseH = sd.dwHeight;
      }
      SafeRelease(back);
    }
  }

  if (baseW == 0 || baseH == 0) {
    int cw = 0, ch = 0;
    if (GetClientSize(hwnd, &cw, &ch)) {
      baseW = (DWORD)cw;
      baseH = (DWORD)ch;
      Tracef("scale baseline from client (backbuffer unknown): %lux%lu", (unsigned long)baseW, (unsigned long)baseH);
    }
  } else {
    Tracef("scale baseline from backbuffer: %lux%lu", (unsigned long)baseW, (unsigned long)baseH);
  }

  if (baseW == 0 || baseH == 0) {
    Tracef("scale resize skipped: baseline size unknown");
    return;
  }

  const int newW = CalcScaledInt((int)baseW, cfg.factor);
  const int newH = CalcScaledInt((int)baseH, cfg.factor);
  const bool ok = SetWindowClientSize(hwnd, newW, newH);
  Tracef("scale resize after primary created: %lux%lu -> %dx%d (scale=%.3f, %s)",
         (unsigned long)baseW,
         (unsigned long)baseH,
         newW,
         newH,
         cfg.factor,
         ok ? "ok" : "failed");
}

static bool RefreshBackbufferCacheFromPrimary(LPDIRECTDRAWSURFACE7 primary, LPDIRECTDRAWSURFACE7* outBack, DWORD* outW, DWORD* outH) {
  if (outBack) {
    *outBack = nullptr;
  }
  if (outW) {
    *outW = 0;
  }
  if (outH) {
    *outH = 0;
  }
  if (!primary) {
    return false;
  }
  DDSCAPS2 caps{};
  caps.dwCaps = DDSCAPS_BACKBUFFER;
  LPDIRECTDRAWSURFACE7 back = nullptr;
  HRESULT hr = primary->GetAttachedSurface(&caps, &back);
  if (FAILED(hr) || !back) {
    SafeRelease(back);
    return false;
  }

  DDSURFACEDESC2 sd{};
  sd.dwSize = sizeof(sd);
  hr = back->GetSurfaceDesc(&sd);
  if (FAILED(hr) || sd.dwWidth == 0 || sd.dwHeight == 0) {
    SafeRelease(back);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    SafeRelease(g_cachedBackbuffer);
    g_cachedBackbuffer = back;
    g_cachedBackbuffer->AddRef();
    g_cachedBackW = sd.dwWidth;
    g_cachedBackH = sd.dwHeight;
  }

  if (outBack) {
    *outBack = back;
  } else {
    // Caller didn't request the ref; release our local reference.
    SafeRelease(back);
  }
  if (outW) {
    *outW = sd.dwWidth;
  }
  if (outH) {
    *outH = sd.dwHeight;
  }
  // If outBack != nullptr, caller owns the returned ref from GetAttachedSurface.
  return true;
}

static HRESULT STDMETHODCALLTYPE Hook_DD7_CreateSurface(LPDIRECTDRAW7 self,
                                                       LPDDSURFACEDESC2 desc,
                                                       LPDIRECTDRAWSURFACE7* outSurf,
                                                       IUnknown* unkOuter);

static HRESULT STDMETHODCALLTYPE Hook_DDS7_Flip(LPDIRECTDRAWSURFACE7 primary, LPDIRECTDRAWSURFACE7 targetOverride, DWORD flags);
static HRESULT STDMETHODCALLTYPE Hook_DDS7_Blt(LPDIRECTDRAWSURFACE7 self,
                                              LPRECT dst,
                                              LPDIRECTDRAWSURFACE7 src,
                                              LPRECT srcRect,
                                              DWORD flags,
                                              LPDDBLTFX fx);
static HRESULT STDMETHODCALLTYPE Hook_DDS7_BltFast(LPDIRECTDRAWSURFACE7 self,
                                                  DWORD x,
                                                  DWORD y,
                                                  LPDIRECTDRAWSURFACE7 src,
                                                  LPRECT srcRect,
                                                  DWORD trans);

static void TraceRect(const char* label, const RECT* rc) {
  if (!label) {
    label = "rc";
  }
  if (!rc) {
    Tracef("%s=<null>", label);
    return;
  }
  Tracef("%s=[%ld,%ld,%ld,%ld]", label, (long)rc->left, (long)rc->top, (long)rc->right, (long)rc->bottom);
}

static bool EnsureCreateSurfaceHookInstalledFromDD7(LPDIRECTDRAW7 dd7) {
  if (!dd7) {
    return false;
  }
  // IDirectDraw7 vtable index: CreateSurface = 6
  void* tgt = GetVtableEntry(dd7, 6);
  if (!tgt) {
    Tracef("CreateSurface vtbl entry missing (dd7=%p)", (void*)dd7);
    return false;
  }
  void* expected = nullptr;
  if (!g_targetDD7_CreateSurface.compare_exchange_strong(expected, tgt)) {
    return true;
  }

  if (MH_CreateHook(tgt, reinterpret_cast<LPVOID>(&Hook_DD7_CreateSurface), reinterpret_cast<LPVOID*>(&g_fpDD7_CreateSurface)) != MH_OK) {
    g_targetDD7_CreateSurface.store(nullptr);
    return false;
  }
  if (MH_EnableHook(tgt) != MH_OK) {
    (void)MH_RemoveHook(tgt);
    g_targetDD7_CreateSurface.store(nullptr);
    g_fpDD7_CreateSurface = nullptr;
    return false;
  }
  Tracef("hooked IDirectDraw7::CreateSurface @ %p", tgt);
  return true;
}

static bool EnsureDD7MethodHooksInstalled(LPDIRECTDRAW7 dd7) {
  if (!dd7) {
    return false;
  }

  // IDirectDraw7 vtable index: SetCooperativeLevel = 20
  void* tgt = GetVtableEntry(dd7, 20);
  if (!tgt) {
    Tracef("SetCooperativeLevel vtbl entry missing (dd7=%p)", (void*)dd7);
  }
  void* expected = nullptr;
  if (g_targetDD7_SetCooperativeLevel.compare_exchange_strong(expected, tgt)) {
    if (MH_CreateHook(tgt, reinterpret_cast<LPVOID>(&Hook_DD7_SetCooperativeLevel), reinterpret_cast<LPVOID*>(&g_fpDD7_SetCooperativeLevel)) == MH_OK) {
      (void)MH_EnableHook(tgt);
      Tracef("hooked IDirectDraw7::SetCooperativeLevel @ %p", tgt);
    } else {
      g_targetDD7_SetCooperativeLevel.store(nullptr);
    }
  }

  (void)EnsureCreateSurfaceHookInstalledFromDD7(dd7);
  return true;
}

static bool InstallDDrawSurfaceDoublingHooksOnce() {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled()) {
    if (cfg.scaleSpecified && !cfg.scaleValid) {
      Tracef("surface scaling disabled (--scale invalid; raw='%ls')", cfg.scaleRaw.c_str());
    } else {
      Tracef("surface scaling disabled (no valid --scale provided)");
    }
    return true;
  }

  if (cfg.methodSpecified && !cfg.methodValid) {
    Tracef("surface scaling: invalid --scale-method '%ls' -> defaulting to point", cfg.methodRaw.c_str());
  }
  Tracef("surface scaling enabled (scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
  if (cfg.method == SurfaceScaleMethod::kBicubic) {
    Tracef("note: DirectDraw path uses GDI HALFTONE StretchBlt for non-point filtering (bicubic is approximated)");
  }

  if (!AcquireMinHook()) {
    Tracef("AcquireMinHook failed");
    return false;
  }

  bool ok = false;
  ok |= CreateHookApiTypedWithFallback("DirectDrawCreateEx", reinterpret_cast<LPVOID>(&Hook_DirectDrawCreateEx), reinterpret_cast<LPVOID*>(&g_fpDirectDrawCreateEx));

  // DirectDrawCreate is older; still hook it as a fallback.
  (void)CreateHookApiTypedWithFallback("DirectDrawCreate", reinterpret_cast<LPVOID>(&Hook_DirectDrawCreate), reinterpret_cast<LPVOID*>(&g_fpDirectDrawCreate));

  if (!ok) {
    Tracef("failed to hook DirectDrawCreateEx exports");
    ReleaseMinHook();
    return false;
  }

  // Install the actual DD7 CreateSurface hook once we have a DD7 vtable.
  // We'll do this lazily: once DirectDrawCreate(Ex) returns an object, we hook its vtable function addresses.
  // Enable hooks broadly.
  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    Tracef("MH_EnableHook(MH_ALL_HOOKS) failed");
    ReleaseMinHook();
    return false;
  }
  Tracef("ddraw export hooks installed");
  return true;
}

static DWORD WINAPI DDrawInitThreadProc(LPVOID) {
  // Wait up to ~10 minutes for ddraw.dll to appear.
  for (int i = 0; i < 12000 && !g_stopInitThread.load(std::memory_order_acquire); i++) {
    if ((i % 40) == 0) {
      HMODULE h = GetModuleHandleW(L"ddraw.dll");
      if (h) {
        Tracef("module loaded: ddraw.dll @ %p", (void*)h);
      }
    }
    if (GetModuleHandleW(L"ddraw.dll") != nullptr || GetModuleHandleW(L"ddraw") != nullptr) {
      break;
    }
    Sleep(50);
  }

  if (!g_stopInitThread.load(std::memory_order_acquire)) {
    const bool ok = InstallDDrawSurfaceDoublingHooksOnce();
    Tracef("init thread finished (ok=%d)", ok ? 1 : 0);
  }
  return 0;
}

static HRESULT STDMETHODCALLTYPE Hook_DD7_CreateSurface(LPDIRECTDRAW7 self,
                                                       LPDDSURFACEDESC2 desc,
                                                       LPDIRECTDRAWSURFACE7* outSurf,
                                                       IUnknown* unkOuter) {
  if (!g_fpDD7_CreateSurface) {
    return DDERR_GENERIC;
  }
  // No-op if scaling disabled; we still hook CreateSurface to discover the primary.

  {
    bool expected = false;
    if (g_loggedFirstCreateSurface.compare_exchange_strong(expected, true)) {
      DWORD flags = desc ? desc->dwFlags : 0;
      DWORD caps = (desc && (flags & DDSD_CAPS)) ? desc->ddsCaps.dwCaps : 0;
      DWORD w = (desc && (flags & DDSD_WIDTH)) ? desc->dwWidth : 0;
      DWORD h = (desc && (flags & DDSD_HEIGHT)) ? desc->dwHeight : 0;
      Tracef("CreateSurface first call self=%p desc=%p flags=0x%08lX caps=0x%08lX w=%lu h=%lu",
             (void*)self,
             (void*)desc,
             (unsigned long)flags,
             (unsigned long)caps,
             (unsigned long)w,
             (unsigned long)h);
    }
  }

  DDSURFACEDESC2 localDesc{};
  LPDDSURFACEDESC2 descToUse = desc;
  if (desc) {
    localDesc = *desc;
    descToUse = &localDesc;
  }

  // NOTE: We intentionally do NOT modify surface creation parameters in this path.
  // Doing so is highly app-specific and can crash dgVoodoo/DirectDraw wrappers.

  HRESULT hr = g_fpDD7_CreateSurface(self, descToUse, outSurf, unkOuter);
  if (FAILED(hr) || !outSurf || !*outSurf) {
    return hr;
  }

  LPDIRECTDRAWSURFACE7 surf = *outSurf;
  if (!surf) {
    return hr;
  }

  // Determine primary surface status from the created surface, not from input desc flags.
  DDSURFACEDESC2 createdDesc{};
  createdDesc.dwSize = sizeof(createdDesc);
  HRESULT hrDesc = surf->GetSurfaceDesc(&createdDesc);
  if (FAILED(hrDesc)) {
    Tracef("CreateSurface: GetSurfaceDesc failed hr=0x%08lX surf=%p", (unsigned long)hrDesc, (void*)surf);
    return hr;
  }

  const bool isPrimary = (createdDesc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
  if (isPrimary) {
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      SafeRelease(g_primary);
      g_primary = surf;
      g_primary->AddRef();

      SafeRelease(g_cachedBackbuffer);
      g_cachedBackW = 0;
      g_cachedBackH = 0;
    }

    Tracef("primary surface created=%p", (void*)surf);

    // Resize only after primary/backbuffer exist so the app keeps rendering at original size.
    MaybeResizeAfterPrimaryCreated(surf);

    // Prime backbuffer cache.
    (void)RefreshBackbufferCacheFromPrimary(surf, nullptr, nullptr, nullptr);

    // Hook common presentation-related methods on the primary surface.
    // IDirectDrawSurface7 vtable indices: Blt=5, BltFast=7, Flip=11
    void* tgtBlt = GetVtableEntry(surf, 5);
    if (tgtBlt) {
      void* expectedBlt = nullptr;
      if (g_targetDDS7_Blt.compare_exchange_strong(expectedBlt, tgtBlt)) {
        if (MH_CreateHook(tgtBlt, reinterpret_cast<LPVOID>(&Hook_DDS7_Blt), reinterpret_cast<LPVOID*>(&g_fpDDS7_Blt)) == MH_OK) {
          (void)MH_EnableHook(tgtBlt);
          Tracef("hooked IDirectDrawSurface7::Blt @ %p", tgtBlt);
        } else {
          g_targetDDS7_Blt.store(nullptr);
        }
      }
    }
    void* tgtBltFast = GetVtableEntry(surf, 7);
    if (tgtBltFast) {
      void* expectedBltFast = nullptr;
      if (g_targetDDS7_BltFast.compare_exchange_strong(expectedBltFast, tgtBltFast)) {
        if (MH_CreateHook(tgtBltFast, reinterpret_cast<LPVOID>(&Hook_DDS7_BltFast), reinterpret_cast<LPVOID*>(&g_fpDDS7_BltFast)) == MH_OK) {
          (void)MH_EnableHook(tgtBltFast);
          Tracef("hooked IDirectDrawSurface7::BltFast @ %p", tgtBltFast);
        } else {
          g_targetDDS7_BltFast.store(nullptr);
        }
      }
    }
    void* tgtFlip = GetVtableEntry(surf, 11);
    if (!tgtFlip) {
      Tracef("primary surface Flip vtbl entry missing (surf=%p)", (void*)surf);
      return hr;
    }
    void* expectedHook = nullptr;
    if (g_targetDDS7_Flip.compare_exchange_strong(expectedHook, tgtFlip)) {
      if (MH_CreateHook(tgtFlip, reinterpret_cast<LPVOID>(&Hook_DDS7_Flip), reinterpret_cast<LPVOID*>(&g_fpDDS7_Flip)) == MH_OK) {
        (void)MH_EnableHook(tgtFlip);
        Tracef("hooked IDirectDrawSurface7::Flip @ %p", tgtFlip);
      } else {
        g_targetDDS7_Flip.store(nullptr);
      }
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DDS7_Flip(LPDIRECTDRAWSURFACE7 primary, LPDIRECTDRAWSURFACE7 targetOverride, DWORD flags) {
    const uint32_t n = g_flipCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) {
      Tracef("Flip call #%lu primary=%p flags=0x%08lX", (unsigned long)n, (void*)primary, (unsigned long)flags);
    }
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !g_fpDDS7_Flip) {
    return g_fpDDS7_Flip ? g_fpDDS7_Flip(primary, targetOverride, flags) : DDERR_GENERIC;
  }

  HWND hwnd = nullptr;
  DWORD coop = 0;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    hwnd = g_hwnd;
    coop = g_coopFlags;
  }
  if (!hwnd || IsFullscreenCoopFlags(coop)) {
    return g_fpDDS7_Flip(primary, targetOverride, flags);
  }

  // Prefer cached backbuffer to avoid per-frame GetAttachedSurface/GetSurfaceDesc overhead.
  LPDIRECTDRAWSURFACE7 back = nullptr;
  DWORD srcW = 0;
  DWORD srcH = 0;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    back = g_cachedBackbuffer;
    srcW = g_cachedBackW;
    srcH = g_cachedBackH;
    if (back) {
      back->AddRef();
    }
  }

  if (!back || srcW == 0 || srcH == 0) {
    SafeRelease(back);
    if (!RefreshBackbufferCacheFromPrimary(primary, &back, &srcW, &srcH)) {
      SafeRelease(back);
      return g_fpDDS7_Flip(primary, targetOverride, flags);
    }
  }

  int clientW = 0, clientH = 0;
  if (!GetClientSize(hwnd, &clientW, &clientH)) {
    SafeRelease(back);
    return g_fpDDS7_Flip(primary, targetOverride, flags);
  }

  // Window resizing (if any) is handled once after primary/backbuffer exist.

  RECT src{0, 0, (LONG)srcW, (LONG)srcH};
  RECT dst{};
  if (!GetClientRectInScreen(hwnd, &dst)) {
    // Fallback (should be rare). Note: primary surface blits are normally screen-space.
    dst = MakeRectFromXYWH(0, 0, clientW, clientH);
  }

  HRESULT hr = DDERR_GENERIC;
  if (cfg.method == SurfaceScaleMethod::kPoint) {
    // Try to avoid introducing extra latency: don't force DDBLT_WAIT.
    // If the blit can't be scheduled immediately, do a one-time blocking fallback
    // to avoid intermittent unscaled presents.
    // Use original Blt pointer (avoid re-entering our Blt hook).
    if (g_fpDDS7_Blt) {
      hr = g_fpDDS7_Blt(primary, &dst, back, &src, DDBLT_DONOTWAIT, nullptr);
    } else {
      hr = primary->Blt(&dst, back, &src, DDBLT_DONOTWAIT, nullptr);
    }
    if (hr == DDERR_WASSTILLDRAWING) {
      if (g_fpDDS7_Blt) {
        hr = g_fpDDS7_Blt(primary, &dst, back, &src, DDBLT_WAIT, nullptr);
      } else {
        hr = primary->Blt(&dst, back, &src, DDBLT_WAIT, nullptr);
      }
    }
  } else {
    // GDI StretchBlt path for smoother filtering.
    HDC hdcDst = nullptr;
    HDC hdcSrc = nullptr;
    HRESULT hrDst = primary->GetDC(&hdcDst);
    HRESULT hrSrc = back->GetDC(&hdcSrc);
    if (FAILED(hrDst) || FAILED(hrSrc) || !hdcDst || !hdcSrc) {
      if (hdcSrc) {
        back->ReleaseDC(hdcSrc);
      }
      if (hdcDst) {
        primary->ReleaseDC(hdcDst);
      }
      hr = primary->Blt(&dst, back, &src, DDBLT_DONOTWAIT, nullptr);
    } else {
      int stretchMode = HALFTONE;
      (void)SetStretchBltMode(hdcDst, stretchMode);
      (void)SetBrushOrgEx(hdcDst, 0, 0, nullptr);
      const int dstW = dst.right - dst.left;
      const int dstH = dst.bottom - dst.top;
      const int srcWInt = src.right - src.left;
      const int srcHInt = src.bottom - src.top;
      BOOL ok = StretchBlt(hdcDst,
                          dst.left,
                          dst.top,
                          dstW,
                          dstH,
                          hdcSrc,
                          src.left,
                          src.top,
                          srcWInt,
                          srcHInt,
                          SRCCOPY);
      back->ReleaseDC(hdcSrc);
      primary->ReleaseDC(hdcDst);
      hr = ok ? DD_OK : E_FAIL;
    }
  }

  SafeRelease(back);

  {
    bool expected = false;
    if (SUCCEEDED(hr) && g_loggedScaleViaFlip.compare_exchange_strong(expected, true)) {
      Tracef("Flip: scaled via %s (method=%ls)",
             (cfg.method == SurfaceScaleMethod::kPoint) ? "DirectDraw::Blt stretch" : "GDI StretchBlt",
             SurfaceScaleMethodToString(cfg.method));
    }
  }

  if (FAILED(hr)) {
    Tracef("Flip: scale blit failed hr=0x%08lX; falling back to original Flip", (unsigned long)hr);
    return g_fpDDS7_Flip(primary, targetOverride, flags);
  }

  // Treat Flip as a present event: we already copied the frame into primary.
  return DD_OK;
}

static HRESULT STDMETHODCALLTYPE Hook_DDS7_Blt(LPDIRECTDRAWSURFACE7 self,
                                              LPRECT dst,
                                              LPDIRECTDRAWSURFACE7 src,
                                              LPRECT srcRect,
                                              DWORD flags,
                                              LPDDBLTFX fx) {
  const uint32_t n = g_bltCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 5) {
    LPDIRECTDRAWSURFACE7 primarySnap = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      primarySnap = g_primary;
    }
    const int isPrimary = (primarySnap && self == primarySnap) ? 1 : 0;
    Tracef("Blt call #%lu self=%p%s src=%p flags=0x%08lX", (unsigned long)n, (void*)self, isPrimary ? " (PRIMARY)" : "", (void*)src, (unsigned long)flags);
    TraceRect("  dst", dst);
    TraceRect("  src", srcRect);
  }
  if (!g_fpDDS7_Blt) {
    return DDERR_GENERIC;
  }

  // Many DirectDraw games (and some wrappers) present via primary->Blt instead of Flip.
  // If this is a present-style blit to the primary surface, apply scaling here.
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (IsScalingEnabled() && src) {
    HWND hwnd = nullptr;
    DWORD coop = 0;
    LPDIRECTDRAWSURFACE7 primarySnap = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      hwnd = g_hwnd;
      coop = g_coopFlags;
      primarySnap = g_primary;
    }

    const bool isPrimary = (primarySnap && self == primarySnap);
    if (isPrimary && hwnd && !IsFullscreenCoopFlags(coop)) {
      // Marker so logs can confirm you're running the updated Blt-scaling build.
      {
        static std::atomic<bool> logged{false};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true)) {
          Tracef("Blt: present-scaling hook active (v2)");
        }
      }

      // Determine source rect size.
      DDSURFACEDESC2 sd{};
      sd.dwSize = sizeof(sd);
      HRESULT hrDesc = src->GetSurfaceDesc(&sd);
      if (SUCCEEDED(hrDesc) && sd.dwWidth && sd.dwHeight) {
        RECT localSrc = srcRect ? *srcRect : MakeRectFromXYWH(0, 0, (int)sd.dwWidth, (int)sd.dwHeight);
        const LONG sW = localSrc.right - localSrc.left;
        const LONG sH = localSrc.bottom - localSrc.top;

        int clientW = 0, clientH = 0;
        if (sW > 0 && sH > 0 && GetClientSize(hwnd, &clientW, &clientH)) {
          const LONG dstW = dst ? (dst->right - dst->left) : 0;
          const LONG dstH = dst ? (dst->bottom - dst->top) : 0;

          // Treat as a present-style call if destination covers either the original render size
          // (common) OR already matches the client size (app/wrapper is already stretching).
          // This avoids missing the common case where the wrapper stretches with point sampling.
            const bool looksLikePresent =
              (!dst) ||
              (dst && ((dstW == sW && dstH == sH) || (dstW == clientW && dstH == clientH)));

          if (looksLikePresent) {
            RECT localDst{};
            if (!GetClientRectInScreen(hwnd, &localDst)) {
              localDst = MakeRectFromXYWH(0, 0, clientW, clientH);
            }

            HRESULT hrScale = DDERR_GENERIC;
            if (cfg.method == SurfaceScaleMethod::kPoint) {
              // Keep original flags if possible, but drop effects.
              const DWORD bltFlags = (flags & (DDBLT_WAIT | DDBLT_DONOTWAIT));
              hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, bltFlags ? bltFlags : DDBLT_DONOTWAIT, nullptr);
              if (hrScale == DDERR_WASSTILLDRAWING) {
                hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, DDBLT_WAIT, nullptr);
              }
            } else {
              // GDI filtered stretch.
              HDC hdcDst = nullptr;
              HDC hdcSrc = nullptr;
              HRESULT hrDst = self->GetDC(&hdcDst);
              HRESULT hrSrc = src->GetDC(&hdcSrc);
              if (SUCCEEDED(hrDst) && SUCCEEDED(hrSrc) && hdcDst && hdcSrc) {
                (void)SetStretchBltMode(hdcDst, HALFTONE);
                (void)SetBrushOrgEx(hdcDst, 0, 0, nullptr);
                BOOL ok = StretchBlt(hdcDst,
                                    localDst.left,
                                    localDst.top,
                                    localDst.right - localDst.left,
                                    localDst.bottom - localDst.top,
                                    hdcSrc,
                                    localSrc.left,
                                    localSrc.top,
                                    localSrc.right - localSrc.left,
                                    localSrc.bottom - localSrc.top,
                                    SRCCOPY);
                src->ReleaseDC(hdcSrc);
                self->ReleaseDC(hdcDst);
                hrScale = ok ? DD_OK : E_FAIL;
              } else {
                if (hdcSrc) {
                  src->ReleaseDC(hdcSrc);
                }
                if (hdcDst) {
                  self->ReleaseDC(hdcDst);
                }
                // Fallback 1: CPU bilinear via Lock (works even when GetDC is unsupported by wrappers).
                hrScale = TryScaleViaLockBilinear(self, localDst, src, localSrc);
                if (SUCCEEDED(hrScale)) {
                  bool expected = false;
                  if (g_loggedLockScale.compare_exchange_strong(expected, true)) {
                    Tracef("Blt: filtered scaling via Lock/CPU active (method=%ls)", SurfaceScaleMethodToString(cfg.method));
                  }
                } else {
                  // Fallback 2: point stretch.
                  hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, DDBLT_DONOTWAIT, nullptr);
                  if (FAILED(hrScale)) {
                    hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, DDBLT_WAIT, nullptr);
                  }
                }
                bool expected = false;
                if (g_loggedFilteredFallback.compare_exchange_strong(expected, true)) {
                  Tracef("Blt: filtered method requested (%ls) but GetDC failed; falling back to point stretch", SurfaceScaleMethodToString(cfg.method));
                  Tracef("Blt: GetDC results hrDst=0x%08lX hrSrc=0x%08lX", (unsigned long)hrDst, (unsigned long)hrSrc);
                }
              }
            }

            if (SUCCEEDED(hrScale)) {
              bool expected = false;
              if (g_loggedScaleViaBlt.compare_exchange_strong(expected, true)) {
                Tracef("Blt: scaled via %s (method=%ls)",
                       (cfg.method == SurfaceScaleMethod::kPoint) ? "DirectDraw::Blt stretch" : "GDI StretchBlt",
                       SurfaceScaleMethodToString(cfg.method));
              }
              return DD_OK;
            }
            // If our scaling failed, fall through to the original call.
          }
          // If scaling is enabled and we didn't treat this as present-style, log once with details.
          if (!looksLikePresent && cfg.method != SurfaceScaleMethod::kPoint) {
            static std::atomic<uint32_t> skips{0};
            const uint32_t c = skips.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c <= 3) {
              Tracef("Blt: filtered scaling skipped (not present-style):");
              TraceRectInline("  dst", dst);
              TraceRectInline("  src", &localSrc);
              Tracef("  srcW=%ld srcH=%ld clientW=%d clientH=%d flags=0x%08lX", (long)sW, (long)sH, clientW, clientH, (unsigned long)flags);
            }
          }
        }
      } else {
        static std::atomic<uint32_t> descFails{0};
        const uint32_t c = descFails.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= 3) {
          Tracef("Blt: src->GetSurfaceDesc failed hr=0x%08lX (cannot decide present-style)", (unsigned long)hrDesc);
          TraceRectInline("  dst", dst);
          TraceRectInline("  src", srcRect);
        }
      }
    }
  }

  return g_fpDDS7_Blt(self, dst, src, srcRect, flags, fx);
}

static HRESULT STDMETHODCALLTYPE Hook_DDS7_BltFast(LPDIRECTDRAWSURFACE7 self,
                                                  DWORD x,
                                                  DWORD y,
                                                  LPDIRECTDRAWSURFACE7 src,
                                                  LPRECT srcRect,
                                                  DWORD trans) {
  const uint32_t n = g_bltFastCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 5) {
    LPDIRECTDRAWSURFACE7 primarySnap = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      primarySnap = g_primary;
    }
    const int isPrimary = (primarySnap && self == primarySnap) ? 1 : 0;
    Tracef("BltFast call #%lu self=%p%s src=%p x=%lu y=%lu trans=0x%08lX",
           (unsigned long)n,
           (void*)self,
           isPrimary ? " (PRIMARY)" : "",
           (void*)src,
           (unsigned long)x,
           (unsigned long)y,
           (unsigned long)trans);
    TraceRect("  src", srcRect);
  }
  if (!g_fpDDS7_BltFast) {
    return DDERR_GENERIC;
  }
  return g_fpDDS7_BltFast(self, x, y, src, srcRect, trans);
}

}

bool InstallDDrawSurfaceDoublingHooks() {
  if (!IsScalingEnabled()) {
    g_active.store(false, std::memory_order_release);
    return true;
  }

  bool expected = false;
  if (!g_active.compare_exchange_strong(expected, true)) {
    return true;
  }

  g_stopInitThread.store(false, std::memory_order_release);
  g_initThread = CreateThread(nullptr, 0, &DDrawInitThreadProc, nullptr, 0, nullptr);
  if (!g_initThread) {
    Tracef("failed to start init thread");
    g_active.store(false, std::memory_order_release);
    return false;
  }
  {
    const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
    Tracef("install requested (waiting for ddraw.dll; scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
  }
  return true;
}

bool AreDDrawSurfaceDoublingHooksActive() {
  return g_active.load(std::memory_order_acquire);
}

void RemoveDDrawSurfaceDoublingHooks() {
  if (!g_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  g_stopInitThread.store(true, std::memory_order_release);
  HANDLE th = g_initThread;
  g_initThread = nullptr;
  if (th) {
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
  }

  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    SafeRelease(g_primary);
    SafeRelease(g_cachedBackbuffer);
    g_cachedBackW = 0;
    g_cachedBackH = 0;
    g_hwnd = nullptr;
    g_coopFlags = 0;
    g_resizedOnce = false;
  }

  void* tgtFlip = g_targetDDS7_Flip.exchange(nullptr);
  if (tgtFlip) {
    (void)MH_DisableHook(tgtFlip);
    (void)MH_RemoveHook(tgtFlip);
  }
  void* tgtBlt = g_targetDDS7_Blt.exchange(nullptr);
  if (tgtBlt) {
    (void)MH_DisableHook(tgtBlt);
    (void)MH_RemoveHook(tgtBlt);
  }
  void* tgtBltFast = g_targetDDS7_BltFast.exchange(nullptr);
  if (tgtBltFast) {
    (void)MH_DisableHook(tgtBltFast);
    (void)MH_RemoveHook(tgtBltFast);
  }
  void* tgt = g_targetDD7_CreateSurface.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetDD7_SetCooperativeLevel.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }

  g_fpDD7_SetCooperativeLevel = nullptr;
  g_fpDD7_CreateSurface = nullptr;
  g_fpDDS7_Flip = nullptr;
    g_fpDDS7_Blt = nullptr;
    g_fpDDS7_BltFast = nullptr;
  g_fpDirectDrawCreate = nullptr;
  g_fpDirectDrawCreateEx = nullptr;

  ReleaseMinHook();
}

}
