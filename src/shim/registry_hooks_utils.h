#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hklmwrap {

std::wstring CanonicalizeSubKey(const std::wstring& s);
std::wstring JoinKeyPath(const std::wstring& base, const std::wstring& sub);
std::wstring AnsiToWide(const char* s, int len);
std::wstring CaseFold(const std::wstring& s);
std::vector<uint8_t> EnsureWideStringData(DWORD type, const BYTE* data, DWORD cbData);
std::vector<uint8_t> WideToAnsiBytesForQuery(DWORD type, const std::vector<uint8_t>& wideBytes);

}
