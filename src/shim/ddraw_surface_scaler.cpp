#include "shim/ddraw_surface_scaler.h"

#include "shim/surface_scale_config.h"

#include "shim/minhook_runtime.h"

#include <MinHook.h>

#include <windows.h>
#include <unknwn.h>
#include <ddraw.h>

#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

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


static std::mutex g_stateMutex;
static HWND g_hwnd = nullptr;
static DWORD g_coopFlags = 0;
static bool g_resizedOnce = false;

static void Tracef(const char* fmt, ...);

// 0=unknown, 1=system ddraw, 2=wrapper ddraw (dgVoodoo/etc)
static std::atomic<int> g_ddrawModuleKind{0};

static std::wstring ToLowerCopy(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& ch : out) {
    ch = (wchar_t)towlower(ch);
  }
  return out;
}

static bool IsLikelyWrapperDDrawDll() {
  const int cached = g_ddrawModuleKind.load(std::memory_order_acquire);
  if (cached == 1) {
    return false;
  }
  if (cached == 2) {
    return true;
  }

  HMODULE h = GetModuleHandleW(L"ddraw.dll");
  if (!h) {
    return false;
  }

  wchar_t modPathBuf[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(h, modPathBuf, (DWORD)(sizeof(modPathBuf) / sizeof(modPathBuf[0])));
  if (!n || n >= (DWORD)(sizeof(modPathBuf) / sizeof(modPathBuf[0]))) {
    return false;
  }
  std::wstring modPath = ToLowerCopy(std::wstring(modPathBuf));

  wchar_t sysDirBuf[MAX_PATH] = {};
  UINT sn = GetSystemDirectoryW(sysDirBuf, (UINT)(sizeof(sysDirBuf) / sizeof(sysDirBuf[0])));
  std::wstring sysDir = ToLowerCopy(std::wstring(sysDirBuf, sysDirBuf + (sn ? sn : 0)));
  if (!sysDir.empty() && sysDir.back() != L'\\') {
    sysDir.push_back(L'\\');
  }

  bool isSystem = false;
  if (!sysDir.empty()) {
    // System32\ddraw.dll
    const std::wstring sysDdraw = sysDir + L"ddraw.dll";
    if (modPath == sysDdraw) {
      isSystem = true;
    }
  }

  // If it's not exactly the system DLL (common for app-local wrappers), treat as wrapper.
  const int kind = isSystem ? 1 : 2;
  g_ddrawModuleKind.store(kind, std::memory_order_release);

  static std::atomic<bool> logged{false};
  bool expected = false;
  if (logged.compare_exchange_strong(expected, true)) {
    Tracef("ddraw.dll path: %ls (%s)", modPathBuf, isSystem ? "system" : "wrapper");
  }

  return !isSystem;
}

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
    // Some wrappers report 16bpp RGB but leave masks zero. Assume 565.
    if (info.rMask == 0 && info.gMask == 0 && info.bMask == 0) {
      info.rMask = 0xF800;
      info.gMask = 0x07E0;
      info.bMask = 0x001F;
      info.rShift = 11;
      info.gShift = 5;
      info.bShift = 0;
      info.rBits = 5;
      info.gBits = 6;
      info.bBits = 5;
    }
  } else if (bpp == 32) {
    info.bytesPerPixel = 4;
    // Some wrappers report 32bpp RGB but leave masks zero. Assume XRGB8888.
    if (info.rMask == 0 && info.gMask == 0 && info.bMask == 0) {
      info.rMask = 0x00FF0000;
      info.gMask = 0x0000FF00;
      info.bMask = 0x000000FF;
      info.aMask = 0;
      info.rShift = 16;
      info.gShift = 8;
      info.bShift = 0;
      info.rBits = 8;
      info.gBits = 8;
      info.bBits = 8;
      info.aBits = 0;
    }
  } else {
    return false;
  }

  // If we still don't have masks/bits, bail out.
  if (info.rMask == 0 || info.gMask == 0 || info.bMask == 0 || info.rBits == 0 || info.gBits == 0 || info.bBits == 0) {
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

static uint32_t ReadPixel(const uint8_t* base, int pitch, int x, int y, int bpp) {
  const uint8_t* p = base + (ptrdiff_t)y * pitch + (ptrdiff_t)x * bpp;
  if (bpp == 4) {
    return *(const uint32_t*)p;
  }
  return (uint32_t)(*(const uint16_t*)p);
}

// --- D3D9-based filtered scaling (hardware accelerated) ---
//
// DirectDraw surfaces from wrappers (e.g. dgVoodoo) can expose GetDC, but using GDI
// StretchBlt every frame is often very slow. For bilinear/bicubic, we instead:
//   1) Lock() the source surface (read-only)
//   2) Convert to A8R8G8B8 in a CPU buffer
//   3) Upload to a dynamic D3D9 texture
//   4) Render to the game window with:
//        - bilinear: fixed-function sampling with linear filtering
//        - bicubic: two-pass 1D cubic filter via pixel shaders
// If anything fails, callers fall back to point stretching.

struct ID3DBlob : public IUnknown {
  virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
  virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using D3DCompile_t = HRESULT(WINAPI*)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const void* pDefines,
    void* pInclude,
    LPCSTR pEntryPoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

class DDrawD3D9Scaler {
 public:
  bool PresentScaled(LPDIRECTDRAWSURFACE7 srcSurf,
                     const RECT& srcRect,
                     HWND hwnd,
                     UINT dstW,
                     UINT dstH,
                     SurfaceScaleMethod method) {
    if (!srcSurf || !hwnd || dstW == 0 || dstH == 0) {
      return false;
    }
    const bool wantLinear = (method == SurfaceScaleMethod::kBilinear || method == SurfaceScaleMethod::kPixelFast);
    const bool wantCubic = (method == SurfaceScaleMethod::kBicubic || method == SurfaceScaleMethod::kCatmullRom || method == SurfaceScaleMethod::kLanczos || method == SurfaceScaleMethod::kLanczos3);
    if (!wantLinear && !wantCubic) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!EnsureDeviceUnlocked(hwnd, dstW, dstH)) {
      return false;
    }

    // Clamp srcRect to the source surface bounds.
    DDSURFACEDESC2 sd{};
    sd.dwSize = sizeof(sd);
    if (FAILED(srcSurf->GetSurfaceDesc(&sd)) || sd.dwWidth == 0 || sd.dwHeight == 0) {
      return false;
    }

    RECT rc = srcRect;
    rc.left = std::max<LONG>(0, rc.left);
    rc.top = std::max<LONG>(0, rc.top);
    rc.right = std::min<LONG>((LONG)sd.dwWidth, rc.right);
    rc.bottom = std::min<LONG>((LONG)sd.dwHeight, rc.bottom);

    const int srcW = (int)(rc.right - rc.left);
    const int srcH = (int)(rc.bottom - rc.top);
    if (srcW <= 0 || srcH <= 0) {
      return false;
    }

    if (!EnsureSrcTextureUnlocked((UINT)srcW, (UINT)srcH)) {
      return false;
    }
    if (!UploadSurfaceRectToSrcTextureUnlocked(srcSurf, rc, (UINT)srcW, (UINT)srcH)) {
      return false;
    }

    HRESULT hr = D3D_OK;
    if (wantLinear) {
      hr = RenderSinglePassUnlocked(srcTex_, dstW, dstH, /*linear=*/true);
      if (SUCCEEDED(hr)) {
        hr = dev_->Present(nullptr, nullptr, nullptr, nullptr);
      }
      return SUCCEEDED(hr);
    }

    // Bicubic: two-pass separable cubic filter (4 taps per pass).
    if (!EnsureBicubicShadersUnlocked()) {
      return false;
    }
    if (!EnsureIntermediateUnlocked(dstW, (UINT)srcH)) {
      return false;
    }

    // Pass 1: horizontal cubic scaling to (dstW x srcH) render target.
    IDirect3DSurface9* interRt = nullptr;
    hr = interTex_->GetSurfaceLevel(0, &interRt);
    if (FAILED(hr) || !interRt) {
      SafeRelease(interRt);
      return false;
    }

    IDirect3DSurface9* prevRt = nullptr;
    hr = dev_->GetRenderTarget(0, &prevRt);
    if (FAILED(hr) || !prevRt) {
      SafeRelease(interRt);
      SafeRelease(prevRt);
      return false;
    }

    hr = dev_->SetRenderTarget(0, interRt);
    SafeRelease(interRt);
    if (FAILED(hr)) {
      SafeRelease(prevRt);
      return false;
    }

    if (FAILED(RenderCubicPassUnlocked(srcTex_, psCubicH_, dstW, (UINT)srcH, (UINT)srcW, (UINT)srcH))) {
      (void)dev_->SetRenderTarget(0, prevRt);
      SafeRelease(prevRt);
      return false;
    }

    // Pass 2: vertical cubic scaling to backbuffer (dstW x dstH).
    hr = dev_->SetRenderTarget(0, prevRt);
    SafeRelease(prevRt);
    if (FAILED(hr)) {
      return false;
    }
    if (FAILED(RenderCubicPassUnlocked(interTex_, psCubicV_, dstW, dstH, dstW, (UINT)srcH))) {
      return false;
    }

    hr = dev_->Present(nullptr, nullptr, nullptr, nullptr);
    return SUCCEEDED(hr);
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    ShutdownUnlocked();
  }

 private:
  static constexpr DWORD kQuadFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

  struct QuadVtx {
    float x, y, z, rhw;
    float u, v;
  };

  void ShutdownUnlocked() {
    SafeRelease(psCubicH_);
    SafeRelease(psCubicV_);
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    SafeRelease(dev_);
    SafeRelease(d3d_);
    hwnd_ = nullptr;
    bbW_ = 0;
    bbH_ = 0;
    staging_.clear();

    fpCreate9_ = nullptr;
    if (d3d9Mod_) {
      FreeLibrary(d3d9Mod_);
      d3d9Mod_ = nullptr;
    }

    fpCompile_ = nullptr;
    if (compilerMod_) {
      FreeLibrary(compilerMod_);
      compilerMod_ = nullptr;
    }
    shadersTried_ = false;
  }

  bool EnsureD3D9LoadedUnlocked() {
    if (fpCreate9_) {
      return true;
    }
    if (!d3d9Mod_) {
      d3d9Mod_ = LoadLibraryW(L"d3d9.dll");
      if (!d3d9Mod_) {
        return false;
      }
    }
    fpCreate9_ = (Direct3DCreate9_t)GetProcAddress(d3d9Mod_, "Direct3DCreate9");
    return fpCreate9_ != nullptr;
  }

  bool EnsureDeviceUnlocked(HWND hwnd, UINT bbW, UINT bbH) {
    if (!EnsureD3D9LoadedUnlocked()) {
      return false;
    }

    const bool needNew = (!dev_ || !d3d_ || hwnd_ != hwnd || bbW_ != bbW || bbH_ != bbH);
    if (!needNew) {
      return true;
    }

    // Recreate everything (simple and robust; resize is infrequent).
    SafeRelease(psCubicH_);
    SafeRelease(psCubicV_);
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    SafeRelease(dev_);
    SafeRelease(d3d_);

    d3d_ = fpCreate9_(D3D_SDK_VERSION);
    if (!d3d_) {
      return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = bbW;
    pp.BackBufferHeight = bbH;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    DWORD createFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT,
                                    D3DDEVTYPE_HAL,
                                    hwnd,
                                    createFlags,
                                    &pp,
                                    &dev_);
    if (FAILED(hr)) {
      createFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_SOFTWARE_VERTEXPROCESSING;
      hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT,
                              D3DDEVTYPE_HAL,
                              hwnd,
                              createFlags,
                              &pp,
                              &dev_);
    }
    if (FAILED(hr) || !dev_) {
      SafeRelease(dev_);
      SafeRelease(d3d_);
      return false;
    }

    hwnd_ = hwnd;
    bbW_ = bbW;
    bbH_ = bbH;

    // Fixed pipeline state (we re-assert key bits per draw).
    return true;
  }

  bool EnsureSrcTextureUnlocked(UINT w, UINT h) {
    if (srcTex_ && srcW_ == w && srcH_ == h) {
      return true;
    }
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    HRESULT hr = dev_->CreateTexture(w,
                                    h,
                                    1,
                                    D3DUSAGE_DYNAMIC,
                                    D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT,
                                    &srcTex_,
                                    nullptr);
    if (FAILED(hr) || !srcTex_) {
      SafeRelease(srcTex_);
      return false;
    }
    srcW_ = w;
    srcH_ = h;
    return true;
  }

  bool EnsureIntermediateUnlocked(UINT w, UINT h) {
    if (interTex_ && interW_ == w && interH_ == h) {
      return true;
    }
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    HRESULT hr = dev_->CreateTexture(w,
                                    h,
                                    1,
                                    D3DUSAGE_RENDERTARGET,
                                    D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT,
                                    &interTex_,
                                    nullptr);
    if (FAILED(hr) || !interTex_) {
      SafeRelease(interTex_);
      return false;
    }
    interW_ = w;
    interH_ = h;
    return true;
  }

  bool EnsureCompilerUnlocked() {
    if (fpCompile_) {
      return true;
    }
    static const wchar_t* kDlls[] = {
        L"d3dcompiler_47.dll",
        L"d3dcompiler_46.dll",
        L"d3dcompiler_45.dll",
        L"d3dcompiler_44.dll",
        L"d3dcompiler_43.dll",
        L"d3dcompiler_42.dll",
        L"d3dcompiler_41.dll",
    };
    for (const wchar_t* name : kDlls) {
      HMODULE m = LoadLibraryW(name);
      if (!m) {
        continue;
      }
      auto fn = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
      if (fn) {
        compilerMod_ = m;
        fpCompile_ = fn;
        return true;
      }
      FreeLibrary(m);
    }
    return false;
  }

  bool EnsureBicubicShadersUnlocked() {
    if (psCubicH_ && psCubicV_) {
      return true;
    }
    if (shadersTried_) {
      return false;
    }
    shadersTried_ = true;

    if (!EnsureCompilerUnlocked()) {
      return false;
    }

    // Catmull-Rom cubic (A=-0.5) with low instruction count (fits ps_2_0).
    static const char* kCubicHlslH =
      "float4 p : register(c0);\n" // x=srcW, y=srcH, z=invW, w=invH
      "sampler2D s0 : register(s0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
      "  float x = uv.x * p.x - 0.5;\n"
      "  float ix = floor(x);\n"
      "  float t = x - ix;\n"
      "  float t2 = t * t;\n"
      "  float t3 = t2 * t;\n"
      "  float w0 = -0.5*t + 1.0*t2 - 0.5*t3;\n"
      "  float w1 = 1.0 - 2.5*t2 + 1.5*t3;\n"
      "  float w2 = 0.5*t + 2.0*t2 - 1.5*t3;\n"
      "  float w3 = -0.5*t2 + 0.5*t3;\n"
      "  float u0 = (ix - 1.0 + 0.5) * p.z;\n"
      "  float u1 = (ix + 0.0 + 0.5) * p.z;\n"
      "  float u2 = (ix + 1.0 + 0.5) * p.z;\n"
      "  float u3 = (ix + 2.0 + 0.5) * p.z;\n"
      "  float4 c = tex2D(s0, float2(u0, uv.y)) * w0 +\n"
      "            tex2D(s0, float2(u1, uv.y)) * w1 +\n"
      "            tex2D(s0, float2(u2, uv.y)) * w2 +\n"
      "            tex2D(s0, float2(u3, uv.y)) * w3;\n"
      "  return c;\n"
      "}\n";

    static const char* kCubicHlslV =
      "float4 p : register(c0);\n" // x=srcW, y=srcH, z=invW, w=invH
      "sampler2D s0 : register(s0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
      "  float y = uv.y * p.y - 0.5;\n"
      "  float iy = floor(y);\n"
      "  float t = y - iy;\n"
      "  float t2 = t * t;\n"
      "  float t3 = t2 * t;\n"
      "  float w0 = -0.5*t + 1.0*t2 - 0.5*t3;\n"
      "  float w1 = 1.0 - 2.5*t2 + 1.5*t3;\n"
      "  float w2 = 0.5*t + 2.0*t2 - 1.5*t3;\n"
      "  float w3 = -0.5*t2 + 0.5*t3;\n"
      "  float v0 = (iy - 1.0 + 0.5) * p.w;\n"
      "  float v1 = (iy + 0.0 + 0.5) * p.w;\n"
      "  float v2 = (iy + 1.0 + 0.5) * p.w;\n"
      "  float v3 = (iy + 2.0 + 0.5) * p.w;\n"
      "  float4 c = tex2D(s0, float2(uv.x, v0)) * w0 +\n"
      "            tex2D(s0, float2(uv.x, v1)) * w1 +\n"
      "            tex2D(s0, float2(uv.x, v2)) * w2 +\n"
      "            tex2D(s0, float2(uv.x, v3)) * w3;\n"
      "  return c;\n"
      "}\n";

    ID3DBlob* codeH = nullptr;
    ID3DBlob* errH = nullptr;
    HRESULT hr = fpCompile_(kCubicHlslH, (SIZE_T)strlen(kCubicHlslH), "hklmwrap_ddraw_cubic_h", nullptr, nullptr,
                            "main", "ps_2_0", 0, 0, &codeH, &errH);
    if (FAILED(hr) || !codeH) {
      if (errH) {
        Tracef("bicubic shader compile (H) failed hr=0x%08lX: %s", (unsigned long)hr, (const char*)errH->GetBufferPointer());
      } else {
        Tracef("bicubic shader compile (H) failed hr=0x%08lX", (unsigned long)hr);
      }
      SafeRelease(errH);
      SafeRelease(codeH);
      return false;
    }
    SafeRelease(errH);

    ID3DBlob* codeV = nullptr;
    ID3DBlob* errV = nullptr;
    hr = fpCompile_(kCubicHlslV, (SIZE_T)strlen(kCubicHlslV), "hklmwrap_ddraw_cubic_v", nullptr, nullptr,
                    "main", "ps_2_0", 0, 0, &codeV, &errV);
    if (FAILED(hr) || !codeV) {
      if (errV) {
        Tracef("bicubic shader compile (V) failed hr=0x%08lX: %s", (unsigned long)hr, (const char*)errV->GetBufferPointer());
      } else {
        Tracef("bicubic shader compile (V) failed hr=0x%08lX", (unsigned long)hr);
      }
      SafeRelease(errV);
      SafeRelease(codeV);
      SafeRelease(codeH);
      return false;
    }
    SafeRelease(errV);

    hr = dev_->CreatePixelShader((const DWORD*)codeH->GetBufferPointer(), &psCubicH_);
    SafeRelease(codeH);
    if (FAILED(hr) || !psCubicH_) {
      SafeRelease(codeV);
      SafeRelease(psCubicH_);
      return false;
    }
    hr = dev_->CreatePixelShader((const DWORD*)codeV->GetBufferPointer(), &psCubicV_);
    SafeRelease(codeV);
    if (FAILED(hr) || !psCubicV_) {
      SafeRelease(psCubicH_);
      SafeRelease(psCubicV_);
      return false;
    }
    return true;
  }

  bool UploadSurfaceRectToSrcTextureUnlocked(LPDIRECTDRAWSURFACE7 srcSurf, const RECT& rc, UINT w, UINT h) {
    PixelFormatInfo srcFmt;
    if (!GetPixelFormatInfoFromSurface(srcSurf, &srcFmt)) {
      return false;
    }

    DDSURFACEDESC2 ssd{};
    ssd.dwSize = sizeof(ssd);
    // Avoid stalling on wrappers that keep surfaces on the GPU (common with dgVoodoo).
    // If we can't lock immediately, let caller fall back to point stretch.
    HRESULT hr = srcSurf->Lock(nullptr, &ssd, DDLOCK_DONOTWAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr) || !ssd.lpSurface || ssd.lPitch <= 0) {
      if (SUCCEEDED(hr)) {
        srcSurf->Unlock(nullptr);
      }
      return false;
    }

    const uint8_t* sBase = (const uint8_t*)ssd.lpSurface;
    const int sPitch = (int)ssd.lPitch;
    const int bpp = srcFmt.bytesPerPixel;
    if (bpp != 2 && bpp != 4) {
      srcSurf->Unlock(nullptr);
      return false;
    }

    const size_t needed = (size_t)w * (size_t)h;
    if (staging_.size() < needed) {
      staging_.resize(needed);
    }

    // Fast paths for common formats.
    const bool isXrgb8888 =
        (bpp == 4) &&
        (srcFmt.rMask == 0x00FF0000) &&
        (srcFmt.gMask == 0x0000FF00) &&
        (srcFmt.bMask == 0x000000FF) &&
        (srcFmt.aMask == 0);
    const bool isArgb8888 =
        (bpp == 4) &&
        (srcFmt.rMask == 0x00FF0000) &&
        (srcFmt.gMask == 0x0000FF00) &&
        (srcFmt.bMask == 0x000000FF) &&
        (srcFmt.aMask == 0xFF000000);
    const bool isRgb565 =
        (bpp == 2) &&
        (srcFmt.rMask == 0xF800) &&
        (srcFmt.gMask == 0x07E0) &&
        (srcFmt.bMask == 0x001F) &&
        (srcFmt.aMask == 0);

    if (isArgb8888 || isXrgb8888) {
      for (UINT y = 0; y < h; y++) {
        const uint32_t* row = (const uint32_t*)(sBase + (ptrdiff_t)(rc.top + (LONG)y) * sPitch) + rc.left;
        uint32_t* out = &staging_[y * w];
        if (isArgb8888) {
          memcpy(out, row, (size_t)w * sizeof(uint32_t));
        } else {
          for (UINT x = 0; x < w; x++) {
            out[x] = row[x] | 0xFF000000u;
          }
        }
      }
    } else if (isRgb565) {
      for (UINT y = 0; y < h; y++) {
        const uint16_t* row = (const uint16_t*)(sBase + (ptrdiff_t)(rc.top + (LONG)y) * sPitch) + rc.left;
        uint32_t* out = &staging_[y * w];
        for (UINT x = 0; x < w; x++) {
          const uint16_t p16 = row[x];
          const uint32_t r5 = (uint32_t)((p16 >> 11) & 0x1F);
          const uint32_t g6 = (uint32_t)((p16 >> 5) & 0x3F);
          const uint32_t b5 = (uint32_t)(p16 & 0x1F);
          const uint32_t r8 = (r5 << 3) | (r5 >> 2);
          const uint32_t g8 = (g6 << 2) | (g6 >> 4);
          const uint32_t b8 = (b5 << 3) | (b5 >> 2);
          out[x] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
        }
      }
    } else {
      for (UINT y = 0; y < h; y++) {
        for (UINT x = 0; x < w; x++) {
          const int sx = (int)rc.left + (int)x;
          const int sy = (int)rc.top + (int)y;
          const uint32_t p = ReadPixel(sBase, sPitch, sx, sy, bpp);
          uint8_t r, g, b, a;
          UnpackRGBA(srcFmt, p, &r, &g, &b, &a);
          staging_[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
      }
    }

    srcSurf->Unlock(nullptr);

    D3DLOCKED_RECT lr{};
    hr = srcTex_->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr) || !lr.pBits || lr.Pitch <= 0) {
      if (SUCCEEDED(hr)) {
        srcTex_->UnlockRect(0);
      }
      return false;
    }
    const int dstPitch = (int)lr.Pitch;
    uint8_t* dst = (uint8_t*)lr.pBits;
    for (UINT y = 0; y < h; y++) {
      memcpy(dst + (size_t)y * (size_t)dstPitch, &staging_[y * w], (size_t)w * sizeof(uint32_t));
    }
    srcTex_->UnlockRect(0);
    return true;
  }

  void SetCommonDrawStateUnlocked(bool linear) {
    (void)dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
    (void)dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    (void)dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
    (void)dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    (void)dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    (void)dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    (void)dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    (void)dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    (void)dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    (void)dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    (void)dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    (void)dev_->SetSamplerState(0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    (void)dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    (void)dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  }

  HRESULT RenderSinglePassUnlocked(IDirect3DTexture9* tex, UINT w, UINT h, bool linear) {
    if (!tex) {
      return E_INVALIDARG;
    }
    D3DVIEWPORT9 vp{};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = w;
    vp.Height = h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    (void)dev_->SetViewport(&vp);

    SetCommonDrawStateUnlocked(linear);

    (void)dev_->SetPixelShader(nullptr);
    (void)dev_->SetTexture(0, tex);
    (void)dev_->SetFVF(kQuadFVF);

    const float fw = (float)w;
    const float fh = (float)h;
    QuadVtx v[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {fw - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, fh - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    HRESULT hr = dev_->BeginScene();
    if (FAILED(hr)) {
      return hr;
    }
    hr = dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVtx));
    (void)dev_->EndScene();
    return hr;
  }

  HRESULT RenderCubicPassUnlocked(IDirect3DTexture9* tex,
                                 IDirect3DPixelShader9* ps,
                                 UINT outW,
                                 UINT outH,
                                 UINT inW,
                                 UINT inH) {
    if (!tex || !ps || outW == 0 || outH == 0 || inW == 0 || inH == 0) {
      return E_INVALIDARG;
    }
    D3DVIEWPORT9 vp{};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = outW;
    vp.Height = outH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    (void)dev_->SetViewport(&vp);

    // Point sampling; shader computes weights.
    SetCommonDrawStateUnlocked(/*linear=*/false);

    (void)dev_->SetTexture(0, tex);
    (void)dev_->SetPixelShader(ps);
    (void)dev_->SetFVF(kQuadFVF);

    const float params[4] = {
        (float)inW,
        (float)inH,
        1.0f / (float)inW,
        1.0f / (float)inH,
    };
    (void)dev_->SetPixelShaderConstantF(0, params, 1);

    const float fw = (float)outW;
    const float fh = (float)outH;
    QuadVtx v[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {fw - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, fh - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    HRESULT hr = dev_->BeginScene();
    if (FAILED(hr)) {
      return hr;
    }
    hr = dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVtx));
    (void)dev_->EndScene();
    return hr;
  }

  std::mutex mu_;
  HMODULE d3d9Mod_ = nullptr;
  Direct3DCreate9_t fpCreate9_ = nullptr;
  IDirect3D9* d3d_ = nullptr;
  IDirect3DDevice9* dev_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT bbW_ = 0;
  UINT bbH_ = 0;

  IDirect3DTexture9* srcTex_ = nullptr;
  UINT srcW_ = 0;
  UINT srcH_ = 0;

  IDirect3DTexture9* interTex_ = nullptr;
  UINT interW_ = 0;
  UINT interH_ = 0;

  HMODULE compilerMod_ = nullptr;
  D3DCompile_t fpCompile_ = nullptr;
  bool shadersTried_ = false;

  IDirect3DPixelShader9* psCubicH_ = nullptr;
  IDirect3DPixelShader9* psCubicV_ = nullptr;

  std::vector<uint32_t> staging_;
};

