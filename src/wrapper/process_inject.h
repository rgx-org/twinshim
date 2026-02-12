#pragma once

#include <string>

#include <windows.h>

namespace hklmwrap {

bool InjectDllIntoProcess(HANDLE processHandle, const std::wstring& dllPath);

}
