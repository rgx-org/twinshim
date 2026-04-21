  bool Init(dgVoodoo::IAddonMainCallback* mainCb) {
    mainCb_ = mainCb;
    return true;
  }

  void Shutdown() {
    // Best-effort cleanup; actual releasing happens through swapchain/adapter callbacks.
    mainCb_ = nullptr;
    root_ = nullptr;
  }

  bool D3D12RootCreated(HMODULE /*hD3D12Dll*/, ID3D12Root* pD3D12Root) override {
    std::lock_guard<std::mutex> lock(mu_);
    root_ = pD3D12Root;
    Tracef("D3D12RootCreated root=%p", (void*)pD3D12Root);
    return true;
  }

  void D3D12RootReleased(const ID3D12Root* /*pD3D12Root*/) override {
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("D3D12RootReleased");
    root_ = nullptr;
    adapters_.clear();
    swapchains_.clear();
  }

  bool D3D12BeginUsingAdapter(UInt32 adapterID) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!root_) {
      return true;
    }
    AdapterState& st = adapters_[adapterID];
    st.adapterID = adapterID;
    st.dev = root_->GetDevice(adapterID);
    st.srvAlloc = root_->GetCBV_SRV_UAV_DescAllocator(adapterID);
    st.rtvAlloc = root_->GetRTV_DescAllocator(adapterID);
    Tracef("BeginUsingAdapter id=%u dev=%p", (unsigned)adapterID, (void*)st.dev);
    return true;
  }

  void D3D12EndUsingAdapter(UInt32 adapterID) override {
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("EndUsingAdapter id=%u", (unsigned)adapterID);
    auto it = adapters_.find(adapterID);
    if (it != adapters_.end()) {
      // NOTE: root signature / PSO are COM objects; safe to Release here.
      if (it->second.psoPoint) {
        it->second.psoPoint->Release();
        it->second.psoPoint = nullptr;
      }
      if (it->second.psoLinear) {
        it->second.psoLinear->Release();
        it->second.psoLinear = nullptr;
      }
      if (it->second.psoCatmullRom) {
        it->second.psoCatmullRom->Release();
        it->second.psoCatmullRom = nullptr;
      }
      if (it->second.psoBicubic) {
        it->second.psoBicubic->Release();
        it->second.psoBicubic = nullptr;
      }
      if (it->second.psoLanczos) {
        it->second.psoLanczos->Release();
        it->second.psoLanczos = nullptr;
      }
      if (it->second.psoLanczos3) {
        it->second.psoLanczos3->Release();
        it->second.psoLanczos3 = nullptr;
      }
      if (it->second.psoPixFast) {
        it->second.psoPixFast->Release();
        it->second.psoPixFast = nullptr;
      }

      if (it->second.vb) {
        it->second.vb->Release();
        it->second.vb = nullptr;
        it->second.vbPos = 0;
      }
      if (it->second.rs) {
        if (root_) {
          root_->GPLRootSignatureReleased(it->second.adapterID, it->second.rs);
        }
        it->second.rs->Release();
        it->second.rs = nullptr;
      }

      if (it->second.vs) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.vs);
        }
        it->second.vs->Release();
        it->second.vs = nullptr;
      }
      if (it->second.psPoint) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psPoint);
        }
        it->second.psPoint->Release();
        it->second.psPoint = nullptr;
      }
      if (it->second.psLinear) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psLinear);
        }
        it->second.psLinear->Release();
        it->second.psLinear = nullptr;
      }
      if (it->second.psCatmullRom) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psCatmullRom);
        }
        it->second.psCatmullRom->Release();
        it->second.psCatmullRom = nullptr;
      }
      if (it->second.psBicubic) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psBicubic);
        }
        it->second.psBicubic->Release();
        it->second.psBicubic = nullptr;
      }
      if (it->second.psLanczos) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psLanczos);
        }
        it->second.psLanczos->Release();
        it->second.psLanczos = nullptr;
      }
      if (it->second.psLanczos3) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psLanczos3);
        }
        it->second.psLanczos3->Release();
        it->second.psLanczos3 = nullptr;
      }
      if (it->second.psPixFast) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psPixFast);
        }
        it->second.psPixFast->Release();
        it->second.psPixFast = nullptr;
      }

      adapters_.erase(it);
    }
  }

  bool D3D12CreateSwapchainHook(UInt32 /*adapterID*/, IDXGIFactory1* /*pDxgiFactory*/, IUnknown* /*pCommandQueue*/, const DXGI_SWAP_CHAIN_DESC& /*desc*/, IDXGISwapChain** /*ppSwapChain*/) override {
    // We do not override swapchain creation.
    return false;
  }

  void D3D12SwapchainCreated(UInt32 adapterID, ID3D12Swapchain* pSwapchain, const ID3D12Root::SwapchainData& swapchainData) override {
    (void)swapchainData;
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("SwapchainCreated adapter=%u sc=%p img=%ldx%ld pres=%ldx%ld fmt=%u",
           (unsigned)adapterID,
           (void*)pSwapchain,
           (long)swapchainData.imageSize.cx,
           (long)swapchainData.imageSize.cy,
           (long)swapchainData.imagePresentationSize.cx,
           (long)swapchainData.imagePresentationSize.cy,
           (unsigned)swapchainData.format);

    SwapchainState st;
    st.adapterID = adapterID;
    st.w = (UINT)swapchainData.imagePresentationSize.cx;
    st.h = (UINT)swapchainData.imagePresentationSize.cy;
    st.fmt = swapchainData.format;

    const UINT iw = (UINT)swapchainData.imageSize.cx;
    const UINT ih = (UINT)swapchainData.imageSize.cy;
    if (iw > 1 && ih > 1) {
      st.nativeW = iw;
      st.nativeH = ih;
    } else if (st.w > 1 && st.h > 1) {
      st.nativeW = st.w;
      st.nativeH = st.h;
    }
    swapchains_[pSwapchain] = st;
  }

  void D3D12SwapchainChanged(UInt32 adapterID, ID3D12Swapchain* pSwapchain, const ID3D12Root::SwapchainData& swapchainData) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      Tracef(
          "SwapchainChanged adapter=%u sc=%p img=%ldx%ld pres=%ldx%ld maxOv=%ldx%ld fmt=%u",
             (unsigned)adapterID,
             (void*)pSwapchain,
             (long)swapchainData.imageSize.cx,
             (long)swapchainData.imageSize.cy,
             (long)swapchainData.imagePresentationSize.cx,
             (long)swapchainData.imagePresentationSize.cy,
             (long)swapchainData.maxOverriddenInputTextureSize.cx,
             (long)swapchainData.maxOverriddenInputTextureSize.cy,
             (unsigned)swapchainData.format);

      auto it = swapchains_.find(pSwapchain);
      if (it == swapchains_.end()) {
        return;
      }
      // Force re-create of our output resources on next present.
      ReleaseSwapchainOutputUnlocked(it->second);
      it->second.adapterID = adapterID;
      it->second.w = (UINT)swapchainData.imagePresentationSize.cx;
      it->second.h = (UINT)swapchainData.imagePresentationSize.cy;
      it->second.fmt = swapchainData.format;

      // Capture native size once (first meaningful value wins).
      if (it->second.nativeW == 0 || it->second.nativeH == 0) {
        const UINT iw = (UINT)swapchainData.imageSize.cx;
        const UINT ih = (UINT)swapchainData.imageSize.cy;
        if (iw > 1 && ih > 1) {
          it->second.nativeW = iw;
          it->second.nativeH = ih;
        } else if (it->second.w > 1 && it->second.h > 1) {
          it->second.nativeW = it->second.w;
          it->second.nativeH = it->second.h;
        }
      }
    }

    // Resize the host app window as soon as we learn the presentation size,
    // so the *first* PresentBegin can observe srcRect!=dstRect (allowing real
    // filtered scaling instead of a 1:1 copy).
    double scale = 1.0;
    if (IsScalingEnabled(&scale)) {
      MaybeResizeWindowOnce(scale);
    }
  }

  void D3D12SwapchainReleased(UInt32 /*adapterID*/, ID3D12Swapchain* pSwapchain) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = swapchains_.find(pSwapchain);
    if (it == swapchains_.end()) {
      return;
    }
    Tracef("SwapchainReleased sc=%p", (void*)pSwapchain);
    ReleaseSwapchainOutputUnlocked(it->second);
    ReleaseSwapchainNativeUnlocked(it->second);
    swapchains_.erase(it);
  }

  bool D3D12SwapchainPresentBegin(UInt32 adapterID, const PresentBeginContextInput& iCtx, PresentBeginContextOutput& oCtx) override {
    // Default: do nothing.
    oCtx.pOutputTexture = nullptr;
    oCtx.outputTexSRVCPUHandle.ptr = 0;
    oCtx.outputTextureExpectedState = (UINT)-1;

    double scale = 1.0;
    if (!IsScalingEnabled(&scale)) {
      return false;
    }

    {
      static std::atomic<int> callCount{0};
      const int n = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 10) {
        Tracef(
            "PresentBegin #%d sc=%p srcTex=%p srcState=%u srcRect=[%ld,%ld-%ld,%ld] dstTex=%p dstState=%u dstRect=[%ld,%ld-%ld,%ld]",
            n,
            (void*)iCtx.pSwapchain,
            (void*)iCtx.pSrcTexture,
            (unsigned)iCtx.srcTextureState,
            (long)iCtx.srcRect.left,
            (long)iCtx.srcRect.top,
            (long)iCtx.srcRect.right,
            (long)iCtx.srcRect.bottom,
            (void*)iCtx.drawingTarget.pDstTexture,
            (unsigned)iCtx.drawingTarget.dstTextureState,
            (long)iCtx.drawingTarget.dstRect.left,
            (long)iCtx.drawingTarget.dstRect.top,
            (long)iCtx.drawingTarget.dstRect.right,
            (long)iCtx.drawingTarget.dstRect.bottom);
      }
    }

    {
      static std::atomic<bool> loggedSrcState{false};
      bool expected = false;
      if (loggedSrcState.compare_exchange_strong(expected, true)) {
        Tracef("srcTextureState initial=%u", (unsigned)iCtx.srcTextureState);
      }
    }

    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        const twinshim::SurfaceScaleConfig& cfg = twinshim::GetSurfaceScaleConfig();
        char raw[256] = {};
        WideToUtf8BestEffort(cfg.methodRaw, raw, sizeof(raw));
        Tracef(
            "PresentBegin active (scale=%.3f method=%u methodSpecified=%d methodValid=%d raw='%s')",
            scale,
            (unsigned)cfg.method,
            cfg.methodSpecified ? 1 : 0,
            cfg.methodValid ? 1 : 0,
            raw);

        if (resizedHwnd_) {
          int cw = 0, ch = 0;
          if (GetClientSize(resizedHwnd_, &cw, &ch)) {
            Tracef("resized HWND %p current client=%dx%d", (void*)resizedHwnd_, cw, ch);
          } else {
            Tracef("resized HWND %p current client=<query failed>", (void*)resizedHwnd_);
          }
        }
      }
    }

    // If a resize was requested but it didn't stick (some games resize back), retry a limited number of times.
    if (resizedHwnd_ && desiredClientW_ > 0 && desiredClientH_ > 0) {
      int cw = 0, ch = 0;
      if (GetClientSize(resizedHwnd_, &cw, &ch)) {
        if ((cw != desiredClientW_ || ch != desiredClientH_) && resizeRetryCount_ < 120) {
          resizeRetryCount_++;
          const bool ok = ResizeWindowClient(resizedHwnd_, desiredClientW_, desiredClientH_);
          if (resizeRetryCount_ <= 3 || resizeRetryCount_ == 30 || resizeRetryCount_ == 120) {
            const DWORD gle = GetLastError();
            Tracef("resize retry #%u -> %dx%d (ok=%d gle=%lu)", (unsigned)resizeRetryCount_, desiredClientW_, desiredClientH_, ok ? 1 : 0, (unsigned long)gle);
          }
        }
      }
    }

    const twinshim::SurfaceScaleMethod method = GetScaleMethod();
    if (method == twinshim::SurfaceScaleMethod::kPoint) {
      // For point sampling, let dgVoodoo present normally.
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        const twinshim::SurfaceScaleConfig& cfg = twinshim::GetSurfaceScaleConfig();
        char raw[256] = {};
        WideToUtf8BestEffort(cfg.methodRaw, raw, sizeof(raw));
        Tracef("PresentBegin: method resolved to POINT; returning false (methodSpecified=%d methodValid=%d raw='%s')", cfg.methodSpecified ? 1 : 0, cfg.methodValid ? 1 : 0, raw);
      }
      return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!root_) {
      Tracef("PresentBegin: root_ is null");
      return false;
    }

    // During resize, dgVoodoo may destroy/recreate swapchain resources shortly after this callback.
    // Force submitting our recorded GPU work for a short period so dgVoodoo can fence/wait safely.
    bool forceFlush = false;
    if (flushCountdown_ > 0) {
      flushCountdown_--;
      forceFlush = true;
    }

    // Keep dgVoodoo tracking enabled so it can correctly manage swapchain/proxy resource states.

    auto itSc = swapchains_.find(iCtx.pSwapchain);
    if (itSc == swapchains_.end()) {
      Tracef("PresentBegin: swapchain not tracked sc=%p", (void*)iCtx.pSwapchain);
      return false;
    }
    SwapchainState& sc = itSc->second;
    sc.adapterID = adapterID;

    // Safety: if dgVoodoo reports the source texture as COPY_DEST, it may still be under upload/copy.
    // Sampling from it (even with a barrier) can be unstable on some drivers.
    if ((iCtx.srcTextureState & D3D12_RESOURCE_STATE_COPY_DEST) != 0) {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: srcTextureState includes COPY_DEST (%u); skipping override", (unsigned)iCtx.srcTextureState);
      }
      return false;
    }

    // Fallback native-size capture: if dgVoodoo didn't provide a meaningful imageSize at swapchain creation,
    // treat the first observed srcRect size as the native image size.
    if ((sc.nativeW == 0 || sc.nativeH == 0)) {
      const LONG sw = (iCtx.srcRect.right > iCtx.srcRect.left) ? (iCtx.srcRect.right - iCtx.srcRect.left) : 0;
      const LONG sh = (iCtx.srcRect.bottom > iCtx.srcRect.top) ? (iCtx.srcRect.bottom - iCtx.srcRect.top) : 0;
      if (sw > 1 && sh > 1) {
        sc.nativeW = (UINT)sw;
        sc.nativeH = (UINT)sh;
      }
    }

    AdapterState* ad = nullptr;
    {
      auto itAd = adapters_.find(adapterID);
      if (itAd != adapters_.end()) {
        ad = &itAd->second;
      }
    }
    if (!ad || !ad->dev || !ad->srvAlloc || !ad->rtvAlloc) {
      Tracef("PresentBegin: adapter state unavailable id=%u (dev=%p srvAlloc=%p rtvAlloc=%p)", (unsigned)adapterID, ad ? (void*)ad->dev : nullptr, ad ? (void*)ad->srvAlloc : nullptr, ad ? (void*)ad->rtvAlloc : nullptr);
      return false;
    }

    // Pick a dgVoodoo proxy texture for output. These are swapchain-sized and have valid SRV/RTV handles.
    // Preferred fast-path: if dgVoodoo provides a drawing target (swapchain RT) then draw directly into it.
    // If we return that same dst texture as output, dgVoodoo can skip its own postprocess copy.
    const bool hasDrawingTarget = (iCtx.drawingTarget.pDstTexture != nullptr) && (iCtx.drawingTarget.rtvCPUHandle.ptr != 0);
    if (hasDrawingTarget) {
      ID3D12Resource* dstTex = iCtx.drawingTarget.pDstTexture;
      UINT dstStateBefore = iCtx.drawingTarget.dstTextureState;

      // Record a simple fullscreen draw into the provided drawing target.
      ID3D12GraphicsCommandListAuto* autoCl = root_->GetGraphicsCommandListAuto(adapterID);
      if (!autoCl) {
        Tracef("PresentBegin: no auto command list");
        return false;
      }
      ID3D12GraphicsCommandList* cl = autoCl->GetCommandListInterface();
      if (!cl) {
        Tracef("PresentBegin: no command list interface");
        return false;
      }

      // Ensure our dynamic vertex buffer exists.
      if (!ad->vb) {
        ad->vb = root_->CreateDynamicBuffer(adapterID, kVbVertexCap * (UInt32)sizeof(Vertex), ID3D12Root::DA_VertexBufferPageHeapAllocator);
        ad->vbPos = 0;
        if (!ad->vb) {
          Tracef("PresentBegin: CreateDynamicBuffer failed");
          return false;
        }
      }

      // Allocate a GPU-visible SRV entry from dgVoodoo's ring buffer and copy the incoming SRV into it.
      ID3D12ResourceDescRingBuffer* srvRing = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
      if (!srvRing) {
        Tracef("PresentBegin: no SRV ring buffer");
        return false;
      }
      ID3D12ResourceDescRingBuffer::AllocData rd{};
      if (!srvRing->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rd)) {
        Tracef("PresentBegin: SRV ring alloc failed");
        return false;
      }
      ad->dev->CopyDescriptorsSimple(1, rd.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      const D3D12_FILTER filter = FilterForMethod(method);

      // Prefer the proxy texture RTV format if available; it reflects the actual RTV format used by dgVoodoo.
      DXGI_FORMAT rtvFormatToUse = sc.fmt;
      {
        ID3D12Root::SwapchainProxyTextureData tmp{};
        if (root_->GetProxyTexture(iCtx.pSwapchain, 0, &tmp)) {
          if (tmp.rtvFormat != DXGI_FORMAT_UNKNOWN) {
            rtvFormatToUse = tmp.rtvFormat;
          }
        }
      }

      {
        static std::atomic<bool> loggedRtv{false};
        bool expected = false;
        if (loggedRtv.compare_exchange_strong(expected, true)) {
          Tracef("pipeline RTV format: swapchainFmt=%u chosenFmt=%u", (unsigned)sc.fmt, (unsigned)rtvFormatToUse);
        }
      }

      // If dgVoodoo already expanded the source image to the presentation size, then our draw is 1:1 and
      // linear filtering won't be visible. In that case do a 2-pass filter:
      //   1) downsample to native size with point sampling
      //   2) upsample to destination with bilinear sampling
      const LONG srcRectW = (iCtx.srcRect.right > iCtx.srcRect.left) ? (iCtx.srcRect.right - iCtx.srcRect.left) : 0;
      const LONG srcRectH = (iCtx.srcRect.bottom > iCtx.srcRect.top) ? (iCtx.srcRect.bottom - iCtx.srcRect.top) : 0;
      const bool srcMatchesPres = (srcRectW > 0 && srcRectH > 0 && (UINT)srcRectW == sc.w && (UINT)srcRectH == sc.h);

      // If dgVoodoo already expanded the source image to the presentation size, then our draw is 1:1 and
      // any filtering won't be visible. In that case do a 2-pass filter:
      //   1) downsample to native size with point sampling
      //   2) upsample to destination with the requested method
      const bool wantTwoPassAny = (method != twinshim::SurfaceScaleMethod::kPoint) &&
                  srcMatchesPres &&
                  (sc.nativeW > 0 && sc.nativeH > 0) &&
                  (sc.w > sc.nativeW + 1 || sc.h > sc.nativeH + 1) &&
                  IsTwoPassEnabledByEnv();

      {
        static std::atomic<int> logCount{0};
        const int n = logCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10) {
          Tracef(
              "present cfg: wantTwoPass=%d native=%ux%u pres=%ux%u srcRect=%ldx%ld",
              wantTwoPassAny ? 1 : 0,
              (unsigned)sc.nativeW,
              (unsigned)sc.nativeH,
              (unsigned)sc.w,
              (unsigned)sc.h,
              (long)srcRectW,
              (long)srcRectH);
        }
      }

      const bool doTwoPass = wantTwoPassAny;

      if (doTwoPass) {
        static std::atomic<bool> loggedTwoPass{false};
        bool expectedTP = false;
        if (loggedTwoPass.compare_exchange_strong(expectedTP, true)) {
          Tracef("two-pass filter enabled: native=%ux%u pres=%ux%u", (unsigned)sc.nativeW, (unsigned)sc.nativeH, (unsigned)sc.w, (unsigned)sc.h);
        }
      }

      if (doTwoPass) {
        if (!EnsureNativeResourcesUnlocked(*ad, sc)) {
          Tracef("PresentBegin: EnsureNativeResources failed (native=%ux%u fmt=%u)", (unsigned)sc.nativeW, (unsigned)sc.nativeH, (unsigned)sc.fmt);
          return false;
        }
      }

      if (!EnsurePipelinesUnlocked(*ad, rtvFormatToUse)) {
        Tracef("PresentBegin: EnsurePipelines failed (fmt=%u)", (unsigned)rtvFormatToUse);
        return false;
      }

      autoCl->AFlushLock();

      // Ensure the incoming src texture is in a shader-resource state before sampling.
      // Do NOT transition back: dgVoodoo's tracking/presenter expects to own the subsequent transitions.
      const UINT srcStateBefore = iCtx.srcTextureState;
      if (iCtx.pSrcTexture && (srcStateBefore & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
        {
          static std::atomic<int> nLog{0};
          const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 6) {
            Tracef("barrier: src %p %u->PSR", (void*)iCtx.pSrcTexture, (unsigned)srcStateBefore);
          }
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = iCtx.pSrcTexture;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)srcStateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
      }

      // Transition dst to RT if needed.
      if (dstStateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        {
          static std::atomic<int> nLog{0};
          const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 6) {
            Tracef("barrier: dst %p %u->RT", (void*)dstTex, (unsigned)dstStateBefore);
          }
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dstTex;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)dstStateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
      }

      cl->SetGraphicsRootSignature(ad->rs);
      ID3D12PipelineState* psoOnePass = PipelineForMethod(*ad, method);
      cl->SetPipelineState(psoOnePass ? psoOnePass : ad->psoPoint);

      const LONG dstL = iCtx.drawingTarget.dstRect.left;
      const LONG dstT = iCtx.drawingTarget.dstRect.top;
      const LONG dstR = iCtx.drawingTarget.dstRect.right;
      const LONG dstB = iCtx.drawingTarget.dstRect.bottom;
      const LONG dstW = (dstR > dstL) ? (dstR - dstL) : (LONG)sc.w;
      const LONG dstH = (dstB > dstT) ? (dstB - dstT) : (LONG)sc.h;
      D3D12_VIEWPORT vp{};
      vp.TopLeftX = (float)dstL;
      vp.TopLeftY = (float)dstT;
      vp.Width = (float)dstW;
      vp.Height = (float)dstH;
      vp.MinDepth = 0.0f;
      vp.MaxDepth = 1.0f;
      D3D12_RECT sr{dstL, dstT, dstL + dstW, dstT + dstH};
      cl->RSSetViewports(1, &vp);
      cl->RSSetScissorRects(1, &sr);

      // Clear to black so letterbox areas remain black.
      const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      cl->ClearRenderTargetView(iCtx.drawingTarget.rtvCPUHandle, clear, 0, nullptr);
      cl->OMSetRenderTargets(1, &iCtx.drawingTarget.rtvCPUHandle, FALSE, nullptr);

      // Bind the ring-buffer heap + descriptor table.
      cl->SetDescriptorHeaps(1, &rd.pHeap);
      cl->SetGraphicsRootDescriptorTable(0, rd.gpuDescHandle);

      // Fill 4 vertices (triangle strip) from full srcRect (no UV clamp; clamping caused zoom/cropping).
      const D3D12_RESOURCE_DESC srcDesc = iCtx.pSrcTexture ? iCtx.pSrcTexture->GetDesc() : D3D12_RESOURCE_DESC{};
      const float srcW = (float)((srcDesc.Width > 0) ? srcDesc.Width : 1);
      const float srcH = (float)((srcDesc.Height > 0) ? srcDesc.Height : 1);
      const float tuLeft = (float)iCtx.srcRect.left / srcW;
      const float tvTop = (float)iCtx.srcRect.top / srcH;
      const float tuRight = (float)iCtx.srcRect.right / srcW;
      const float tvBottom = (float)iCtx.srcRect.bottom / srcH;

      const bool useNoOverwrite = ((ad->vbPos + 4) <= kVbVertexCap);
      const ID3D12Buffer::LockData lData = ad->vb->Lock(useNoOverwrite ? ID3D12Buffer::LT_NoOverwrite : ID3D12Buffer::LT_Discard, autoCl->AGetFence(), autoCl->GetFenceValue());
      if (!lData.ptr || lData.gpuAddress == 0) {
        Tracef("PresentBegin: vb lock failed");
        return false;
      }
      if (!useNoOverwrite) {
        ad->vbPos = 0;
      }

      volatile Vertex* v = ((Vertex*)lData.ptr) + ad->vbPos;
      v[0].pX = -1.0f;
      v[0].pY = 1.0f;
      v[0].tU = tuLeft;
      v[0].tV = tvTop;
      v[1].pX = -1.0f;
      v[1].pY = -1.0f;
      v[1].tU = tuLeft;
      v[1].tV = tvBottom;
      v[2].pX = 1.0f;
      v[2].pY = 1.0f;
      v[2].tU = tuRight;
      v[2].tV = tvTop;
      v[3].pX = 1.0f;
      v[3].pY = -1.0f;
      v[3].tU = tuRight;
      v[3].tV = tvBottom;

      const UInt64 vbGpu = lData.gpuAddress + (UInt64)ad->vbPos * (UInt64)sizeof(Vertex);
      ad->vb->Unlock();

      const D3D12_VERTEX_BUFFER_VIEW vbv{vbGpu, 4u * (UInt32)sizeof(Vertex), (UInt32)sizeof(Vertex)};

      if (!doTwoPass) {
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);
        ad->vbPos += 4;
      } else {
        // Pass 1: downsample src -> nativeTex with point sampling.
        // Allocate SRV descriptor for current src.
        ID3D12ResourceDescRingBuffer* srvRing2 = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
        ID3D12ResourceDescRingBuffer::AllocData rdSrc{};
        if (!srvRing2 || !srvRing2->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rdSrc)) {
          Tracef("PresentBegin: SRV ring alloc failed (pass1)");
          return false;
        }
        ad->dev->CopyDescriptorsSimple(1, rdSrc.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Transition nativeTex to RT.
        if (sc.nativeTexState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("twopass: pass1 native->RT begin");
            }
          }
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("barrier: native %p %u->RT", (void*)sc.nativeTex, (unsigned)sc.nativeTexState);
            }
          }
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition.pResource = sc.nativeTex;
          b.Transition.StateBefore = (D3D12_RESOURCE_STATES)sc.nativeTexState;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
          sc.nativeTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;

          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("twopass: pass1 native->RT end");
            }
          }
        }

        // Set viewport to native size.
        D3D12_VIEWPORT vp1{};
        vp1.TopLeftX = 0.0f;
        vp1.TopLeftY = 0.0f;
        vp1.Width = (float)sc.nativeW;
        vp1.Height = (float)sc.nativeH;
        vp1.MinDepth = 0.0f;
        vp1.MaxDepth = 1.0f;
        D3D12_RECT sr1{0, 0, (LONG)sc.nativeW, (LONG)sc.nativeH};
        cl->RSSetViewports(1, &vp1);
        cl->RSSetScissorRects(1, &sr1);

        // Pass 1 uses point PSO.
        cl->SetGraphicsRootSignature(ad->rs);
        cl->SetPipelineState(ad->psoPoint);
        cl->OMSetRenderTargets(1, &sc.nativeRtvCpu, FALSE, nullptr);
        cl->SetDescriptorHeaps(1, &rdSrc.pHeap);
        cl->SetGraphicsRootDescriptorTable(0, rdSrc.gpuDescHandle);
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);

        // Transition nativeTex to PS.
        {
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("barrier: native %p RT->PSR", (void*)sc.nativeTex);
            }
          }
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition.pResource = sc.nativeTex;
          b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
          sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // Pass 2 uses linear PSO (no pipeline rebuild here; avoids mid-frame release/recreate).
        ID3D12PipelineState* psoPass2 = PipelineForMethod(*ad, method);
        cl->SetPipelineState(psoPass2 ? psoPass2 : ad->psoLinear);

        // Restore dst viewport/scissor.
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sr);
        cl->OMSetRenderTargets(1, &iCtx.drawingTarget.rtvCPUHandle, FALSE, nullptr);

        ID3D12ResourceDescRingBuffer::AllocData rdNat{};
        if (!srvRing2->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rdNat)) {
          Tracef("PresentBegin: SRV ring alloc failed (pass2)");
          return false;
        }
        ad->dev->CopyDescriptorsSimple(1, rdNat.cpuDescHandle, sc.nativeSrvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cl->SetDescriptorHeaps(1, &rdNat.pHeap);
        cl->SetGraphicsRootDescriptorTable(0, rdNat.gpuDescHandle);
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);

        ad->vbPos += 4;
      }

      // Do NOT transition dst back: dgVoodoo can present directly from the swapchain texture when we return it.

      (void)autoCl->AFlushUnlock(forceFlush);

      oCtx.pOutputTexture = dstTex;
      oCtx.outputTexSRVCPUHandle.ptr = 0;
      oCtx.outputTextureExpectedState = (UINT)-1;

      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: drew into drawingTarget (filtered=%u)", (unsigned)filter);
      }
      return true;
    }

    ID3D12Root::SwapchainProxyTextureData proxy{};
    UInt32 proxyIdxChosen = (UInt32)-1;
    const UInt32 proxyCount = root_->GetMaxNumberOfProxyTextures(adapterID);
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: proxyCount=%u", (unsigned)proxyCount);
      }
    }
    for (UInt32 idx = 0; idx < proxyCount; idx++) {
      ID3D12Root::SwapchainProxyTextureData tmp{};
      if (!root_->GetProxyTexture(iCtx.pSwapchain, idx, &tmp)) {
        continue;
      }
      if (!tmp.pTexture) {
        continue;
      }
      // Avoid writing into the current source texture if it happens to be a proxy.
      if (tmp.pTexture == iCtx.pSrcTexture) {
        continue;
      }
      proxy = tmp;
      proxyIdxChosen = idx;
      break;
    }
    if (proxyIdxChosen == (UInt32)-1 || !proxy.pTexture || proxy.srvHandle.ptr == 0 || proxy.rtvHandle.ptr == 0) {
      Tracef("PresentBegin: no suitable proxy texture available (count=%u)", (unsigned)proxyCount);
      return false;
    }

    // dgVoodoo proxy textures can have an RTV format different from the swapchain format (e.g. typeless backing + concrete RTV).
    // Using the wrong RTV format can cause pipeline creation to fail in the backend.
    const DXGI_FORMAT proxyRtvFormat = (proxy.rtvFormat != DXGI_FORMAT_UNKNOWN) ? proxy.rtvFormat : sc.fmt;

    {
      static std::atomic<bool> loggedProxyFmt{false};
      bool expected = false;
      if (loggedProxyFmt.compare_exchange_strong(expected, true)) {
        Tracef("proxy RTV format: swapchainFmt=%u proxyRtvFmt=%u chosenFmt=%u", (unsigned)sc.fmt, (unsigned)proxy.rtvFormat, (unsigned)proxyRtvFormat);
      }
    }

    const UINT proxyStateBefore = proxy.texState;

    const D3D12_FILTER filter = FilterForMethod(method);
    if (!EnsurePipelinesUnlocked(*ad, proxyRtvFormat)) {
      Tracef("PresentBegin: EnsurePipelines failed (fmt=%u)", (unsigned)proxyRtvFormat);
      return false;
    }

    // Record a simple fullscreen draw into our output texture.
    ID3D12GraphicsCommandListAuto* autoCl = root_->GetGraphicsCommandListAuto(adapterID);
    if (!autoCl) {
      return false;
    }
    ID3D12GraphicsCommandList* cl = autoCl->GetCommandListInterface();
    if (!cl) {
      return false;
    }

    // Allocate a GPU-visible SRV entry from dgVoodoo's ring buffer and copy the incoming SRV into it.
    ID3D12ResourceDescRingBuffer* srvRing = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
    if (!srvRing) {
      Tracef("PresentBegin: no SRV ring buffer");
      return false;
    }
    ID3D12ResourceDescRingBuffer::AllocData rd{};
    if (!srvRing->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rd)) {
      Tracef("PresentBegin: SRV ring alloc failed");
      return false;
    }
    ad->dev->CopyDescriptorsSimple(1, rd.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    autoCl->AFlushLock();

    // Ensure the incoming src texture is in a shader-resource state before sampling.
    // Do NOT transition back: dgVoodoo's tracking/presenter expects to own the subsequent transitions.
    const UINT srcStateBefore = iCtx.srcTextureState;
    if (iCtx.pSrcTexture && (srcStateBefore & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
      {
        static std::atomic<int> nLog{0};
        const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 6) {
          Tracef("barrier: src %p %u->PSR", (void*)iCtx.pSrcTexture, (unsigned)srcStateBefore);
        }
      }
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = iCtx.pSrcTexture;
      b.Transition.StateBefore = (D3D12_RESOURCE_STATES)srcStateBefore;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
    }

    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: using proxy idx=%u state=%u srcState=%u", (unsigned)proxyIdxChosen, (unsigned)proxy.texState, (unsigned)iCtx.srcTextureState);
      }
    }

    // Transition proxy output to RT.
    if (proxy.texState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = proxy.pTexture;
      b.Transition.StateBefore = (D3D12_RESOURCE_STATES)proxy.texState;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      proxy.texState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Set pipeline.
    cl->SetGraphicsRootSignature(ad->rs);
    {
      ID3D12PipelineState* pso = PipelineForMethod(*ad, method);
      cl->SetPipelineState(pso ? pso : ad->psoPoint);
    }

    // Viewport + scissor to destination rect (handles aspect-ratio letterboxing cases).
    const LONG dstL = iCtx.drawingTarget.dstRect.left;
    const LONG dstT = iCtx.drawingTarget.dstRect.top;
    const LONG dstR = iCtx.drawingTarget.dstRect.right;
    const LONG dstB = iCtx.drawingTarget.dstRect.bottom;
    const LONG dstW = (dstR > dstL) ? (dstR - dstL) : (LONG)sc.w;
    const LONG dstH = (dstB > dstT) ? (dstB - dstT) : (LONG)sc.h;

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = (float)dstL;
    vp.TopLeftY = (float)dstT;
    vp.Width = (float)dstW;
    vp.Height = (float)dstH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr{dstL, dstT, dstL + dstW, dstT + dstH};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);

    // Clear to black so areas outside dstRect remain black.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cl->ClearRenderTargetView(proxy.rtvHandle, clear, 0, nullptr);

    // RTV.
    cl->OMSetRenderTargets(1, &proxy.rtvHandle, FALSE, nullptr);

    // Bind the ring-buffer heap + descriptor table.
    cl->SetDescriptorHeaps(1, &rd.pHeap);
    cl->SetGraphicsRootDescriptorTable(0, rd.gpuDescHandle);

    // Ensure our dynamic vertex buffer exists.
    if (!ad->vb) {
      ad->vb = root_->CreateDynamicBuffer(adapterID, kVbVertexCap * (UInt32)sizeof(Vertex), ID3D12Root::DA_VertexBufferPageHeapAllocator);
      ad->vbPos = 0;
      if (!ad->vb) {
        Tracef("PresentBegin: CreateDynamicBuffer failed");
        return false;
      }
    }

    // Fill 4 vertices (triangle strip) with the same native-size UV clamp logic as the drawingTarget path.
    const D3D12_RESOURCE_DESC srcDesc = iCtx.pSrcTexture ? iCtx.pSrcTexture->GetDesc() : D3D12_RESOURCE_DESC{};
    const float srcW = (float)((srcDesc.Width > 0) ? srcDesc.Width : 1);
    const float srcH = (float)((srcDesc.Height > 0) ? srcDesc.Height : 1);
    const LONG nativeR = (sc.nativeW > 0) ? (LONG)sc.nativeW : iCtx.srcRect.right;
    const LONG nativeB = (sc.nativeH > 0) ? (LONG)sc.nativeH : iCtx.srcRect.bottom;
    const LONG useR = (iCtx.srcRect.right < nativeR) ? iCtx.srcRect.right : nativeR;
    const LONG useB = (iCtx.srcRect.bottom < nativeB) ? iCtx.srcRect.bottom : nativeB;
    const float tuLeft = (float)iCtx.srcRect.left / srcW;
    const float tvTop = (float)iCtx.srcRect.top / srcH;
    const float tuRight = (float)useR / srcW;
    const float tvBottom = (float)useB / srcH;

    const bool useNoOverwrite = ((ad->vbPos + 4) <= kVbVertexCap);
    const ID3D12Buffer::LockData lData = ad->vb->Lock(useNoOverwrite ? ID3D12Buffer::LT_NoOverwrite : ID3D12Buffer::LT_Discard, autoCl->AGetFence(), autoCl->GetFenceValue());
    if (!lData.ptr || lData.gpuAddress == 0) {
      Tracef("PresentBegin: vb lock failed");
      return false;
    }
    if (!useNoOverwrite) {
      ad->vbPos = 0;
    }

    volatile Vertex* v = ((Vertex*)lData.ptr) + ad->vbPos;
    v[0].pX = -1.0f;
    v[0].pY = 1.0f;
    v[0].tU = tuLeft;
    v[0].tV = tvTop;
    v[1].pX = -1.0f;
    v[1].pY = -1.0f;
    v[1].tU = tuLeft;
    v[1].tV = tvBottom;
    v[2].pX = 1.0f;
    v[2].pY = 1.0f;
    v[2].tU = tuRight;
    v[2].tV = tvTop;
    v[3].pX = 1.0f;
    v[3].pY = -1.0f;
    v[3].tU = tuRight;
    v[3].tV = tvBottom;

    const UInt64 vbGpu = lData.gpuAddress + (UInt64)ad->vbPos * (UInt64)sizeof(Vertex);
    ad->vb->Unlock();

    const D3D12_VERTEX_BUFFER_VIEW vbv{vbGpu, 4u * (UInt32)sizeof(Vertex), (UInt32)sizeof(Vertex)};
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    cl->DrawInstanced(4, 1, 0, 0);
    ad->vbPos += 4;

    // Do NOT transition src back.

    // Transition proxy output to SRV state for presenter.
    {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = proxy.pTexture;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      proxy.texState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    (void)autoCl->AFlushUnlock(forceFlush);

    // Output override.
    oCtx.pOutputTexture = proxy.pTexture;
    oCtx.outputTexSRVCPUHandle = proxy.srvHandle;
    oCtx.outputTextureExpectedState = (UINT)-1;
    return true;
  }

  void D3D12SwapchainPresentEnd(UInt32 /*adapterID*/, const PresentEndContextInput& /*iCtx*/) override {
    // Not used.
  }

