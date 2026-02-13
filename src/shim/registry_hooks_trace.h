#pragma once

#include <windows.h>

#include <string>

namespace hklmwrap {

class InternalDispatchGuard {
 public:
  InternalDispatchGuard();
  ~InternalDispatchGuard();
};

std::wstring FormatRegType(DWORD type);
std::wstring FormatValuePreview(DWORD type, const BYTE* data, DWORD cbData);

void TraceApiEvent(const wchar_t* apiName,
                   const wchar_t* opType,
                   const std::wstring& keyPath,
                   const std::wstring& valueName,
                   const std::wstring& valueData);

LONG TraceReadResultAndReturn(const wchar_t* apiName,
                              const std::wstring& keyPath,
                              const std::wstring& valueName,
                              LONG status,
                              bool typeKnown,
                              DWORD type,
                              const BYTE* data,
                              DWORD cbData,
                              bool sizeOnly);

LONG TraceEnumReadResultAndReturn(const wchar_t* apiName,
                                  const std::wstring& keyPath,
                                  DWORD index,
                                  const std::wstring& valueName,
                                  LONG status,
                                  bool typeKnown,
                                  DWORD type,
                                  const BYTE* data,
                                  DWORD cbData,
                                  bool sizeOnly);

}
