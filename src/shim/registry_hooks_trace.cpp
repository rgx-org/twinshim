#include "shim/registry_hooks_trace.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <vector>

namespace hklmwrap {
namespace {

constexpr DWORD kMaxTraceDataBytes = 1024;

std::once_flag g_debugInitOnce;
bool g_debugAll = false;
std::vector<std::wstring> g_debugTokens;
HANDLE g_debugPipe = INVALID_HANDLE_VALUE;
std::mutex g_debugPipeMutex;
thread_local int g_internalDispatchDepth = 0;

bool ApiUsesAnsiStrings(const wchar_t* apiName) {
  if (!apiName) {
    return false;
  }
  size_t len = wcslen(apiName);
  if (len == 0) {
    return false;
  }
  return apiName[len - 1] == L'A';
}

std::wstring AnsiBytesToWideBestEffort(const char* bytes, int len) {
  if (!bytes || len <= 0) {
    return {};
  }

  // Prefer strict conversion when supported; fall back to Windows' default substitution behavior.
  const DWORD flagsToTry[] = {MB_ERR_INVALID_CHARS, 0};
  for (DWORD flags : flagsToTry) {
    int needed = MultiByteToWideChar(CP_ACP, flags, bytes, len, nullptr, 0);
    if (needed <= 0) {
      continue;
    }
    std::wstring out;
    out.resize((size_t)needed);
    if (MultiByteToWideChar(CP_ACP, flags, bytes, len, out.data(), needed) <= 0) {
      continue;
    }
    return out;
  }

  return {};
}

std::wstring TrimCopy(const std::wstring& value) {
  size_t begin = 0;
  while (begin < value.size() && std::iswspace(value[begin])) {
    begin++;
  }
  size_t end = value.size();
  while (end > begin && std::iswspace(value[end - 1])) {
    end--;
  }
  return value.substr(begin, end - begin);
}

std::wstring NormalizeApiToken(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size());
  for (wchar_t ch : in) {
    if (!std::iswspace(ch)) {
      out.push_back((wchar_t)std::towlower(ch));
    }
  }
  return out;
}

std::wstring StripAnsiWideSuffix(const std::wstring& apiNameNorm) {
  if (apiNameNorm.size() > 1) {
    wchar_t tail = apiNameNorm.back();
    if (tail == L'a' || tail == L'w') {
      return apiNameNorm.substr(0, apiNameNorm.size() - 1);
    }
  }
  return apiNameNorm;
}

void InitializeDebugConfig() {
  std::call_once(g_debugInitOnce, [] {
    wchar_t tokenBuf[4096]{};
    DWORD tokenLen =
        GetEnvironmentVariableW(L"TWINSHIM_DEBUG_APIS", tokenBuf, (DWORD)(sizeof(tokenBuf) / sizeof(tokenBuf[0])));
    if (!tokenLen || tokenLen >= (DWORD)(sizeof(tokenBuf) / sizeof(tokenBuf[0]))) {
      tokenLen =
          GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_APIS", tokenBuf, (DWORD)(sizeof(tokenBuf) / sizeof(tokenBuf[0])));
    }
    if (!tokenLen || tokenLen >= (sizeof(tokenBuf) / sizeof(tokenBuf[0]))) {
      return;
    }

    std::wstring csv(tokenBuf, tokenBuf + tokenLen);
    size_t start = 0;
    while (start <= csv.size()) {
      size_t comma = csv.find(L',', start);
      size_t end = (comma == std::wstring::npos) ? csv.size() : comma;
      std::wstring token = NormalizeApiToken(TrimCopy(csv.substr(start, end - start)));
      if (!token.empty()) {
        if (token == L"all") {
          g_debugAll = true;
          g_debugTokens.clear();
          break;
        }
        g_debugTokens.push_back(token);
      }
      if (comma == std::wstring::npos) {
        break;
      }
      start = comma + 1;
    }
  });
}

bool ShouldTraceApi(const wchar_t* apiName) {
  InitializeDebugConfig();
  if (g_debugAll) {
    return true;
  }
  if (g_debugTokens.empty() || !apiName) {
    return false;
  }

  const std::wstring apiNorm = NormalizeApiToken(apiName);
  const std::wstring apiNoAw = StripAnsiWideSuffix(apiNorm);

  for (const auto& token : g_debugTokens) {
    const std::wstring tokenNoAw = StripAnsiWideSuffix(token);
    if (tokenNoAw == apiNoAw) {
      return true;
    }
    if (tokenNoAw.size() + 2 == apiNoAw.size() && apiNoAw.rfind(tokenNoAw, 0) == 0 && apiNoAw.substr(tokenNoAw.size()) == L"ex") {
      return true;
    }
  }
  return false;
}

void EnsureDebugPipeConnected() {
  if (g_debugPipe != INVALID_HANDLE_VALUE) {
    return;
  }

  wchar_t pipeBuf[512]{};
  DWORD pipeLen =
      GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    pipeLen =
        GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  }
  if (!pipeLen || pipeLen >= (sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }

  HANDLE h = CreateFileW(std::wstring(pipeBuf, pipeBuf + pipeLen).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  g_debugPipe = h;
}

std::wstring SanitizeForLog(const std::wstring& value, size_t maxChars = 140) {
  std::wstring out;
  out.reserve(value.size());
  for (wchar_t ch : value) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
      out.push_back(L' ');
    } else {
      out.push_back(ch);
    }
  }
  if (out.size() > maxChars) {
    out.resize(maxChars);
    out += L"...";
  }
  return out;
}

