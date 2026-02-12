#pragma once

#include <string>

namespace hklmwrap {

std::wstring GetModulePath();
std::wstring GetDirectoryName(const std::wstring& path);
std::wstring GetFileName(const std::wstring& path);
std::wstring GetFileStem(const std::wstring& path);
std::wstring CombinePath(const std::wstring& a, const std::wstring& b);
std::wstring NormalizeSlashes(const std::wstring& path);

}
