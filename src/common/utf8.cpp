#include "common/utf8.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cstdint>
#endif

namespace twinshim {

#ifndef _WIN32
static bool IsHighSurrogate(uint32_t v) {
  return v >= 0xD800u && v <= 0xDBFFu;
}

static bool IsLowSurrogate(uint32_t v) {
  return v >= 0xDC00u && v <= 0xDFFFu;
}

static bool AppendUtf8CodePoint(std::string& out, uint32_t cp) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
    return true;
  }
  if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return true;
  }
  if (cp <= 0xFFFFu) {
    if (IsHighSurrogate(cp) || IsLowSurrogate(cp)) {
      return false;
    }
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return true;
  }
  if (cp <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return true;
  }
  return false;
}

static bool DecodeUtf8One(const std::string& s, size_t* index, uint32_t* outCp) {
  const size_t i = *index;
  const size_t n = s.size();
  if (i >= n) {
    return false;
  }

  const uint8_t b0 = static_cast<uint8_t>(s[i]);
  if (b0 <= 0x7Fu) {
    *outCp = b0;
    *index = i + 1;
    return true;
  }

  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (i + 1 >= n) {
      return false;
    }
    const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
    if ((b1 & 0xC0u) != 0x80u) {
      return false;
    }
    *outCp = ((static_cast<uint32_t>(b0 & 0x1Fu)) << 6) | static_cast<uint32_t>(b1 & 0x3Fu);
    *index = i + 2;
    return true;
  }

  if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (i + 2 >= n) {
      return false;
    }
    const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
    const uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
      return false;
    }
    if (b0 == 0xE0u && b1 < 0xA0u) {
      return false;
    }
    if (b0 == 0xEDu && b1 >= 0xA0u) {
      return false;
    }
    *outCp = ((static_cast<uint32_t>(b0 & 0x0Fu)) << 12) |
             ((static_cast<uint32_t>(b1 & 0x3Fu)) << 6) |
             static_cast<uint32_t>(b2 & 0x3Fu);
    *index = i + 3;
    return true;
  }

  if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (i + 3 >= n) {
      return false;
    }
    const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
    const uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
    const uint8_t b3 = static_cast<uint8_t>(s[i + 3]);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
      return false;
    }
    if (b0 == 0xF0u && b1 < 0x90u) {
      return false;
    }
    if (b0 == 0xF4u && b1 > 0x8Fu) {
      return false;
    }
    *outCp = ((static_cast<uint32_t>(b0 & 0x07u)) << 18) |
             ((static_cast<uint32_t>(b1 & 0x3Fu)) << 12) |
             ((static_cast<uint32_t>(b2 & 0x3Fu)) << 6) |
             static_cast<uint32_t>(b3 & 0x3Fu);
    *index = i + 4;
    return true;
  }

  return false;
}

static bool AppendWideCodePoint(std::wstring& out, uint32_t cp) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (cp <= 0xFFFFu) {
      if (IsHighSurrogate(cp) || IsLowSurrogate(cp)) {
        return false;
      }
      out.push_back(static_cast<wchar_t>(cp));
      return true;
    }
    if (cp > 0x10FFFFu) {
      return false;
    }
    const uint32_t adjusted = cp - 0x10000u;
    const wchar_t hi = static_cast<wchar_t>(0xD800u + ((adjusted >> 10) & 0x3FFu));
    const wchar_t lo = static_cast<wchar_t>(0xDC00u + (adjusted & 0x3FFu));
    out.push_back(hi);
    out.push_back(lo);
    return true;
  } else {
    if (cp > 0x10FFFFu || IsHighSurrogate(cp) || IsLowSurrogate(cp)) {
      return false;
    }
    out.push_back(static_cast<wchar_t>(cp));
    return true;
  }
}
#endif

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
  std::string out;
  out.reserve(s.size() * 3);

  if constexpr (sizeof(wchar_t) == 2) {
    for (size_t i = 0; i < s.size(); i++) {
      const uint32_t w = static_cast<uint32_t>(s[i]);
      uint32_t cp = 0;
      if (IsHighSurrogate(w)) {
        if (i + 1 >= s.size()) {
          return {};
        }
        const uint32_t w2 = static_cast<uint32_t>(s[i + 1]);
        if (!IsLowSurrogate(w2)) {
          return {};
        }
        cp = 0x10000u + (((w - 0xD800u) << 10) | (w2 - 0xDC00u));
        i++;
      } else if (IsLowSurrogate(w)) {
        return {};
      } else {
        cp = w;
      }
      if (!AppendUtf8CodePoint(out, cp)) {
        return {};
      }
    }
  } else {
    for (wchar_t wc : s) {
      const uint32_t cp = static_cast<uint32_t>(wc);
      if (!AppendUtf8CodePoint(out, cp)) {
        return {};
      }
    }
  }
  return out;
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
  std::wstring out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = 0;
    if (!DecodeUtf8One(s, &i, &cp)) {
      return {};
    }
    if (!AppendWideCodePoint(out, cp)) {
      return {};
    }
  }
  return out;
#endif
}

}
