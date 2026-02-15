#pragma once

namespace hklmwrap {

// DirectDraw-based implementation of surface doubling (for titles that go
// through dgVoodoo's ddraw.dll rather than d3d9.dll).
//
// Controlled by target process command-line options:
//   --scale <1.1-100>
//   --scale-method <point|bilinear|bicubic>
bool InstallDDrawSurfaceDoublingHooks();
bool AreDDrawSurfaceDoublingHooksActive();
void RemoveDDrawSurfaceDoublingHooks();

}
