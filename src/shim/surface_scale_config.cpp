#include "shim/surface_scale_config.h"

#include <windows.h>
#include <shellapi.h>

#include <cwctype>
#include <mutex>

namespace hklmwrap {
namespace {

static std::wstring ToLowerCopy(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& ch : out) {
    ch = (wchar_t)towlower(ch);
  }
  return out;
}

static bool TryParseDouble(const std::wstring& s, double* out) {
  if (!out) {
    return false;
  }
  *out = 0.0;
  if (s.empty()) {
    return false;
  }
  const wchar_t* begin = s.c_str();
  wchar_t* end = nullptr;
  SetLastError(0);
  const double v = wcstod(begin, &end);
  if (end == begin) {
    return false;
  }
  while (end && *end) {
    if (!iswspace(*end)) {
      return false;
    }
    end++;
  }
  *out = v;
  return true;
}

static bool ParseSurfaceScaleConfigFromCommandLine(SurfaceScaleConfig* out) {
  if (!out) {
    return false;
  }
  *out = SurfaceScaleConfig{};

  // Prefer environment variable overrides when present (set by the wrapper).
  // This lets other injected components (like a dgVoodoo AddOn) read the same
  // scale settings even if command-line parsing is impacted by third-party code.
  {
    wchar_t scaleBuf[128] = {};
    DWORD n = GetEnvironmentVariableW(L"TWINSHIM_SCALE", scaleBuf, (DWORD)(sizeof(scaleBuf) / sizeof(scaleBuf[0])));
    if (!n || n >= (DWORD)(sizeof(scaleBuf) / sizeof(scaleBuf[0]))) {
      n = GetEnvironmentVariableW(L"HKLM_WRAPPER_SCALE", scaleBuf, (DWORD)(sizeof(scaleBuf) / sizeof(scaleBuf[0])));
    }
    if (n && n < (DWORD)(sizeof(scaleBuf) / sizeof(scaleBuf[0]))) {
      scaleBuf[n] = L'\0';
      out->scaleSpecified = true;
      out->scaleRaw = scaleBuf;
      double v = 0.0;
      if (TryParseDouble(out->scaleRaw, &v) && (v >= 1.1 && v <= 100.0)) {
        out->factor = v;
        out->enabled = true;
        out->scaleValid = true;
      } else {
        out->scaleValid = false;
      }
    }
  }
  {
    wchar_t methodBuf[128] = {};
    DWORD n = GetEnvironmentVariableW(L"TWINSHIM_SCALE_METHOD", methodBuf, (DWORD)(sizeof(methodBuf) / sizeof(methodBuf[0])));
    if (!n || n >= (DWORD)(sizeof(methodBuf) / sizeof(methodBuf[0]))) {
      n = GetEnvironmentVariableW(L"HKLM_WRAPPER_SCALE_METHOD", methodBuf, (DWORD)(sizeof(methodBuf) / sizeof(methodBuf[0])));
    }
    if (n && n < (DWORD)(sizeof(methodBuf) / sizeof(methodBuf[0]))) {
      methodBuf[n] = L'\0';
      out->methodSpecified = true;
      out->methodRaw = methodBuf;
      const std::wstring lower = ToLowerCopy(out->methodRaw);
      if (lower == L"point") {
        out->method = SurfaceScaleMethod::kPoint;
      } else if (lower == L"bilinear") {
        out->method = SurfaceScaleMethod::kBilinear;
      } else if (lower == L"bicubic") {
        out->method = SurfaceScaleMethod::kBicubic;
      } else if (lower == L"catmull-rom" || lower == L"catmullrom" || lower == L"cr") {
        out->method = SurfaceScaleMethod::kCatmullRom;
      } else if (lower == L"lanczos" || lower == L"lanczos2") {
        out->method = SurfaceScaleMethod::kLanczos;
      } else if (lower == L"lanczos3") {
        out->method = SurfaceScaleMethod::kLanczos3;
      } else if (lower == L"pixfast" || lower == L"pixel" || lower == L"pix") {
        out->method = SurfaceScaleMethod::kPixelFast;
      } else {
        out->methodValid = false;
      }
    }
  }

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc <= 0) {
    if (argv) {
      LocalFree(argv);
    }
    return true;
  }