static DDrawD3D9Scaler g_d3d9Scaler;

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

static bool InstallDDrawSurfaceScalerHooksOnce() {
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
  Tracef("DirectDraw path: filtered scaling uses D3D9 (GPU); fallback on failure is point stretch");

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
    const bool ok = InstallDDrawSurfaceScalerHooksOnce();
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

  // DirectDraw wrappers (dgVoodoo/etc) commonly keep surfaces on the GPU and can
  // make Lock/CPU readback paths unreliable/slow. The shim no longer tries to
  // "fix" wrapper present paths via DXGI post-filter hooks; use a dgVoodoo AddOn
  // for scaling/filtering instead.
  const bool isWrapper = IsLikelyWrapperDDrawDll();
  if (isWrapper) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      Tracef("DirectDraw wrapper detected; shim surface scaling disabled for this path (use dgVoodoo AddOn)");
    }
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
  const bool usePointPath = (cfg.method == SurfaceScaleMethod::kPoint);
  if (usePointPath) {
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
    // Hardware accelerated path (D3D9). If it fails, fall back to point stretch.
    const bool okGpu = g_d3d9Scaler.PresentScaled(back, src, hwnd, (UINT)clientW, (UINT)clientH, cfg.method);
    if (okGpu) {
      hr = DD_OK;
    } else {
      bool expected = false;
      if (g_loggedFilteredFallback.compare_exchange_strong(expected, true)) {
        Tracef("Flip: GPU filtered scaling failed (method=%ls); falling back to point stretch", SurfaceScaleMethodToString(cfg.method));
      }
      if (g_fpDDS7_Blt) {
        hr = g_fpDDS7_Blt(primary, &dst, back, &src, DDBLT_DONOTWAIT, nullptr);
        if (hr == DDERR_WASSTILLDRAWING) {
          hr = g_fpDDS7_Blt(primary, &dst, back, &src, DDBLT_WAIT, nullptr);
        }
      } else {
        hr = primary->Blt(&dst, back, &src, DDBLT_DONOTWAIT, nullptr);
        if (hr == DDERR_WASSTILLDRAWING) {
          hr = primary->Blt(&dst, back, &src, DDBLT_WAIT, nullptr);
        }
      }
    }
  }

  SafeRelease(back);

  {
    bool expected = false;
    if (SUCCEEDED(hr) && g_loggedScaleViaFlip.compare_exchange_strong(expected, true)) {
      Tracef("Flip: scaled via %s (method=%ls)",
             usePointPath ? "DirectDraw::Blt stretch" : "D3D9 GPU present",
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

      // Do not attempt DirectDraw present-time scaling on wrapper ddraw.dlls
      // (dgVoodoo/etc). This used to rely on a separate DXGI post-filter hook,
      // but that path is intentionally removed.
      if (IsLikelyWrapperDDrawDll()) {
        static std::atomic<bool> logged{false};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true)) {
          Tracef("Blt: DirectDraw wrapper detected; shim surface scaling disabled for this path (use dgVoodoo AddOn)");
        }
        return g_fpDDS7_Blt(self, dst, src, srcRect, flags, fx);
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
              // Hardware accelerated path (D3D9). If it fails, fall back to point stretch.
              const bool okGpu = g_d3d9Scaler.PresentScaled(src, localSrc, hwnd, (UINT)clientW, (UINT)clientH, cfg.method);
              if (okGpu) {
                hrScale = DD_OK;
              } else {
                hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, DDBLT_DONOTWAIT, nullptr);
                if (FAILED(hrScale)) {
                  hrScale = g_fpDDS7_Blt(self, &localDst, src, &localSrc, DDBLT_WAIT, nullptr);
                }
                bool expected = false;
                if (g_loggedFilteredFallback.compare_exchange_strong(expected, true)) {
                  Tracef("Blt: GPU filtered scaling failed (method=%ls); falling back to point stretch", SurfaceScaleMethodToString(cfg.method));
                }
              }
            }

            if (SUCCEEDED(hrScale)) {
              bool expected = false;
              if (g_loggedScaleViaBlt.compare_exchange_strong(expected, true)) {
                Tracef("Blt: scaled via %s (method=%ls)",
                       (cfg.method == SurfaceScaleMethod::kPoint) ? "DirectDraw::Blt stretch" : "D3D9 GPU present",
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

bool InstallDDrawSurfaceScalerHooks() {
  if (!IsScalingEnabled()) {
    g_active.store(false, std::memory_order_release);
    return true;
  }

  // If ddraw.dll is already loaded and it's not the system DLL, assume a wrapper
  // (dgVoodoo/etc) and do not install DirectDraw scaling hooks.
  if (GetModuleHandleW(L"ddraw.dll") != nullptr && IsLikelyWrapperDDrawDll()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      Tracef("ddraw.dll wrapper detected at install time; shim DirectDraw scaling hooks disabled (use dgVoodoo AddOn)");
    }
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

bool AreDDrawSurfaceScalerHooksActive() {
  return g_active.load(std::memory_order_acquire);
}

void RemoveDDrawSurfaceScalerHooks() {
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

  // Release any D3D9 resources used for filtered scaling.
  g_d3d9Scaler.Shutdown();

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