std::wstring HexPreview(const BYTE* data, DWORD cbData, size_t maxBytes = 24) {
  if (!data || cbData == 0) {
    return L"<empty>";
  }
  static const wchar_t* kHex = L"0123456789ABCDEF";
  size_t used = std::min<size_t>(cbData, maxBytes);
  std::wstring out;
  out.reserve(used * 2 + 8);
  for (size_t i = 0; i < used; i++) {
    BYTE b = data[i];
    out.push_back(kHex[(b >> 4) & 0xF]);
    out.push_back(kHex[b & 0xF]);
  }
  if (used < cbData) {
    out += L"...";
  }
  return out;
}

std::wstring HexEncodeAll(const BYTE* data, DWORD cbData) {
  if (!data || cbData == 0) {
    return L"<empty>";
  }
  static const wchar_t* kHex = L"0123456789ABCDEF";
  std::wstring out;
  out.reserve((size_t)cbData * 2);
  for (DWORD i = 0; i < cbData; i++) {
    BYTE b = data[i];
    out.push_back(kHex[(b >> 4) & 0xF]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out;
  out.resize((size_t)needed);
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), needed, nullptr, nullptr);
  return out;
}

} // namespace

InternalDispatchGuard::InternalDispatchGuard() {
  g_internalDispatchDepth++;
}

InternalDispatchGuard::~InternalDispatchGuard() {
  g_internalDispatchDepth--;
}

std::wstring FormatRegType(DWORD type) {
  switch (type) {
    case REG_NONE:
      return L"REG_NONE";
    case REG_SZ:
      return L"REG_SZ";
    case REG_EXPAND_SZ:
      return L"REG_EXPAND_SZ";
    case REG_BINARY:
      return L"REG_BINARY";
    case REG_DWORD:
      return L"REG_DWORD";
    case REG_MULTI_SZ:
      return L"REG_MULTI_SZ";
    case REG_QWORD:
      return L"REG_QWORD";
    default:
      return L"REG_" + std::to_wstring(type);
  }
}

std::wstring FormatValuePreview(DWORD type, const BYTE* data, DWORD cbData) {
  if (!data || cbData == 0) {
    return L"<empty>";
  }

  if (type == REG_DWORD && cbData >= sizeof(uint32_t)) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return L"dword:" + std::to_wstring(value);
  }
  if (type == REG_QWORD && cbData >= sizeof(uint64_t)) {
    uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return L"qword:" + std::to_wstring(value);
  }

  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
    size_t chars = cbData / sizeof(wchar_t);
    size_t end = 0;
    while (end < chars && w[end] != L'\0') {
      end++;
    }
    std::wstring text(w, w + end);
    return L"str:\"" + SanitizeForLog(text) + L"\"";
  }

  if (type == REG_MULTI_SZ) {
    const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
    size_t chars = cbData / sizeof(wchar_t);
    size_t i = 0;
    std::vector<std::wstring> parts;
    while (i < chars) {
      size_t start = i;
      while (i < chars && w[i] != L'\0') {
        i++;
      }
      if (i == start) {
        break;
      }
      parts.emplace_back(w + start, w + i);
      i++;
      if (parts.size() >= 2) {
        break;
      }
    }
    std::wstring joined;
    for (size_t idx = 0; idx < parts.size(); idx++) {
      if (idx) {
        joined += L"|";
      }
      joined += SanitizeForLog(parts[idx], 40);
    }
    if (joined.empty()) {
      joined = L"<empty>";
    }
    if (parts.size() >= 2) {
      joined += L"|...";
    }
    return L"multi:\"" + joined + L"\"";
  }

  return L"hex:" + HexPreview(data, cbData);
}

