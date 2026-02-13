#include "common/utf8.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <codecvt>
#  include <locale>
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
  try {
    if constexpr (sizeof(wchar_t) == 2) {
      std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> conv;
      return conv.to_bytes(s.data(), s.data() + s.size());
    } else {
      std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> conv;
      return conv.to_bytes(s.data(), s.data() + s.size());
    }
  } catch (...) {
    return {};
  }
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
  try {
    if constexpr (sizeof(wchar_t) == 2) {
      std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> conv;
      return conv.from_bytes(s.data(), s.data() + s.size());
    } else {
      std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> conv;
      return conv.from_bytes(s.data(), s.data() + s.size());
    }
  } catch (...) {
    return {};
  }
#endif
}

}
