#pragma once

#include <string>
#include <vector>

namespace hklmwrap {

std::wstring QuoteWindowsCommandLineArg(const std::wstring& arg);
std::wstring BuildCommandLine(const std::wstring& exePath, const std::vector<std::wstring>& args);

}
