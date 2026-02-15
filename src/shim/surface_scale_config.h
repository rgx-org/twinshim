#pragma once

#include <string>

namespace hklmwrap {

enum class SurfaceScaleMethod {
  kPoint = 0,
  kBilinear = 1,
  kBicubic = 2,
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
//   --scale-method <point|bilinear|bicubic>
// Also supports --scale=<...> and --scale-method=<...>.
const SurfaceScaleConfig& GetSurfaceScaleConfig();

const wchar_t* SurfaceScaleMethodToString(SurfaceScaleMethod m);

}