std::wstring FormatValueForTrace(bool typeKnown, DWORD type, const BYTE* data, DWORD cbData, bool ansiStrings) {
  if (!data || cbData == 0) {
    return L"<empty>";
  }

  if (!typeKnown) {
    return L"hex:" + HexEncodeAll(data, cbData);
  }

  if (type == REG_DWORD && cbData >= sizeof(uint32_t)) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return L"dword:" + std::to_wstring(value);
  }
  if (type == REG_QWORD && cbData >= sizeof(uint64_t)) {
    uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return L"qword:" + std::to_wstring(value);
  }

  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    if (ansiStrings) {
      const char* bytes = reinterpret_cast<const char*>(data);
      size_t end = 0;
      while (end < cbData && bytes[end] != '\0') {
        end++;
      }
      if (end == 0) {
        return L"str:\"\"";
      }
      std::wstring text = AnsiBytesToWideBestEffort(bytes, (int)end);
      if (text.empty()) {
        return L"hex:" + HexPreview(data, cbData);
      }
      return L"str:\"" + SanitizeForLog(text, 512) + L"\"";
    } else {
      const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
      size_t chars = cbData / sizeof(wchar_t);
      size_t end = 0;
      while (end < chars && w[end] != L'\0') {
        end++;
      }
      std::wstring text(w, w + end);
      return L"str:\"" + SanitizeForLog(text, 512) + L"\"";
    }
  }

  if (type == REG_MULTI_SZ) {
    std::wstring joined;
    if (ansiStrings) {
      const char* bytes = reinterpret_cast<const char*>(data);
      size_t i = 0;
      while (i < cbData) {
        size_t start = i;
        while (i < cbData && bytes[i] != '\0') {
          i++;
        }
        if (i == start) {
          break;
        }
        if (!joined.empty()) {
          joined += L"|";
        }
        std::wstring part = AnsiBytesToWideBestEffort(bytes + start, (int)(i - start));
        if (part.empty()) {
          joined += L"<hex:" + HexPreview(reinterpret_cast<const BYTE*>(bytes + start), (DWORD)(i - start)) + L">";
        } else {
          joined += SanitizeForLog(part, 256);
        }
        // Skip the NUL separator.
        i++;
      }
    } else {
      const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
      size_t chars = cbData / sizeof(wchar_t);
      size_t i = 0;
      while (i < chars) {
        size_t start = i;
        while (i < chars && w[i] != L'\0') {
          i++;
        }
        if (i == start) {
          break;
        }
        if (!joined.empty()) {
          joined += L"|";
        }
        joined += SanitizeForLog(std::wstring(w + start, w + i), 256);
        i++;
      }
    }
    if (joined.empty()) {
      joined = L"<empty>";
    }
    return L"multi:\"" + joined + L"\"";
  }

  return L"hex:" + HexEncodeAll(data, cbData);
}

