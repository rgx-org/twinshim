  dgVoodoo::IAddonMainCallback* mainCb_ = nullptr; // not owned
  ID3D12Root* root_ = nullptr; // not owned

  std::mutex mu_;
  std::unordered_map<UInt32, AdapterState> adapters_;
  std::unordered_map<ID3D12Swapchain*, SwapchainState> swapchains_;

  std::atomic<bool> didResize_{false};
  HWND resizedHwnd_ = nullptr;

  int desiredClientW_ = 0;
  int desiredClientH_ = 0;
  uint32_t resizeRetryCount_ = 0;
  uint32_t flushCountdown_ = 0;

  // Swapchain override tracking (set in D3D12CreateSwapchainHook).
  // When we create the DXGI swapchain at the scaled size, dgVoodoo may still
  // report dstRect at the original game resolution.  We track both sizes so
  // PresentBegin can use the correct viewport.
  bool swapchainOverrideActive_ = false;
  UINT swapchainOrigW_ = 0;   // Original desc dimensions from the hook.
  UINT swapchainOrigH_ = 0;
  UINT swapchainScaledW_ = 0;  // Scaled dimensions we created the swapchain with.
  UINT swapchainScaledH_ = 0;

  // No global D3D12 objects; state is per-adapter/per-swapchain.
