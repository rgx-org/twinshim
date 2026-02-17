#pragma once

#include <string>

namespace hklmwrap {

enum class SurfaceScaleMethod {
  kPoint = 0,
  kBilinear = 1,
  kBicubic = 2,
  kCatmullRom = 3,
  kLanczos = 4,
  kLanczos3 = 5,
  kPixelFast = 6,
};

struct SurfaceScaleConfig {
  bool enabled = false;
  double factor = 1.0;
  SurfaceScaleMethod method = SurfaceScaleMethod::kPoint;

  bool scaleSpecified = false;
  bool methodSpecified = false;
  bool scaleValid = true;
  bool methodValid = true;

  std::wstring scaleRaw;
  std::wstring methodRaw;
};

// Parses the *target process* command line (GetCommandLineW) once and returns a cached result.
// Recognized options:
//   --scale <1.1-100>
//   --scale-method <point|bilinear|bicubic|catmull-rom|cr|lanczos|lanczos3|pixfast>
// Also supports --scale=<...> and --scale-method=<...>.
const SurfaceScaleConfig& GetSurfaceScaleConfig();

const wchar_t* SurfaceScaleMethodToString(SurfaceScaleMethod m);

}
