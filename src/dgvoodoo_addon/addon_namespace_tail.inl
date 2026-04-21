
static D3D12Observer g_observer;
static dgVoodoo::IAddonMainCallback* g_main = nullptr;
static std::atomic<bool> g_registered{false};

static const char* kAddonBuildId = "TwinShim SampleAddon (rev=ringbuf-11-dualpso) " __DATE__ " " __TIME__;

// Notify the injected shim that the addon is loaded and actively scaling.
// This lets the shim's mouse-hook inference path (TryInferScaledWindow)
// know it is safe to derive source dimensions from the current window size.
static void SignalAddonReadyToShim() {
  // The shim exports use __cdecl (not WINAPI/stdcall) to avoid name decoration.
  using NotifyFn = void(*)();
  HMODULE hShim = GetModuleHandleW(L"twinshim_shim.dll");
  if (!hShim) {
    hShim = GetModuleHandleW(L"twinshim_shim");
  }
  if (!hShim) {
    Tracef("SignalAddonReadyToShim: shim not loaded; skipped");
    return;
  }
  auto fn = reinterpret_cast<NotifyFn>(GetProcAddress(hShim, "TwinShim_NotifyAddonReady"));
  if (fn) {
    fn();
    Tracef("SignalAddonReadyToShim: signalled");
  } else {
    Tracef("SignalAddonReadyToShim: TwinShim_NotifyAddonReady export missing");
  }
}

static bool AddonInitCommon(dgVoodoo::IAddonMainCallback* pAddonMain) {
  // Short-circuit completely unless scaling is explicitly enabled via --scale.
  // This avoids registering for dgVoodoo callbacks and prevents any add-on work.
  double scale = 1.0;
  if (!IsScalingEnabled(&scale)) {
    g_registered.store(false, std::memory_order_release);
    g_main = nullptr;
    return true;
  }

  g_main = pAddonMain;
  Tracef("AddOnInit/AddOnInit called main=%p (%s)", (void*)pAddonMain, kAddonBuildId);
  if (!pAddonMain) {
    return false;
  }
  if (!g_observer.Init(pAddonMain)) {
    return false;
  }
  const bool ok = pAddonMain->RegisterForCallback(IID_D3D12RootObserver, &g_observer);
  g_registered.store(ok, std::memory_order_release);
  Tracef("RegisterForCallback(IID_D3D12RootObserver) -> %d", ok ? 1 : 0);

  // Signal to the shim that the addon is alive so mouse-hook inference can
  // safely engage.  This must happen after successful registration so the
  // shim doesn't start inferring before dgVoodoo is actually driving
  // the scaling pipeline.
  if (ok) {
    SignalAddonReadyToShim();
  }

  return ok;
}

static void AddonExitCommon() {
  if (!g_registered.exchange(false, std::memory_order_acq_rel)) {
    g_main = nullptr;
    return;
  }

  Tracef("AddOnExit/AddOnExit called (%s)", kAddonBuildId);
  if (g_main) {
    g_main->UnregisterForCallback(IID_D3D12RootObserver, &g_observer);
  }
  g_observer.Shutdown();
  g_main = nullptr;
}

