#pragma once

#include <string>

namespace hklmwrap {

std::string WideToUtf8(const std::wstring& s);
std::wstring Utf8ToWide(const std::string& s);

}
