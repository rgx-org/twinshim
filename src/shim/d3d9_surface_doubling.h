#pragma once

namespace hklmwrap {

// Enables 2x surface doubling for Direct3D9 apps in windowed mode.
//
// Controlled by target process command-line options:
//   --scale <1.1-100>
//   --scale-method <point|bilinear|bicubic>
bool InstallD3D9SurfaceDoublingHooks();
bool AreD3D9SurfaceDoublingHooksActive();
void RemoveD3D9SurfaceDoublingHooks();

}
