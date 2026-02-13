#include "shim/registry_hooks_utils.h"

#include <cstring>

namespace hklmwrap {

std::wstring CanonicalizeSubKey(const std::wstring& s) {
  std::wstring out = s;
  while (!out.empty() && (out.front() == L'\\' || out.front() == L'/')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) {
    out.pop_back();
  }
  for (auto& ch : out) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  return out;
}

std::wstring JoinKeyPath(const std::wstring& base, const std::wstring& sub) {
  if (sub.empty()) {
    return base;
  }
  if (base.empty()) {
    return sub;
  }
  if (base.back() == L'\\') {
    return base + sub;
  }
  return base + L"\\" + sub;
}

std::wstring AnsiToWide(const char* s, int len) {
  if (!s) {
    return {};
  }
  if (len == 0) {
    return {};
  }
  bool nullTerminatedInput = (len < 0);
  int needed = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out;
  out.resize((size_t)needed);
  MultiByteToWideChar(CP_ACP, 0, s, len, out.data(), needed);
  if (nullTerminatedInput && !out.empty() && out.back() == L'\0') {
    out.pop_back();
  }
  return out;
}

std::wstring CaseFold(const std::wstring& s) {
  std::wstring out;
  out.resize(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    out[i] = (wchar_t)towlower(s[i]);
  }
  return out;
}

std::vector<uint8_t> EnsureWideStringData(DWORD type, const BYTE* data, DWORD cbData) {
  // Convert ANSI string payloads to UTF-16LE for storage.
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
    if (!data || !cbData) {
      return {};
    }
    return std::vector<uint8_t>(data, data + cbData);
  }

  if (!data || cbData == 0) {
    // Empty string: include terminator.
    std::wstring empty = L"";
    std::vector<uint8_t> out((empty.size() + 1) * sizeof(wchar_t));
    std::memcpy(out.data(), empty.c_str(), out.size());
    if (type == REG_MULTI_SZ) {
      // Double-NULL terminator.
      out.resize(2 * sizeof(wchar_t), 0);
    }
    return out;
  }

  std::wstring wide = AnsiToWide(reinterpret_cast<const char*>(data), (int)cbData);
  if (wide.empty() || wide.back() != L'\0') {
    wide.push_back(L'\0');
  }
  if (type == REG_MULTI_SZ) {
    // Ensure double-null termination.
    if (wide.size() < 2 || wide[wide.size() - 2] != L'\0') {
      wide.push_back(L'\0');
    }
  }
  std::vector<uint8_t> out(wide.size() * sizeof(wchar_t));
  std::memcpy(out.data(), wide.data(), out.size());
  return out;
}

std::vector<uint8_t> WideToAnsiBytesForQuery(DWORD type, const std::vector<uint8_t>& wideBytes) {
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
    return wideBytes;
  }
  if (wideBytes.empty()) {
    return std::vector<uint8_t>{0};
  }
  const wchar_t* w = reinterpret_cast<const wchar_t*>(wideBytes.data());
  int wchars = (int)(wideBytes.size() / sizeof(wchar_t));
  int needed = WideCharToMultiByte(CP_ACP, 0, w, wchars, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::vector<uint8_t>{0};
  }
  std::vector<uint8_t> out((size_t)needed);
  WideCharToMultiByte(CP_ACP, 0, w, wchars, reinterpret_cast<char*>(out.data()), needed, nullptr, nullptr);
  // Make sure we end with null (or double-null for multi_sz).
  if (out.empty() || out.back() != 0) {
    out.push_back(0);
  }
  if (type == REG_MULTI_SZ) {
    if (out.size() < 2 || out[out.size() - 2] != 0) {
      out.push_back(0);
    }
  }
  return out;
}

}