void TraceApiEvent(const wchar_t* apiName,
                   const wchar_t* opType,
                   const std::wstring& keyPath,
                   const std::wstring& valueName,
                   const std::wstring& valueData) {
  if (g_internalDispatchDepth > 0 || !ShouldTraceApi(apiName)) {
    return;
  }

  SYSTEMTIME localTime{};
  GetLocalTime(&localTime);
  const std::wstring ts = L"(" +
                          std::to_wstring((unsigned long)localTime.wHour / 10) + std::to_wstring((unsigned long)localTime.wHour % 10) + L":" +
                          std::to_wstring((unsigned long)localTime.wMinute / 10) + std::to_wstring((unsigned long)localTime.wMinute % 10) + L":" +
                          std::to_wstring((unsigned long)localTime.wSecond / 10) + std::to_wstring((unsigned long)localTime.wSecond % 10) + L"." +
                          (localTime.wMilliseconds < 100 ? (localTime.wMilliseconds < 10 ? L"00" : L"0") : L"") +
                          std::to_wstring((unsigned long)localTime.wMilliseconds) + L")";

  std::lock_guard<std::mutex> lock(g_debugPipeMutex);
  EnsureDebugPipeConnected();
  if (g_debugPipe == INVALID_HANDLE_VALUE) {
    return;
  }

  const std::wstring lineW = ts + L" [" + std::to_wstring((unsigned long)GetCurrentProcessId()) + L":" +
                             std::to_wstring((unsigned long)GetCurrentThreadId()) + L"] api=" +
                             (apiName ? std::wstring(apiName) : L"Reg?") + L" op=" +
                             (opType ? std::wstring(opType) : L"call") + L" key=\"" +
                             SanitizeForLog(keyPath.empty() ? L"-" : keyPath) + L"\" name=\"" +
                             SanitizeForLog(valueName.empty() ? L"-" : valueName) + L"\" value=\"" +
                             SanitizeForLog(valueData.empty() ? L"-" : valueData) + L"\"\n";
  std::string line = WideToUtf8(lineW);
  if (line.empty()) {
    return;
  }

  DWORD written = 0;
  if (!WriteFile(g_debugPipe, line.data(), (DWORD)line.size(), &written, nullptr)) {
    CloseHandle(g_debugPipe);
    g_debugPipe = INVALID_HANDLE_VALUE;
  }
}

LONG TraceReadResultAndReturn(const wchar_t* apiName,
                              const std::wstring& keyPath,
                              const std::wstring& valueName,
                              LONG status,
                              bool typeKnown,
                              DWORD type,
                              const BYTE* data,
                              DWORD cbData,
                              bool sizeOnly) {
  std::wstring value = L"rc=" + std::to_wstring((unsigned long)status);
  if (typeKnown) {
    value += L" type=" + FormatRegType(type);
  }
  value += L" cb=" + std::to_wstring((unsigned long)cbData);

  if (status == ERROR_SUCCESS) {
    if (data && cbData) {
      if (cbData <= kMaxTraceDataBytes) {
        value += L" data=" + FormatValueForTrace(typeKnown, type, data, cbData, ApiUsesAnsiStrings(apiName));
      } else {
        value += L" <data_present>";
      }
    } else if (sizeOnly) {
      value += L" <size_only>";
    }
  } else if (status == ERROR_MORE_DATA) {
    value += L" <more_data>";
  }

  TraceApiEvent(apiName, L"query_value", keyPath, valueName, value);
  return status;
}

LONG TraceEnumReadResultAndReturn(const wchar_t* apiName,
                                  const std::wstring& keyPath,
                                  DWORD index,
                                  const std::wstring& valueName,
                                  LONG status,
                                  bool typeKnown,
                                  DWORD type,
                                  const BYTE* data,
                                  DWORD cbData,
                                  bool sizeOnly) {
  std::wstring detail = L"idx=" + std::to_wstring((unsigned long)index) +
                        L" rc=" + std::to_wstring((unsigned long)status);
  if (typeKnown) {
    detail += L" type=" + FormatRegType(type);
  }
  detail += L" cb=" + std::to_wstring((unsigned long)cbData);

  if (status == ERROR_SUCCESS) {
    if (data && cbData) {
      if (cbData <= kMaxTraceDataBytes) {
        detail += L" data=" + FormatValueForTrace(typeKnown, type, data, cbData, ApiUsesAnsiStrings(apiName));
      } else {
        detail += L" <data_present>";
      }
    } else if (sizeOnly) {
      detail += L" <size_only>";
    }
  } else if (status == ERROR_MORE_DATA) {
    detail += L" <more_data>";
  }

  std::wstring nameField = valueName.empty() ? (L"index:" + std::to_wstring((unsigned long)index)) : valueName;
  TraceApiEvent(apiName, L"enum_value", keyPath, nameField, detail);
  return status;
}

}
