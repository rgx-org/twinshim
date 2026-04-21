  void MaybeResizeWindowOnce(double factor) {
    if (factor <= 1.0) {
      return;
    }
    bool expected = false;
    if (!didResize_.compare_exchange_strong(expected, true)) {
      return;
    }

    HWND hwnd = FindBestTopLevelWindowForCurrentProcess();
    if (!hwnd) {
      Tracef("window resize skipped: no suitable top-level window found");
      return;
    }

    resizedHwnd_ = hwnd;
    {
      wchar_t cls[128] = {};
      wchar_t title[256] = {};
      (void)GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
      (void)GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));
      char cls8[256] = {};
      char title8[512] = {};
      WideToUtf8BestEffort(std::wstring(cls), cls8, sizeof(cls8));
      WideToUtf8BestEffort(std::wstring(title), title8, sizeof(title8));
      Tracef("resize target hwnd=%p class='%s' title='%s'", (void*)hwnd, cls8, title8);
    }
    int cw = 0, ch = 0;
    if (!GetClientSize(hwnd, &cw, &ch)) {
      Tracef("window resize skipped: could not query client size");
      return;
    }
    const UINT dstW = CalcScaledUInt((UINT)cw, factor);
    const UINT dstH = CalcScaledUInt((UINT)ch, factor);

    desiredClientW_ = (int)dstW;
    desiredClientH_ = (int)dstH;
    resizeRetryCount_ = 0;
    flushCountdown_ = 120;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    Tracef("resize styles: style=0x%08lX ex=0x%08lX", (unsigned long)style, (unsigned long)exStyle);

    // Install window-size guard BEFORE the first resize.  Some DX9 games (and
    // dgVoodoo itself) actively enforce the window size to match the back-buffer
    // and will immediately undo our resize.  The guard subclasses the window
    // procedure and clamps WM_WINDOWPOSCHANGING so the window cannot shrink
    // below the desired scaled client size.
    const bool guardOk = InstallWindowSizeGuard(hwnd, (int)dstW, (int)dstH);
    Tracef("window size guard: %s (hwnd=%p min=%ux%u)", guardOk ? "installed" : "failed", (void*)hwnd, (unsigned)dstW, (unsigned)dstH);

    const bool ok = ResizeWindowClient(hwnd, (int)dstW, (int)dstH);
    const DWORD gle = GetLastError();
    int cw2 = 0, ch2 = 0;
    const bool ok2 = GetClientSize(hwnd, &cw2, &ch2);
    Tracef(
      "resize window client %dx%d -> %ux%u (scale=%.3f %s; after=%s %dx%d)",
      cw,
      ch,
      dstW,
      dstH,
      factor,
        ok ? "ok" : "failed",
      ok2 ? "ok" : "failed",
      cw2,
      ch2);
    if (!ok) {
      Tracef("resize initial failed gle=%lu", (unsigned long)gle);
    }

    // Publish explicit mapping to the injected shim (if present) so mouse coordinate
    // remapping does not need to infer src/dst sizes.
    {
      // The shim exports use __cdecl (not WINAPI/stdcall) to avoid name decoration.
      using RegisterFn = void(*)(HWND, int, int, int, int, double);
      HMODULE hShim = GetModuleHandleW(L"twinshim_shim.dll");
      if (!hShim) {
        hShim = GetModuleHandleW(L"twinshim_shim");
      }
      if (hShim) {
        auto fn = reinterpret_cast<RegisterFn>(GetProcAddress(hShim, "TwinShim_RegisterScaledWindow"));
        if (fn) {
          fn(hwnd, cw, ch, (int)dstW, (int)dstH, factor);
          Tracef("published scale mapping to shim hwnd=%p src=%dx%d dst=%ux%u", (void*)hwnd, cw, ch, (unsigned)dstW, (unsigned)dstH);
        } else {
          Tracef("shim present but TwinShim_RegisterScaledWindow export missing");
        }
      } else {
        Tracef("shim not loaded; scale mapping not published");
      }
    }
  }

  void ReleaseSwapchainOutputUnlocked(SwapchainState& sc) {
    if (!sc.outputTex) {
      return;
    }
    if (root_) {
      // Notify dgVoodoo tracking system because we used ResourceBarrier on this resource.
      (void)root_->RTResourceDestroyed(sc.outputTex, true);
    }
    sc.outputTex->Release();
    sc.outputTex = nullptr;
    sc.outputTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    sc.outputSrvCpu.ptr = 0;
    sc.outputRtvCpu.ptr = 0;

    // Free descriptor allocations.
    auto itAd = adapters_.find(sc.adapterID);
    if (itAd != adapters_.end() && itAd->second.srvAlloc && itAd->second.rtvAlloc) {
      if (sc.outputSrvHandle != (UInt32)-1) {
        itAd->second.srvAlloc->DeallocDescriptorGroup(sc.outputSrvHandle, 1, nullptr, 0);
      }
      if (sc.outputRtvHandle != (UInt32)-1) {
        itAd->second.rtvAlloc->DeallocDescriptorGroup(sc.outputRtvHandle, 1, nullptr, 0);
      }
    }
    sc.outputSrvHandle = (UInt32)-1;
    sc.outputRtvHandle = (UInt32)-1;
  }

  void ReleaseSwapchainNativeUnlocked(SwapchainState& sc) {
    if (!sc.nativeTex) {
      return;
    }
    if (root_) {
      (void)root_->RTResourceDestroyed(sc.nativeTex, true);
    }
    sc.nativeTex->Release();
    sc.nativeTex = nullptr;
    sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sc.nativeSrvCpu.ptr = 0;
    sc.nativeRtvCpu.ptr = 0;

    auto itAd = adapters_.find(sc.adapterID);
    if (itAd != adapters_.end() && itAd->second.srvAlloc && itAd->second.rtvAlloc) {
      if (sc.nativeSrvHandle != (UInt32)-1) {
        itAd->second.srvAlloc->DeallocDescriptorGroup(sc.nativeSrvHandle, 1, nullptr, 0);
      }
      if (sc.nativeRtvHandle != (UInt32)-1) {
        itAd->second.rtvAlloc->DeallocDescriptorGroup(sc.nativeRtvHandle, 1, nullptr, 0);
      }
    }
    sc.nativeSrvHandle = (UInt32)-1;
    sc.nativeRtvHandle = (UInt32)-1;
  }

  bool EnsureNativeResourcesUnlocked(AdapterState& ad, SwapchainState& sc) {
    if (sc.nativeW == 0 || sc.nativeH == 0 || sc.fmt == DXGI_FORMAT_UNKNOWN) {
      return false;
    }
    if (sc.nativeTex && sc.nativeSrvCpu.ptr && sc.nativeRtvCpu.ptr) {
      return true;
    }

    ReleaseSwapchainNativeUnlocked(sc);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = sc.nativeW;
    rd.Height = sc.nativeH;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = sc.fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = sc.fmt;
    cv.Color[0] = 0.0f;
    cv.Color[1] = 0.0f;
    cv.Color[2] = 0.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* tex = nullptr;
    HRESULT hr = ad.dev->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &cv,
        __uuidof(ID3D12Resource),
        (void**)&tex);
    if (FAILED(hr) || !tex) {
      Tracef("CreateCommittedResource(nativeTex) failed hr=0x%08lX", (unsigned long)hr);
      return false;
    }
    sc.nativeTex = tex;
    sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    const UInt32 srvHandle = ad.srvAlloc->AllocDescriptorGroup(1);
    const UInt32 rtvHandle = ad.rtvAlloc->AllocDescriptorGroup(1);
    if (srvHandle == (UInt32)-1 || rtvHandle == (UInt32)-1) {
      Tracef("descriptor allocation failed (nativeTex)");
      ReleaseSwapchainNativeUnlocked(sc);
      return false;
    }
    sc.nativeSrvHandle = srvHandle;
    sc.nativeRtvHandle = rtvHandle;
    sc.nativeSrvCpu = ad.srvAlloc->GetCPUDescHandle(srvHandle, 0);
    sc.nativeRtvCpu = ad.rtvAlloc->GetCPUDescHandle(rtvHandle, 0);

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = sc.fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ad.dev->CreateShaderResourceView(sc.nativeTex, &sd, sc.nativeSrvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rdv{};
    rdv.Format = sc.fmt;
    rdv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rdv.Texture2D.MipSlice = 0;
    rdv.Texture2D.PlaneSlice = 0;
    ad.dev->CreateRenderTargetView(sc.nativeTex, &rdv, sc.nativeRtvCpu);

    return true;
  }

  bool EnsureOutputResourcesUnlocked(AdapterState& ad, SwapchainState& sc) {
    if (sc.w == 0 || sc.h == 0 || sc.fmt == DXGI_FORMAT_UNKNOWN) {
      return false;
    }
    if (sc.outputTex && sc.outputSrvCpu.ptr && sc.outputRtvCpu.ptr) {
      return true;
    }

    ReleaseSwapchainOutputUnlocked(sc);

    // Create output texture.
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment = 0;
    rd.Width = sc.w;
    rd.Height = sc.h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = sc.fmt;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = sc.fmt;
    cv.Color[0] = 0.0f;
    cv.Color[1] = 0.0f;
    cv.Color[2] = 0.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* tex = nullptr;
    HRESULT hr = ad.dev->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &cv,
        __uuidof(ID3D12Resource),
        (void**)&tex);
    if (FAILED(hr) || !tex) {
      Tracef("CreateCommittedResource failed hr=0x%08lX", (unsigned long)hr);
      return false;
    }
    sc.outputTex = tex;
    sc.outputTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // Allocate descriptors from dgVoodoo allocators (CPU-only heaps).
    const UInt32 srvHandle = ad.srvAlloc->AllocDescriptorGroup(1);
    const UInt32 rtvHandle = ad.rtvAlloc->AllocDescriptorGroup(1);
    if (srvHandle == (UInt32)-1 || rtvHandle == (UInt32)-1) {
      Tracef("descriptor allocation failed");
      ReleaseSwapchainOutputUnlocked(sc);
      return false;
    }
    sc.outputSrvHandle = srvHandle;
    sc.outputRtvHandle = rtvHandle;
    sc.outputSrvCpu = ad.srvAlloc->GetCPUDescHandle(srvHandle, 0);
    sc.outputRtvCpu = ad.rtvAlloc->GetCPUDescHandle(rtvHandle, 0);

    // Create SRV/RTV.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = sc.fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ad.dev->CreateShaderResourceView(sc.outputTex, &sd, sc.outputSrvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rdv{};
    rdv.Format = sc.fmt;
    rdv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rdv.Texture2D.MipSlice = 0;
    rdv.Texture2D.PlaneSlice = 0;
    ad.dev->CreateRenderTargetView(sc.outputTex, &rdv, sc.outputRtvCpu);

    return true;
  }

  static bool EnsureD3DCompilerLoaded(D3DCompile_t* outFn) {
    if (!outFn) {
      return false;
    }
    *outFn = nullptr;
    static std::atomic<HMODULE> g_mod{nullptr};
    static std::atomic<D3DCompile_t> g_fn{nullptr};
    if (auto fn = g_fn.load(std::memory_order_acquire)) {
      *outFn = fn;
      return true;
    }
    static const wchar_t* kDlls[] = {
        L"d3dcompiler_47.dll",
        L"d3dcompiler_46.dll",
        L"d3dcompiler_45.dll",
    };
    for (const wchar_t* name : kDlls) {
      HMODULE m = LoadLibraryW(name);
      if (!m) {
        continue;
      }
      auto fn = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
      if (fn) {
        g_mod.store(m, std::memory_order_release);
        g_fn.store(fn, std::memory_order_release);
        *outFn = fn;
        return true;
      }
      FreeLibrary(m);
    }
    return false;
  }

  static bool CompileHlsl(const char* src, const char* entry, const char* target, ID3DBlob** outBlob) {
    if (!src || !entry || !target || !outBlob) {
      return false;
    }
    *outBlob = nullptr;
    D3DCompile_t fp = nullptr;
    if (!EnsureD3DCompilerLoaded(&fp) || !fp) {
      return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const UINT flags1 = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = fp(src, std::strlen(src), "hklm_dgvoodoo_addon", nullptr, nullptr, entry, target, flags1, 0, &code, &err);
    if (FAILED(hr) || !code) {
      if (err) {
        Tracef("shader compile failed (%s/%s): %s", entry, target, (const char*)err->GetBufferPointer());
        err->Release();
      } else {
        Tracef("shader compile failed (%s/%s) hr=0x%08lX", entry, target, (unsigned long)hr);
      }
      if (code) {
        code->Release();
      }
      return false;
    }
    if (err) {
      err->Release();
    }
    *outBlob = code;
    return true;
  }

  bool EnsurePipelinesUnlocked(AdapterState& ad, DXGI_FORMAT rtvFormat) {
    if (ad.psoDisabled) {
      return false;
    }
    if (ad.rs && ad.psoPoint && ad.psoLinear && ad.psoCatmullRom && ad.psoBicubic && ad.psoLanczos && ad.psoLanczos3 && ad.psoPixFast && ad.psoRtvFormat == rtvFormat) {
      return true;
    }

    if (ad.psoPoint) {
      ad.psoPoint->Release();
      ad.psoPoint = nullptr;
    }
    if (ad.psoLinear) {
      ad.psoLinear->Release();
      ad.psoLinear = nullptr;
    }
    if (ad.psoCatmullRom) {
      ad.psoCatmullRom->Release();
      ad.psoCatmullRom = nullptr;
    }
    if (ad.psoBicubic) {
      ad.psoBicubic->Release();
      ad.psoBicubic = nullptr;
    }
    if (ad.psoLanczos) {
      ad.psoLanczos->Release();
      ad.psoLanczos = nullptr;
    }
      if (ad.psLanczos3) {
        ad.psLanczos3->Release();
        ad.psLanczos3 = nullptr;
      }
    if (ad.psoLanczos3) {
      ad.psoLanczos3->Release();
      ad.psoLanczos3 = nullptr;
    }
    if (ad.psoPixFast) {
      ad.psoPixFast->Release();
      ad.psoPixFast = nullptr;
    }

    // Ensure we have shader blobs; dgVoodoo's pipeline cache uses ID3DBlob pointers.
    static const char* kHlsl =
      "Texture2D tex0 : register(t0, space1);\n"
      "SamplerState sampPoint : register(s0);\n"
      "SamplerState sampLinear : register(s1);\n"
      "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
      "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
      "VSOut VS(VSIn i) { VSOut o; o.pos=float4(i.pos,0.0,1.0); o.uv=i.uv; return o; }\n"
      "static const float PI = 3.14159265358979323846;\n"
      "float SafeRcp(float v) { return (abs(v) > 1e-7) ? (1.0 / v) : 0.0; }\n"
      "float CubicKeys(float x, float A) {\n"
      "  x = abs(x);\n"
      "  float x2 = x * x;\n"
      "  float x3 = x2 * x;\n"
      "  if (x <= 1.0) return (A + 2.0) * x3 - (A + 3.0) * x2 + 1.0;\n"
      "  if (x <  2.0) return A * x3 - 5.0 * A * x2 + 8.0 * A * x - 4.0 * A;\n"
      "  return 0.0;\n"
      "}\n"
      "float MitchellNetravali(float x) {\n"
      "  // Mitchell-Netravali with B=C=1/3.\n"
      "  const float B = 1.0 / 3.0;\n"
      "  const float C = 1.0 / 3.0;\n"
      "  x = abs(x);\n"
      "  float x2 = x * x;\n"
      "  float x3 = x2 * x;\n"
      "  if (x < 1.0) {\n"
      "    return ((12.0 - 9.0*B - 6.0*C) * x3 + (-18.0 + 12.0*B + 6.0*C) * x2 + (6.0 - 2.0*B)) / 6.0;\n"
      "  }\n"
      "  if (x < 2.0) {\n"
      "    return ((-B - 6.0*C) * x3 + (6.0*B + 30.0*C) * x2 + (-12.0*B - 48.0*C) * x + (8.0*B + 24.0*C)) / 6.0;\n"
      "  }\n"
      "  return 0.0;\n"
      "}\n"
      "float SincPi(float x) {\n"
      "  float ax = abs(x);\n"
      "  if (ax < 1e-5) return 1.0;\n"
      "  float px = PI * x;\n"
      "  return sin(px) / px;\n"
      "}\n"
      "float Lanczos2Weight(float x) {\n"
      "  x = abs(x);\n"
      "  if (x >= 2.0) return 0.0;\n"
      "  return SincPi(x) * SincPi(x * 0.5);\n"
      "}\n"
      "float Lanczos3Weight(float x) {\n"
      "  x = abs(x);\n"
      "  if (x >= 3.0) return 0.0;\n"
      "  return SincPi(x) * SincPi(x / 3.0);\n"
      "}\n"
      "float4 Sample4TapKernel(float2 uv, float4 wx, float4 wy) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  float2 texSize = float2((float)w, (float)h);\n"
      "  float2 coord = uv * texSize - 0.5;\n"
      "  float2 base = floor(coord);\n"
      "  float w01x = wx.x + wx.y;\n"
      "  float w23x = wx.z + wx.w;\n"
      "  float w01y = wy.x + wy.y;\n"
      "  float w23y = wy.z + wy.w;\n"
      "  float x0 = base.x - 1.0 + wx.y * SafeRcp(w01x);\n"
      "  float x1 = base.x + 1.0 + wx.w * SafeRcp(w23x);\n"
      "  float y0 = base.y - 1.0 + wy.y * SafeRcp(w01y);\n"
      "  float y1 = base.y + 1.0 + wy.w * SafeRcp(w23y);\n"
      "  float2 uv00 = (float2(x0, y0) + 0.5) / texSize;\n"
      "  float2 uv10 = (float2(x1, y0) + 0.5) / texSize;\n"
      "  float2 uv01 = (float2(x0, y1) + 0.5) / texSize;\n"
      "  float2 uv11 = (float2(x1, y1) + 0.5) / texSize;\n"
      "  float4 c00 = tex0.SampleLevel(sampLinear, uv00, 0.0);\n"
      "  float4 c10 = tex0.SampleLevel(sampLinear, uv10, 0.0);\n"
      "  float4 c01 = tex0.SampleLevel(sampLinear, uv01, 0.0);\n"
      "  float4 c11 = tex0.SampleLevel(sampLinear, uv11, 0.0);\n"
      "  float4 sum = c00 * (w01x * w01y) + c10 * (w23x * w01y) + c01 * (w01x * w23y) + c11 * (w23x * w23y);\n"
      "  float norm = (w01x + w23x) * (w01y + w23y);\n"
      "  return sum * SafeRcp(max(norm, 1e-6));\n"
      "}\n"
      "float4 SampleKeysCubic(float2 uv, float A) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  float2 texSize = float2((float)w, (float)h);\n"
      "  float2 coord = uv * texSize - 0.5;\n"
      "  float2 f = coord - floor(coord);\n"
      "  float4 dx = float4(f.x + 1.0, f.x, 1.0 - f.x, 2.0 - f.x);\n"
      "  float4 dy = float4(f.y + 1.0, f.y, 1.0 - f.y, 2.0 - f.y);\n"
      "  float4 wx = float4(CubicKeys(dx.x, A), CubicKeys(dx.y, A), CubicKeys(dx.z, A), CubicKeys(dx.w, A));\n"
      "  float4 wy = float4(CubicKeys(dy.x, A), CubicKeys(dy.y, A), CubicKeys(dy.z, A), CubicKeys(dy.w, A));\n"
      "  return Sample4TapKernel(uv, wx, wy);\n"
      "}\n"
      "float4 SampleMitchell(float2 uv) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  float2 texSize = float2((float)w, (float)h);\n"
      "  float2 coord = uv * texSize - 0.5;\n"
      "  float2 f = coord - floor(coord);\n"
      "  float4 dx = float4(f.x + 1.0, f.x, 1.0 - f.x, 2.0 - f.x);\n"
      "  float4 dy = float4(f.y + 1.0, f.y, 1.0 - f.y, 2.0 - f.y);\n"
      "  float4 wx = float4(MitchellNetravali(dx.x), MitchellNetravali(dx.y), MitchellNetravali(dx.z), MitchellNetravali(dx.w));\n"
      "  float4 wy = float4(MitchellNetravali(dy.x), MitchellNetravali(dy.y), MitchellNetravali(dy.z), MitchellNetravali(dy.w));\n"
      "  return Sample4TapKernel(uv, wx, wy);\n"
      "}\n"
      "float4 SampleLanczos2(float2 uv) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  float2 texSize = float2((float)w, (float)h);\n"
      "  float2 coord = uv * texSize - 0.5;\n"
      "  float2 f = coord - floor(coord);\n"
      "  float4 dx = float4(f.x + 1.0, f.x, 1.0 - f.x, 2.0 - f.x);\n"
      "  float4 dy = float4(f.y + 1.0, f.y, 1.0 - f.y, 2.0 - f.y);\n"
      "  float4 wx = float4(Lanczos2Weight(dx.x), Lanczos2Weight(dx.y), Lanczos2Weight(dx.z), Lanczos2Weight(dx.w));\n"
      "  float4 wy = float4(Lanczos2Weight(dy.x), Lanczos2Weight(dy.y), Lanczos2Weight(dy.z), Lanczos2Weight(dy.w));\n"
      "  return Sample4TapKernel(uv, wx, wy);\n"
      "}\n"
      "float4 SampleLanczos3(float2 uv) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  float2 texSize = float2((float)w, (float)h);\n"
      "  float2 coord = uv * texSize - 0.5;\n"
      "  float2 base = floor(coord);\n"
      "  float2 f = coord - base;\n"
      "  float wx0 = Lanczos3Weight(f.x + 2.0);\n"
      "  float wx1 = Lanczos3Weight(f.x + 1.0);\n"
      "  float wx2 = Lanczos3Weight(f.x);\n"
      "  float wx3 = Lanczos3Weight(1.0 - f.x);\n"
      "  float wx4 = Lanczos3Weight(2.0 - f.x);\n"
      "  float wx5 = Lanczos3Weight(3.0 - f.x);\n"
      "  float wy0 = Lanczos3Weight(f.y + 2.0);\n"
      "  float wy1 = Lanczos3Weight(f.y + 1.0);\n"
      "  float wy2 = Lanczos3Weight(f.y);\n"
      "  float wy3 = Lanczos3Weight(1.0 - f.y);\n"
      "  float wy4 = Lanczos3Weight(2.0 - f.y);\n"
      "  float wy5 = Lanczos3Weight(3.0 - f.y);\n"
      "  float wx01 = wx0 + wx1;\n"
      "  float wx23 = wx2 + wx3;\n"
      "  float wx45 = wx4 + wx5;\n"
      "  float wy01 = wy0 + wy1;\n"
      "  float wy23 = wy2 + wy3;\n"
      "  float wy45 = wy4 + wy5;\n"
      "  float x0 = base.x - 2.0 + wx1 * SafeRcp(wx01);\n"
      "  float x1 = base.x + 0.0 + wx3 * SafeRcp(wx23);\n"
      "  float x2 = base.x + 2.0 + wx5 * SafeRcp(wx45);\n"
      "  float y0 = base.y - 2.0 + wy1 * SafeRcp(wy01);\n"
      "  float y1 = base.y + 0.0 + wy3 * SafeRcp(wy23);\n"
      "  float y2 = base.y + 2.0 + wy5 * SafeRcp(wy45);\n"
      "  float2 uv00 = (float2(x0, y0) + 0.5) / texSize;\n"
      "  float2 uv10 = (float2(x1, y0) + 0.5) / texSize;\n"
      "  float2 uv20 = (float2(x2, y0) + 0.5) / texSize;\n"
      "  float2 uv01 = (float2(x0, y1) + 0.5) / texSize;\n"
      "  float2 uv11 = (float2(x1, y1) + 0.5) / texSize;\n"
      "  float2 uv21 = (float2(x2, y1) + 0.5) / texSize;\n"
      "  float2 uv02 = (float2(x0, y2) + 0.5) / texSize;\n"
      "  float2 uv12 = (float2(x1, y2) + 0.5) / texSize;\n"
      "  float2 uv22 = (float2(x2, y2) + 0.5) / texSize;\n"
      "  float4 c00 = tex0.SampleLevel(sampLinear, uv00, 0.0);\n"
      "  float4 c10 = tex0.SampleLevel(sampLinear, uv10, 0.0);\n"
      "  float4 c20 = tex0.SampleLevel(sampLinear, uv20, 0.0);\n"
      "  float4 c01 = tex0.SampleLevel(sampLinear, uv01, 0.0);\n"
      "  float4 c11 = tex0.SampleLevel(sampLinear, uv11, 0.0);\n"
      "  float4 c21 = tex0.SampleLevel(sampLinear, uv21, 0.0);\n"
      "  float4 c02 = tex0.SampleLevel(sampLinear, uv02, 0.0);\n"
      "  float4 c12 = tex0.SampleLevel(sampLinear, uv12, 0.0);\n"
      "  float4 c22 = tex0.SampleLevel(sampLinear, uv22, 0.0);\n"
      "  float4 row0 = c00 * wx01 + c10 * wx23 + c20 * wx45;\n"
      "  float4 row1 = c01 * wx01 + c11 * wx23 + c21 * wx45;\n"
      "  float4 row2 = c02 * wx01 + c12 * wx23 + c22 * wx45;\n"
      "  float4 sum = row0 * wy01 + row1 * wy23 + row2 * wy45;\n"
      "  float norm = (wx01 + wx23 + wx45) * (wy01 + wy23 + wy45);\n"
      "  return sum * SafeRcp(max(norm, 1e-6));\n"
      "}\n"
      "float Luma(float3 rgb) { return dot(rgb, float3(0.299, 0.587, 0.114)); }\n"
      "float4 SamplePixFast(float2 uv) {\n"
      "  uint w, h;\n"
      "  tex0.GetDimensions(w, h);\n"
      "  int2 sz = int2((int)w, (int)h);\n"
      "  float2 coord = uv * float2(sz) - 0.5;\n"
      "  int2 base = int2(floor(coord));\n"
      "  float2 f = coord - float2(base);\n"
      "  int2 p00 = clamp(base, int2(0,0), sz - 1);\n"
      "  int2 p10 = clamp(base + int2(1,0), int2(0,0), sz - 1);\n"
      "  int2 p01 = clamp(base + int2(0,1), int2(0,0), sz - 1);\n"
      "  int2 p11 = clamp(base + int2(1,1), int2(0,0), sz - 1);\n"
      "  float4 c00 = tex0.Load(int3(p00, 0));\n"
      "  float4 c10 = tex0.Load(int3(p10, 0));\n"
      "  float4 c01 = tex0.Load(int3(p01, 0));\n"
      "  float4 c11 = tex0.Load(int3(p11, 0));\n"
      "  float4 cx0 = lerp(c00, c10, f.x);\n"
      "  float4 cx1 = lerp(c01, c11, f.x);\n"
      "  float4 bil = lerp(cx0, cx1, f.y);\n"
      "  float sx = step(0.5, f.x);\n"
      "  float sy = step(0.5, f.y);\n"
      "  float4 nx0 = lerp(c00, c10, sx);\n"
      "  float4 nx1 = lerp(c01, c11, sx);\n"
      "  float4 nearest = lerp(nx0, nx1, sy);\n"
      "  float e0 = abs(Luma(c00.rgb) - Luma(c11.rgb));\n"
      "  float e1 = abs(Luma(c10.rgb) - Luma(c01.rgb));\n"
      "  float edge = max(e0, e1);\n"
      "  // Blend toward nearest on sharp edges to preserve pixel-art crispness.\n"
      "  float t = saturate((edge - 0.08) * 12.0);\n"
      "  return lerp(bil, nearest, t);\n"
      "}\n"
      "float4 PSPoint(VSOut i) : SV_Target { return tex0.Sample(sampPoint, i.uv); }\n"
      "float4 PSLinear(VSOut i) : SV_Target { return tex0.Sample(sampLinear, i.uv); }\n"
      "float4 PSCatmullRom(VSOut i) : SV_Target { return SampleKeysCubic(i.uv, -0.5); }\n"
      "float4 PSBicubic(VSOut i) : SV_Target { return SampleMitchell(i.uv); }\n"
      "float4 PSLanczos(VSOut i) : SV_Target { return SampleLanczos2(i.uv); }\n"
      "float4 PSLanczos3(VSOut i) : SV_Target { return SampleLanczos3(i.uv); }\n"
      "float4 PSPixFast(VSOut i) : SV_Target { return SamplePixFast(i.uv); }\n";

    if (!ad.vs || !ad.psPoint || !ad.psLinear || !ad.psCatmullRom || !ad.psBicubic || !ad.psLanczos || !ad.psLanczos3 || !ad.psPixFast) {
      if (ad.vs) {
        ad.vs->Release();
        ad.vs = nullptr;
      }
      if (ad.psPoint) {
        ad.psPoint->Release();
        ad.psPoint = nullptr;
      }
      if (ad.psLinear) {
        ad.psLinear->Release();
        ad.psLinear = nullptr;
      }
      if (ad.psCatmullRom) {
        ad.psCatmullRom->Release();
        ad.psCatmullRom = nullptr;
      }
      if (ad.psBicubic) {
        ad.psBicubic->Release();
        ad.psBicubic = nullptr;
      }
      if (ad.psLanczos) {
        ad.psLanczos->Release();
        ad.psLanczos = nullptr;
      }
      if (ad.psLanczos3) {
        ad.psLanczos3->Release();
        ad.psLanczos3 = nullptr;
      }
      if (ad.psPixFast) {
        ad.psPixFast->Release();
        ad.psPixFast = nullptr;
      }

      // Compile via D3DCompiler then clone into dgVoodoo-created blobs.
      // (The SDK sample uses ID3D12Root::CreateD3DBlob; using it here improves compatibility.)
      ID3DBlob* tmpVs = nullptr;
      ID3DBlob* tmpPsPoint = nullptr;
      ID3DBlob* tmpPsLinear = nullptr;
      ID3DBlob* tmpPsCr = nullptr;
      ID3DBlob* tmpPsBic = nullptr;
        ID3DBlob* tmpPsLan = nullptr;
        ID3DBlob* tmpPsLan3 = nullptr;
      ID3DBlob* tmpPsPix = nullptr;
      if (!CompileHlsl(kHlsl, "VS", "vs_5_1", &tmpVs) || !CompileHlsl(kHlsl, "PSPoint", "ps_5_1", &tmpPsPoint) ||
          !CompileHlsl(kHlsl, "PSLinear", "ps_5_1", &tmpPsLinear) || !CompileHlsl(kHlsl, "PSCatmullRom", "ps_5_1", &tmpPsCr) ||
          !CompileHlsl(kHlsl, "PSBicubic", "ps_5_1", &tmpPsBic) || !CompileHlsl(kHlsl, "PSLanczos", "ps_5_1", &tmpPsLan) ||
          !CompileHlsl(kHlsl, "PSLanczos3", "ps_5_1", &tmpPsLan3) || !CompileHlsl(kHlsl, "PSPixFast", "ps_5_1", &tmpPsPix)) {
        if (tmpVs) {
          tmpVs->Release();
        }
        if (tmpPsPoint) {
          tmpPsPoint->Release();
        }
        if (tmpPsLinear) {
          tmpPsLinear->Release();
        }
        if (tmpPsCr) {
          tmpPsCr->Release();
        }
        if (tmpPsBic) {
          tmpPsBic->Release();
        }
        if (tmpPsLan) {
          tmpPsLan->Release();
        }
        if (tmpPsLan3) {
          tmpPsLan3->Release();
        }
        if (tmpPsPix) {
          tmpPsPix->Release();
        }
        Tracef("shader compile unavailable (d3dcompiler missing?)");
        return false;
      }

      ad.vs = root_->CreateD3DBlob((UIntPtr)tmpVs->GetBufferSize(), tmpVs->GetBufferPointer());
      ad.psPoint = root_->CreateD3DBlob((UIntPtr)tmpPsPoint->GetBufferSize(), tmpPsPoint->GetBufferPointer());
      ad.psLinear = root_->CreateD3DBlob((UIntPtr)tmpPsLinear->GetBufferSize(), tmpPsLinear->GetBufferPointer());
      ad.psCatmullRom = root_->CreateD3DBlob((UIntPtr)tmpPsCr->GetBufferSize(), tmpPsCr->GetBufferPointer());
      ad.psBicubic = root_->CreateD3DBlob((UIntPtr)tmpPsBic->GetBufferSize(), tmpPsBic->GetBufferPointer());
      ad.psLanczos = root_->CreateD3DBlob((UIntPtr)tmpPsLan->GetBufferSize(), tmpPsLan->GetBufferPointer());
      ad.psLanczos3 = root_->CreateD3DBlob((UIntPtr)tmpPsLan3->GetBufferSize(), tmpPsLan3->GetBufferPointer());
      ad.psPixFast = root_->CreateD3DBlob((UIntPtr)tmpPsPix->GetBufferSize(), tmpPsPix->GetBufferPointer());

      tmpVs->Release();
      tmpPsPoint->Release();
      tmpPsLinear->Release();
      tmpPsCr->Release();
      tmpPsBic->Release();
      tmpPsLan->Release();
      tmpPsLan3->Release();
      tmpPsPix->Release();

      if (!ad.vs || !ad.psPoint || !ad.psLinear || !ad.psCatmullRom || !ad.psBicubic || !ad.psLanczos || !ad.psLanczos3 || !ad.psPixFast) {
        if (ad.vs) {
          ad.vs->Release();
          ad.vs = nullptr;
        }
        if (ad.psPoint) {
          ad.psPoint->Release();
          ad.psPoint = nullptr;
        }
        if (ad.psLinear) {
          ad.psLinear->Release();
          ad.psLinear = nullptr;
        }
        if (ad.psCatmullRom) {
          ad.psCatmullRom->Release();
          ad.psCatmullRom = nullptr;
        }
        if (ad.psBicubic) {
          ad.psBicubic->Release();
          ad.psBicubic = nullptr;
        }
        if (ad.psLanczos) {
          ad.psLanczos->Release();
          ad.psLanczos = nullptr;
        }
        if (ad.psLanczos3) {
          ad.psLanczos3->Release();
          ad.psLanczos3 = nullptr;
        }
        if (ad.psPixFast) {
          ad.psPixFast->Release();
          ad.psPixFast = nullptr;
        }
        Tracef("CreateD3DBlob failed for compiled shaders");
        return false;
      }
    }

    // Root signature: descriptor table (t0, space1) + 2 static samplers (s0 point, s1 linear).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 1;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[1]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samps[2]{};
    samps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samps[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].MipLODBias = 0.0f;
    samps[0].MaxAnisotropy = 1;
    samps[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samps[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samps[0].MinLOD = 0.0f;
    samps[0].MaxLOD = D3D12_FLOAT32_MAX;
    samps[0].ShaderRegister = 0;
    samps[0].RegisterSpace = 0;
    samps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samps[1] = samps[0];
    samps[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samps[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samps;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    if (!ad.rs) {
      // Serialize+create root signature via dgVoodoo helper (more compatible with dgVoodoo's backend).
      ID3DBlob* rsErr = nullptr;
      ad.rs = root_->SerializeAndCreateRootSignature(ad.adapterID, D3D_ROOT_SIGNATURE_VERSION_1, &rsd, &rsErr);
      if (!ad.rs) {
        if (rsErr) {
          Tracef("SerializeAndCreateRootSignature failed: %s", (const char*)rsErr->GetBufferPointer());
          rsErr->Release();
        } else {
          Tracef("SerializeAndCreateRootSignature failed (no error blob)");
        }
        return false;
      }
      if (rsErr) {
        rsErr->Release();
        rsErr = nullptr;
      }
    }

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
      auto& rt = blend.RenderTarget[i];
      rt.BlendEnable = FALSE;
      rt.LogicOpEnable = FALSE;
      rt.SrcBlend = D3D12_BLEND_ONE;
      rt.DestBlend = D3D12_BLEND_ZERO;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      rt.SrcBlendAlpha = D3D12_BLEND_ONE;
      rt.DestBlendAlpha = D3D12_BLEND_ZERO;
      rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      rt.LogicOp = D3D12_LOGIC_OP_NOOP;
      rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;
    rast.MultisampleEnable = FALSE;
    rast.AntialiasedLineEnable = FALSE;
    rast.ForcedSampleCount = 0;
    rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.StencilEnable = FALSE;
    ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.BackFace = ds.FrontFace;

    // Build a cache-friendly pipeline description using dgVoodoo's pipeline cache helpers.
    // (Direct CreateGraphicsPipelineState often fails under dgVoodoo's D3D12 backend with E_INVALIDARG.)
    ID3D12Root::GraphicsPLDesc pl{};
    std::memset(&pl, 0, sizeof(pl));
    pl.pRootSignature = ad.rs;
    pl.pVS = ad.vs;
    // We'll fill PS per variant below.
    pl.pPS = nullptr;
    pl.pDS = nullptr;
    pl.pHS = nullptr;
    pl.pGS = nullptr;
    pl.pStreamOutput = nullptr;
    pl.pBlendState = root_->PLCacheGetBlend4Desc(ad.adapterID, blend);
    pl.SampleMask = 0xFFFFFFFFu;
    pl.pRasterizerState = root_->PLCacheGetRasterizerDesc(ad.adapterID, rast);
    pl.pDepthStencilState = root_->PLCacheGetDepthStencilDesc(ad.adapterID, ds);
    // Input layout for our dynamic quad vertex buffer.
    static const D3D12_INPUT_ELEMENT_DESC kIlElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    pl.pInputLayout = root_->PLCacheGetInputLayoutDesc(ad.adapterID, 2, kIlElems);
    pl.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pl.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pl.NumRenderTargets = 1;
    for (int i = 0; i < 8; i++) {
      pl.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    }
    pl.RTVFormats[0] = rtvFormat;
    pl.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pl.SampleDesc = {1, 0};
    pl.NodeMask = 0;
    pl.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Point PSO
    pl.pPS = ad.psPoint;
    ad.plDescPoint = pl;
    ad.psoPoint = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescPoint);

    // Linear PSO
    pl.pPS = ad.psLinear;
    ad.plDescLinear = pl;
    ad.psoLinear = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescLinear);

    // Catmull-Rom cubic (Keys A=-0.5)
    pl.pPS = ad.psCatmullRom;
    ad.plDescCatmullRom = pl;
    ad.psoCatmullRom = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescCatmullRom);

    // Bicubic (Mitchell-Netravali)
    pl.pPS = ad.psBicubic;
    ad.plDescBicubic = pl;
    ad.psoBicubic = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescBicubic);

    // Lanczos2
    pl.pPS = ad.psLanczos;
    ad.plDescLanczos = pl;
    ad.psoLanczos = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescLanczos);

    // Lanczos3
    pl.pPS = ad.psLanczos3;
    ad.plDescLanczos3 = pl;
    ad.psoLanczos3 = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescLanczos3);

    // PixFast (edge-aware bilinear)
    pl.pPS = ad.psPixFast;
    ad.plDescPixFast = pl;
    ad.psoPixFast = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescPixFast);

    if (!ad.psoPoint || !ad.psoLinear || !ad.psoCatmullRom || !ad.psoBicubic || !ad.psoLanczos || !ad.psoLanczos3 || !ad.psoPixFast) {
      {
        static std::atomic<bool> loggedOnce{false};
        bool expected = false;
        if (loggedOnce.compare_exchange_strong(expected, true)) {
          const size_t vsSz = pl.pVS ? (size_t)pl.pVS->GetBufferSize() : 0;
          const size_t ps0Sz = ad.psPoint ? (size_t)ad.psPoint->GetBufferSize() : 0;
          const size_t ps1Sz = ad.psLinear ? (size_t)ad.psLinear->GetBufferSize() : 0;
          const size_t psCrSz = ad.psCatmullRom ? (size_t)ad.psCatmullRom->GetBufferSize() : 0;
          const size_t psBicSz = ad.psBicubic ? (size_t)ad.psBicubic->GetBufferSize() : 0;
          const size_t psLanSz = ad.psLanczos ? (size_t)ad.psLanczos->GetBufferSize() : 0;
          const size_t psLan3Sz = ad.psLanczos3 ? (size_t)ad.psLanczos3->GetBufferSize() : 0;
          const size_t psPixSz = ad.psPixFast ? (size_t)ad.psPixFast->GetBufferSize() : 0;
          Tracef(
              "PSO cache failure detail: rtvFmt=%u rs=%p vs=%p(v=%zu) psPoint=%p(v=%zu) psLin=%p(v=%zu) psCR=%p(v=%zu) psBic=%p(v=%zu) psLan=%p(v=%zu) psLan3=%p(v=%zu) psPix=%p(v=%zu) blend=%p rast=%p ds=%p il=%p topo=%u numRT=%u samp=(%u,%u)",
              (unsigned)rtvFormat,
              (void*)pl.pRootSignature,
              (void*)pl.pVS,
              vsSz,
              (void*)ad.psPoint,
              ps0Sz,
              (void*)ad.psLinear,
              ps1Sz,
              (void*)ad.psCatmullRom,
              psCrSz,
              (void*)ad.psBicubic,
              psBicSz,
              (void*)ad.psLanczos,
              psLanSz,
              (void*)ad.psLanczos3,
              psLan3Sz,
              (void*)ad.psPixFast,
              psPixSz,
              (void*)pl.pBlendState,
              (void*)pl.pRasterizerState,
              (void*)pl.pDepthStencilState,
              (void*)pl.pInputLayout,
              (unsigned)pl.PrimitiveTopologyType,
              (unsigned)pl.NumRenderTargets,
              (unsigned)pl.SampleDesc.Count,
              (unsigned)pl.SampleDesc.Quality);
        }
      }
      // Best-effort diagnostics: if any descriptor pointer is null, this is likely the root cause.
      if (!pl.pRootSignature || !pl.pVS || !ad.psPoint || !ad.psLinear || !ad.psCatmullRom || !ad.psBicubic || !ad.psLanczos || !ad.psLanczos3 || !ad.psPixFast || !pl.pBlendState || !pl.pRasterizerState || !pl.pDepthStencilState) {
        Tracef(
            "pipeline desc has nulls (rs=%p vs=%p psPoint=%p psLin=%p psCR=%p psBic=%p psLan=%p psLan3=%p psPix=%p blend=%p rast=%p ds=%p il=%p)",
            (void*)pl.pRootSignature,
            (void*)pl.pVS,
            (void*)ad.psPoint,
            (void*)ad.psLinear,
            (void*)ad.psCatmullRom,
            (void*)ad.psBicubic,
            (void*)ad.psLanczos,
            (void*)ad.psLanczos3,
            (void*)ad.psPixFast,
            (void*)pl.pBlendState,
            (void*)pl.pRasterizerState,
            (void*)pl.pDepthStencilState,
            (void*)pl.pInputLayout);
      }
      ad.psoFailCount++;
      const uint32_t nFail = ad.psoFailCount;
      if (nFail <= 5 || (nFail % 120) == 0) {
        Tracef(
            "PLCacheGetGraphicsPipeline failed (failCount=%lu rtvFmt=%u)",
            (unsigned long)nFail,
            (unsigned)rtvFormat);
      }
      if (nFail >= 10) {
        ad.psoDisabled = true;
        Tracef("disabling filtered scaling: PSO creation repeatedly failed");
      }
      return false;
    }

    ad.psoRtvFormat = rtvFormat;
    return true;
  }

