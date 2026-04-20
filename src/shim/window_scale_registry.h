#pragma once

#include <windows.h>

namespace twinshim {

struct ScaledWindowInfo {
  HWND hwnd = nullptr;
  int srcW = 0;
  int srcH = 0;
  int dstW = 0;
  int dstH = 0;
  double scaleFactor = 1.0;
};

// Registers a window as being physically resized for scaling, while the app/game
// still conceptually renders and/or clamps input to (srcW x srcH).
//
// Note: dstW/dstH should be the *actual* current client size after resizing.
void RegisterScaledWindow(HWND hwnd, int srcW, int srcH, int dstW, int dstH, double scaleFactor);

void UnregisterScaledWindow(HWND hwnd);

// Returns true if hwnd is known to be scaled. Out params are filled on success.
bool TryGetScaledWindow(HWND hwnd, ScaledWindowInfo* out);

// Clears all registered windows (best-effort teardown).
void ClearScaledWindows();

// Signalled by the dgVoodoo addon (or any external scaler) once it has
// initialized and is actively scaling.  The mouse-hook inference fallback
// (TryInferScaledWindow) gates on this flag so that cursor remapping only
// engages when an external scaler is actually present.
void NotifyAddonReady();
bool IsAddonReady();

} // namespace twinshim