  for (int i = 1; i < argc; i++) {
    std::wstring arg = argv[i] ? argv[i] : L"";

    auto consumeValue = [&](const wchar_t* optName, std::wstring* outValue) -> bool {
      if (!outValue) {
        return false;
      }
      outValue->clear();
      if (arg == optName) {
        if (i + 1 >= argc) {
          return false;
        }
        *outValue = argv[i + 1] ? argv[i + 1] : L"";
        i++;
        return true;
      }
      const std::wstring prefix = std::wstring(optName) + L"=";
      if (arg.rfind(prefix, 0) == 0) {
        *outValue = arg.substr(prefix.size());
        return true;
      }
      return false;
    };

    std::wstring value;
    if (consumeValue(L"--scale", &value)) {
      out->scaleSpecified = true;
      out->scaleRaw = value;
      double v = 0.0;
      if (!TryParseDouble(value, &v) || !(v >= 1.1 && v <= 100.0)) {
        out->scaleValid = false;
        continue;
      }
      out->factor = v;
      out->enabled = true;
      continue;
    }

    if (consumeValue(L"--scale-method", &value)) {
      out->methodSpecified = true;
      out->methodRaw = value;
      const std::wstring lower = ToLowerCopy(value);
      if (lower == L"point") {
        out->method = SurfaceScaleMethod::kPoint;
      } else if (lower == L"bilinear") {
        out->method = SurfaceScaleMethod::kBilinear;
      } else if (lower == L"bicubic") {
        out->method = SurfaceScaleMethod::kBicubic;
      } else if (lower == L"catmull-rom" || lower == L"catmullrom" || lower == L"cr") {
        out->method = SurfaceScaleMethod::kCatmullRom;
      } else if (lower == L"lanczos" || lower == L"lanczos2") {
        out->method = SurfaceScaleMethod::kLanczos;
      } else if (lower == L"lanczos3") {
        out->method = SurfaceScaleMethod::kLanczos3;
      } else if (lower == L"pixfast" || lower == L"pixel" || lower == L"pix") {
        out->method = SurfaceScaleMethod::kPixelFast;
      } else {
        out->methodValid = false;
      }
      continue;
    }
  }

  LocalFree(argv);

  // If scale was provided but invalid, force disabled.
  if (out->scaleSpecified && !out->scaleValid) {
    out->enabled = false;
    out->factor = 1.0;
  }
  // If method was invalid, fall back to point.
  if (out->methodSpecified && !out->methodValid) {
    out->method = SurfaceScaleMethod::kPoint;
  }
  return true;
}

static std::once_flag g_once;
static SurfaceScaleConfig g_cfg;

}

const SurfaceScaleConfig& GetSurfaceScaleConfig() {
  std::call_once(g_once, [] {
    (void)ParseSurfaceScaleConfigFromCommandLine(&g_cfg);
  });
  return g_cfg;
}

const wchar_t* SurfaceScaleMethodToString(SurfaceScaleMethod m) {
  switch (m) {
    case SurfaceScaleMethod::kPoint:
      return L"point";
    case SurfaceScaleMethod::kBilinear:
      return L"bilinear";
    case SurfaceScaleMethod::kBicubic:
      return L"bicubic";
    case SurfaceScaleMethod::kCatmullRom:
      return L"catmull-rom";
    case SurfaceScaleMethod::kLanczos:
      return L"lanczos";
    case SurfaceScaleMethod::kLanczos3:
      return L"lanczos3";
    case SurfaceScaleMethod::kPixelFast:
      return L"pixfast";
    default:
      return L"unknown";
  }
}

}
