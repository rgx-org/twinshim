#pragma once

#include <stdbool.h>

namespace hklmwrap {

// Shared MinHook initialization lifetime for the injected shim.
//
// Multiple hook modules (e.g. registry hooks + D3D9 hooks) may need MinHook.
// MinHook itself is process-global, so we ref-count initialization to avoid
// double init/uninit bugs.
bool AcquireMinHook();
void ReleaseMinHook();

}
