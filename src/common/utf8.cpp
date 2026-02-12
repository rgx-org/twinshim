#include "common/utf8.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace hklmwrap {

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) {
    return {};
  }
#ifdef _WIN32
  int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out;
  out.resize((size_t)needed);
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), needed, nullptr, nullptr);
  return out;
#else
  // Non-Windows: best-effort narrow. Project builds are Windows-only.
  return std::string(s.begin(), s.end());
#endif
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
#ifdef _WIN32
  int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out;
  out.resize((size_t)needed);
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), needed);
  return out;
#else
  return std::wstring(s.begin(), s.end());
#endif
}

}
