#include "common/path_util.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace hklmwrap {

std::wstring GetModulePath() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return {};
  }
  return std::wstring(buf, buf + n);
#else
  return {};
#endif
}

std::wstring NormalizeSlashes(const std::wstring& path) {
  std::wstring out = path;
  for (auto& ch : out) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  return out;
}

std::wstring GetDirectoryName(const std::wstring& path) {
  auto p = NormalizeSlashes(path);
  auto pos = p.find_last_of(L"\\");
  if (pos == std::wstring::npos) {
    return {};
  }
  return p.substr(0, pos);
}

std::wstring GetFileName(const std::wstring& path) {
  auto p = NormalizeSlashes(path);
  auto pos = p.find_last_of(L"\\");
  if (pos == std::wstring::npos) {
    return p;
  }
  return p.substr(pos + 1);
}

std::wstring GetFileStem(const std::wstring& path) {
  auto name = GetFileName(path);
  auto pos = name.find_last_of(L'.');
  if (pos == std::wstring::npos) {
    return name;
  }
  return name.substr(0, pos);
}

std::wstring CombinePath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  auto a2 = NormalizeSlashes(a);
  auto b2 = NormalizeSlashes(b);
  if (a2.back() == L'\\') {
    return a2 + b2;
  }
  return a2 + L"\\" + b2;
}

}
