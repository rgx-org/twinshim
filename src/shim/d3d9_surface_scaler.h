#pragma once

namespace hklmwrap {

// Enables surface scaling for Direct3D9 apps in windowed mode.
//
// Controlled by target process command-line options:
//   --scale <1.1-100>
//   --scale-method <point|bilinear|bicubic|catmull-rom|cr|lanczos|lanczos3|pixfast>
bool InstallD3D9SurfaceScalerHooks();
bool AreD3D9SurfaceScalerHooksActive();
void RemoveD3D9SurfaceScalerHooks();

}
