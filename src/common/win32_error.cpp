#include "common/win32_error.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace hklmwrap {

std::wstring FormatWin32Error(unsigned long error) {
#ifdef _WIN32
  wchar_t* buf = nullptr;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD len = ::FormatMessageW(flags, nullptr, error, 0, (LPWSTR)&buf, 0, nullptr);
  if (!len || !buf) {
    return L"";
  }
  std::wstring msg(buf, buf + len);
  ::LocalFree(buf);
  while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) {
    msg.pop_back();
  }
  return msg;
#else
  (void)error;
  return {};
#endif
}

}
