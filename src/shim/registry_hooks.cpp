#include "shim/registry_hooks.h"

#include "common/local_registry_store.h"
#include "common/path_util.h"

#include <MinHook.h>

#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace hklmwrap {

namespace {

constexpr uint32_t kKeyMagic = 0x4D4C4B48u; // 'HKLM'

struct VirtualKey {
  uint32_t magic = kKeyMagic;
  HKEY real = nullptr;
  std::wstring keyPath; // Canonical: HKLM\\... (no trailing slash)
};

std::mutex g_virtualKeysMutex;
std::unordered_set<VirtualKey*> g_virtualKeys;

thread_local bool g_bypass = false;

struct BypassGuard {
  BypassGuard() { g_bypass = true; }
  ~BypassGuard() { g_bypass = false; }
};

bool IsVirtual(HKEY h) {
  auto* vk = reinterpret_cast<VirtualKey*>(h);
  std::lock_guard<std::mutex> lock(g_virtualKeysMutex);
  return g_virtualKeys.find(vk) != g_virtualKeys.end();
}

VirtualKey* AsVirtual(HKEY h) {
  return IsVirtual(h) ? reinterpret_cast<VirtualKey*>(h) : nullptr;
}

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

bool IsHKLMRoot(HKEY h) {
  return h == HKEY_LOCAL_MACHINE;
}

LocalRegistryStore g_store;
std::once_flag g_openOnce;
std::mutex g_storeMutex;

std::once_flag g_debugInitOnce;
bool g_debugAll = false;
std::vector<std::wstring> g_debugTokens;
HANDLE g_debugPipe = INVALID_HANDLE_VALUE;
std::mutex g_debugPipeMutex;
thread_local int g_internalDispatchDepth = 0;

struct InternalDispatchGuard {
  InternalDispatchGuard() { g_internalDispatchDepth++; }
  ~InternalDispatchGuard() { g_internalDispatchDepth--; }
};

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
        GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_APIS", tokenBuf, (DWORD)(sizeof(tokenBuf) / sizeof(tokenBuf[0])));
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
      GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }

  HANDLE h = CreateFileW(std::wstring(pipeBuf, pipeBuf + pipeLen).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  g_debugPipe = h;
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

void TraceApiEvent(const wchar_t* apiName,
                   const wchar_t* opType,
                   const std::wstring& keyPath,
                   const std::wstring& valueName,
                   const std::wstring& valueData) {
  if (g_internalDispatchDepth > 0 || !ShouldTraceApi(apiName)) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_debugPipeMutex);
  EnsureDebugPipeConnected();
  if (g_debugPipe == INVALID_HANDLE_VALUE) {
    return;
  }

  const std::wstring lineW = L"[" + std::to_wstring((unsigned long)GetCurrentProcessId()) + L":" +
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

void TraceApiCall(const wchar_t* apiName) {
  TraceApiEvent(apiName, L"call", L"-", L"-", L"-");
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
      value += L" " + FormatValuePreview(typeKnown ? type : REG_BINARY, data, cbData);
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
      detail += L" " + FormatValuePreview(typeKnown ? type : REG_BINARY, data, cbData);
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

void EnsureStoreOpen() {
  std::call_once(g_openOnce, [] {
    wchar_t dbPath[4096];
    DWORD n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DB_PATH", dbPath, (DWORD)(sizeof(dbPath) / sizeof(dbPath[0])));
    if (!n || n >= (sizeof(dbPath) / sizeof(dbPath[0]))) {
      // Fallback: next to the target EXE.
      std::wstring exePath;
      exePath.resize(32768);
      DWORD got = GetModuleFileNameW(nullptr, exePath.data(), (DWORD)exePath.size());
      if (got) {
        exePath.resize(got);
      } else {
        exePath.clear();
      }
      std::wstring dir = GetDirectoryName(exePath);
      std::wstring stem = GetFileStem(exePath);
      std::wstring fallback = CombinePath(dir, stem + L"-HKLM.sqlite");
      g_store.Open(fallback);
      return;
    }
    g_store.Open(std::wstring(dbPath, dbPath + n));
  });
}

// Original function pointers.
decltype(&RegOpenKeyExW) fpRegOpenKeyExW = nullptr;
decltype(&RegCreateKeyExW) fpRegCreateKeyExW = nullptr;
decltype(&RegCloseKey) fpRegCloseKey = nullptr;
decltype(&RegSetValueExW) fpRegSetValueExW = nullptr;
decltype(&RegQueryValueExW) fpRegQueryValueExW = nullptr;
decltype(&RegDeleteValueW) fpRegDeleteValueW = nullptr;
decltype(&RegDeleteKeyW) fpRegDeleteKeyW = nullptr;
decltype(&RegDeleteKeyExW) fpRegDeleteKeyExW = nullptr;

decltype(&RegOpenKeyW) fpRegOpenKeyW = nullptr;
decltype(&RegOpenKeyA) fpRegOpenKeyA = nullptr;
decltype(&RegCreateKeyW) fpRegCreateKeyW = nullptr;
decltype(&RegCreateKeyA) fpRegCreateKeyA = nullptr;
decltype(&RegQueryValueW) fpRegQueryValueW = nullptr;
decltype(&RegQueryValueA) fpRegQueryValueA = nullptr;
decltype(&RegSetValueW) fpRegSetValueW = nullptr;
decltype(&RegSetValueA) fpRegSetValueA = nullptr;

decltype(&RegEnumValueW) fpRegEnumValueW = nullptr;
decltype(&RegEnumValueA) fpRegEnumValueA = nullptr;
decltype(&RegEnumKeyExW) fpRegEnumKeyExW = nullptr;
decltype(&RegEnumKeyExA) fpRegEnumKeyExA = nullptr;
decltype(&RegEnumKeyW) fpRegEnumKeyW = nullptr;
decltype(&RegEnumKeyA) fpRegEnumKeyA = nullptr;
decltype(&RegQueryInfoKeyW) fpRegQueryInfoKeyW = nullptr;
decltype(&RegQueryInfoKeyA) fpRegQueryInfoKeyA = nullptr;

decltype(&RegSetKeyValueW) fpRegSetKeyValueW = nullptr;
decltype(&RegSetKeyValueA) fpRegSetKeyValueA = nullptr;

decltype(&RegOpenKeyExA) fpRegOpenKeyExA = nullptr;
decltype(&RegCreateKeyExA) fpRegCreateKeyExA = nullptr;
decltype(&RegSetValueExA) fpRegSetValueExA = nullptr;
decltype(&RegQueryValueExA) fpRegQueryValueExA = nullptr;
decltype(&RegDeleteValueA) fpRegDeleteValueA = nullptr;
decltype(&RegDeleteKeyA) fpRegDeleteKeyA = nullptr;

LONG WINAPI Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LONG WINAPI Hook_RegCreateKeyExA(HKEY hKey,
                                 LPCSTR lpSubKey,
                                 DWORD Reserved,
                                 LPSTR lpClass,
                                 DWORD dwOptions,
                                 REGSAM samDesired,
                                 const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                 PHKEY phkResult,
                                 LPDWORD lpdwDisposition);

std::wstring KeyPathFromHandle(HKEY hKey) {
  if (auto* vk = AsVirtual(hKey)) {
    return vk->keyPath;
  }
  if (IsHKLMRoot(hKey)) {
    return L"HKLM";
  }
  return L"";
}

HKEY RealHandleForFallback(HKEY hKey) {
  if (auto* vk = AsVirtual(hKey)) {
    return vk->real;
  }
  return hKey;
}

VirtualKey* NewVirtualKey(const std::wstring& keyPath, HKEY real) {
  auto* vk = new VirtualKey();
  vk->keyPath = keyPath;
  vk->real = real;
  {
    std::lock_guard<std::mutex> lock(g_virtualKeysMutex);
    g_virtualKeys.insert(vk);
  }
  return vk;
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

struct MergedNames {
  std::vector<std::wstring> names;          // original spelling
  std::unordered_set<std::wstring> folded;  // case-fold set
  std::unordered_set<std::wstring> deleted; // case-fold set
};

MergedNames GetMergedValueNames(const std::wstring& keyPath, HKEY real) {
  MergedNames merged;
  std::unordered_map<std::wstring, std::wstring> foldedToName;

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    for (const auto& r : g_store.ListValues(keyPath)) {
      auto f = CaseFold(r.valueName);
      if (r.isDeleted) {
        merged.deleted.insert(f);
        merged.folded.insert(f);
        foldedToName.emplace(f, r.valueName);
      } else {
        merged.folded.insert(f);
        foldedToName[f] = r.valueName;
      }
    }
  }

  // Include local non-deleted first.
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    for (const auto& r : g_store.ListValues(keyPath)) {
      if (r.isDeleted) {
        continue;
      }
      merged.names.push_back(r.valueName);
    }
  }

  // Merge real values.
  if (real && fpRegEnumValueW) {
    DWORD index = 0;
    while (true) {
      std::wstring name;
      DWORD type = 0;
      LONG lastRc = ERROR_SUCCESS;

      std::vector<wchar_t> buf(256);
      while (true) {
        DWORD nameLen = (DWORD)buf.size();
        {
          BypassGuard guard;
          lastRc = fpRegEnumValueW(real, index, buf.data(), &nameLen, nullptr, &type, nullptr, nullptr);
        }
        if (lastRc == ERROR_MORE_DATA) {
          buf.resize((size_t)nameLen + 1);
          continue;
        }
        if (lastRc == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (lastRc != ERROR_SUCCESS) {
          break;
        }
        name.assign(buf.data(), buf.data() + nameLen);
        break;
      }

      if (lastRc == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (lastRc != ERROR_SUCCESS) {
        break;
      }

      auto f = CaseFold(name);
      if (merged.deleted.find(f) == merged.deleted.end() && merged.folded.find(f) == merged.folded.end()) {
        merged.folded.insert(f);
        merged.names.push_back(name);
      }
      index++;
      if (index > 100000) {
        break;
      }
    }
  }

  std::sort(merged.names.begin(), merged.names.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  return merged;
}

std::vector<std::wstring> GetMergedSubKeyNames(const std::wstring& keyPath, HKEY real) {
  std::unordered_set<std::wstring> deleted;
  std::unordered_set<std::wstring> folded;
  std::vector<std::wstring> out;

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    // Deleted children: any immediate child whose full path is deleted.
    for (const auto& child : g_store.ListImmediateSubKeys(keyPath)) {
      std::wstring full = keyPath;
      full.append(L"\\");
      full.append(child);
      if (g_store.IsKeyDeleted(full)) {
        deleted.insert(CaseFold(child));
      }
    }
    for (const auto& child : g_store.ListImmediateSubKeys(keyPath)) {
      std::wstring full = keyPath;
      full.append(L"\\");
      full.append(child);
      if (g_store.IsKeyDeleted(full)) {
        continue;
      }
      auto f = CaseFold(child);
      folded.insert(f);
      out.push_back(child);
    }
  }

  if (real && fpRegEnumKeyExW) {
    DWORD index = 0;
    while (true) {
      std::wstring name;
      std::vector<wchar_t> buf(256);
      while (true) {
        DWORD nameLen = (DWORD)buf.size();
        LONG rc;
        {
          BypassGuard guard;
          rc = fpRegEnumKeyExW(real, index, buf.data(), &nameLen, nullptr, nullptr, nullptr, nullptr);
        }
        if (rc == ERROR_MORE_DATA) {
          buf.resize((size_t)nameLen + 1);
          continue;
        }
        if (rc == ERROR_NO_MORE_ITEMS) {
          name.clear();
          break;
        }
        if (rc != ERROR_SUCCESS) {
          name.clear();
          break;
        }
        name.assign(buf.data(), buf.data() + nameLen);
        break;
      }

      if (name.empty()) {
        break;
      }

      auto f = CaseFold(name);
      if (deleted.find(f) == deleted.end() && folded.find(f) == folded.end()) {
        folded.insert(f);
        out.push_back(name);
      }
      index++;
      if (index > 100000) {
        break;
      }
    }
  }

  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
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

void DeleteVirtualKey(VirtualKey* vk) {
  if (!vk) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_virtualKeysMutex);
    g_virtualKeys.erase(vk);
  }
  delete vk;
}

// --- Hooks ---

LONG WINAPI Hook_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
  if (g_bypass) {
    return fpRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }

  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegOpenKeyExW", L"open_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      *phkResult = nullptr;
      return ERROR_FILE_NOT_FOUND;
    }
    if (g_store.KeyExistsLocally(full)) {
      *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
      return ERROR_SUCCESS;
    }
  }

  // Fall back to real registry for read-only/open.
  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  {
    BypassGuard guard;
    LONG rc = ERROR_FILE_NOT_FOUND;
    if (realParent) {
      rc = fpRegOpenKeyExW(realParent, lpSubKey, ulOptions, samDesired, &realOut);
    } else {
      // Parent is virtual-only; attempt an absolute open from HKLM using the full canonical path.
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        rc = fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, absSub.c_str(), 0, samDesired, &realOut);
      }
    }
    if (rc != ERROR_SUCCESS) {
      *phkResult = nullptr;
      return rc;
    }
  }
  *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, realOut));
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegCreateKeyExW(HKEY hKey,
                                LPCWSTR lpSubKey,
                                DWORD Reserved,
                                LPWSTR lpClass,
                                DWORD dwOptions,
                                REGSAM samDesired,
                                const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                PHKEY phkResult,
                                LPDWORD lpdwDisposition) {
  if (g_bypass) {
    return fpRegCreateKeyExW(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }

  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegCreateKeyExW", L"create_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegCreateKeyExW(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      // Creating a key should undelete it.
      g_store.PutKey(full);
    } else {
      g_store.PutKey(full);
    }
  }

  // For read fallback, try to open the real key (read-only) but don't create it.
  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  {
    BypassGuard guard;
    if (realParent) {
      fpRegOpenKeyExW(realParent, lpSubKey, 0, KEY_READ | (samDesired & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)), &realOut);
    } else {
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, absSub.c_str(), 0, KEY_READ, &realOut);
      }
    }
  }

  *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, realOut));
  if (lpdwDisposition) {
    *lpdwDisposition = REG_OPENED_EXISTING_KEY;
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegCloseKey(HKEY hKey) {
  if (g_bypass) {
    return fpRegCloseKey(hKey);
  }
  TraceApiEvent(L"RegCloseKey", L"close_key", KeyPathFromHandle(hKey), L"-", L"-");
  if (auto* vk = AsVirtual(hKey)) {
    if (vk->real) {
      BypassGuard guard;
      fpRegCloseKey(vk->real);
      vk->real = nullptr;
    }
    DeleteVirtualKey(vk);
    return ERROR_SUCCESS;
  }
  return fpRegCloseKey(hKey);
}

LONG WINAPI Hook_RegSetValueExW(HKEY hKey,
                               LPCWSTR lpValueName,
                               DWORD Reserved,
                               DWORD dwType,
                               const BYTE* lpData,
                               DWORD cbData) {
  if (g_bypass) {
    return fpRegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? std::wstring(lpValueName) : std::wstring();
  TraceApiEvent(L"RegSetValueExW",
                L"set_value",
                keyPath,
                valueName,
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, lpData, cbData));
  if (keyPath.empty()) {
    return fpRegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(keyPath)) {
      // Writing into a deleted key implicitly undeletes the key.
      g_store.PutKey(keyPath);
    }
    if (!g_store.PutValue(keyPath, valueName, (uint32_t)dwType, lpData, cbData)) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegQueryValueExW(HKEY hKey,
                                 LPCWSTR lpValueName,
                                 LPDWORD lpReserved,
                                 LPDWORD lpType,
                                 LPBYTE lpData,
                                 LPDWORD lpcbData) {
  if (g_bypass) {
    return fpRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
  }

  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? std::wstring(lpValueName) : std::wstring();
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegQueryValueExW(hKey, lpValueName, lpReserved, typeOut, lpData, lpcbData);
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueExW", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(keyPath, valueName);
    if (v.has_value()) {
      if (v->isDeleted) {
        return TraceReadResultAndReturn(
            L"RegQueryValueExW", keyPath, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
      }
      if (lpType) {
        *lpType = (DWORD)v->type;
      }
      DWORD needed = (DWORD)v->data.size();
      if (!lpcbData) {
        return TraceReadResultAndReturn(
            L"RegQueryValueExW", keyPath, valueName, ERROR_INVALID_PARAMETER, true, (DWORD)v->type, nullptr, 0, false);
      }
      if (!lpData) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueExW", keyPath, valueName, ERROR_SUCCESS, true, (DWORD)v->type, nullptr, needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueExW", keyPath, valueName, ERROR_MORE_DATA, true, (DWORD)v->type, nullptr, needed, false);
      }
      if (needed) {
        std::memcpy(lpData, v->data.data(), needed);
      }
      *lpcbData = needed;
      return TraceReadResultAndReturn(
          L"RegQueryValueExW", keyPath, valueName, ERROR_SUCCESS, true, (DWORD)v->type, lpData, needed, false);
    }
  }

  // Not in local store: fall back to real registry.
  HKEY real = RealHandleForFallback(hKey);
  if (auto* vk = AsVirtual(hKey)) {
    if (!vk->real) {
      // Lazily open real key for fallback.
      std::wstring sub;
      if (vk->keyPath.rfind(L"HKLM\\", 0) == 0) {
        sub = vk->keyPath.substr(5);
      }
      if (!sub.empty()) {
        HKEY opened = nullptr;
        BypassGuard guard;
        if (fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &opened) == ERROR_SUCCESS) {
          vk->real = opened;
          real = opened;
        }
      }
    }
    real = vk->real ? vk->real : nullptr;
  }
  if (!real) {
    return TraceReadResultAndReturn(
        L"RegQueryValueExW", keyPath, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
  }
  BypassGuard guard;
  DWORD typeLocal = 0;
  LPDWORD typeOut = lpType ? lpType : &typeLocal;
  LONG rc = fpRegQueryValueExW(real, lpValueName, lpReserved, typeOut, lpData, lpcbData);
  DWORD cb = lpcbData ? *lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
  return TraceReadResultAndReturn(
      L"RegQueryValueExW", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
}

LONG WINAPI Hook_RegDeleteValueW(HKEY hKey, LPCWSTR lpValueName) {
  if (g_bypass) {
    return fpRegDeleteValueW(hKey, lpValueName);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? std::wstring(lpValueName) : std::wstring();
  TraceApiEvent(L"RegDeleteValueW", L"delete_value", keyPath, valueName, L"-");
  if (keyPath.empty()) {
    return fpRegDeleteValueW(hKey, lpValueName);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (!g_store.DeleteValue(keyPath, valueName)) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegDeleteKeyW(HKEY hKey, LPCWSTR lpSubKey) {
  if (g_bypass) {
    return fpRegDeleteKeyW(hKey, lpSubKey);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegDeleteKeyW", L"delete_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegDeleteKeyW(hKey, lpSubKey);
  }
  if (sub.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
  full = JoinKeyPath(base, sub);
  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.DeleteKeyTree(full);
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegDeleteKeyExW(HKEY hKey, LPCWSTR lpSubKey, REGSAM samDesired, DWORD Reserved) {
  if (g_bypass) {
    return fpRegDeleteKeyExW ? fpRegDeleteKeyExW(hKey, lpSubKey, samDesired, Reserved) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegDeleteKeyExW", L"delete_key", full, L"-", L"-");
  (void)samDesired;
  (void)Reserved;
  InternalDispatchGuard internalGuard;
  return Hook_RegDeleteKeyW(hKey, lpSubKey);
}

// --- Old (non-Ex) APIs and extra operations ---

LONG WINAPI Hook_RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegOpenKeyW", L"open_key", full, L"-", L"-");
  InternalDispatchGuard internalGuard;
  return Hook_RegOpenKeyExW(hKey, lpSubKey, 0, KEY_READ, phkResult);
}

LONG WINAPI Hook_RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegOpenKeyA", L"open_key", full, L"-", L"-");
  InternalDispatchGuard internalGuard;
  return Hook_RegOpenKeyExA(hKey, lpSubKey, 0, KEY_READ, phkResult);
}

LONG WINAPI Hook_RegCreateKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegCreateKeyW", L"create_key", full, L"-", L"-");
  InternalDispatchGuard internalGuard;
  DWORD disp = 0;
  return Hook_RegCreateKeyExW(hKey, lpSubKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, phkResult, &disp);
}

LONG WINAPI Hook_RegCreateKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegCreateKeyA", L"create_key", full, L"-", L"-");
  InternalDispatchGuard internalGuard;
  DWORD disp = 0;
  return Hook_RegCreateKeyExA(hKey, lpSubKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, phkResult, &disp);
}

LONG WINAPI Hook_RegSetKeyValueW(HKEY hKey,
                                LPCWSTR lpSubKey,
                                LPCWSTR lpValueName,
                                DWORD dwType,
                                LPCVOID lpData,
                                DWORD cbData) {
  if (g_bypass) {
    return fpRegSetKeyValueW ? fpRegSetKeyValueW(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  std::wstring valueName = lpValueName ? std::wstring(lpValueName) : std::wstring();
  TraceApiEvent(L"RegSetKeyValueW",
                L"set_value",
                full,
                valueName,
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, reinterpret_cast<const BYTE*>(lpData), cbData));
  if (base.empty()) {
    return fpRegSetKeyValueW ? fpRegSetKeyValueW(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.PutKey(full);
    if (!g_store.PutValue(full, valueName, (uint32_t)dwType, lpData, cbData)) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegSetKeyValueA(HKEY hKey,
                                LPCSTR lpSubKey,
                                LPCSTR lpValueName,
                                DWORD dwType,
                                LPCVOID lpData,
                                DWORD cbData) {
  if (g_bypass) {
    return fpRegSetKeyValueA ? fpRegSetKeyValueA(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  std::wstring valueName = lpValueName ? AnsiToWide(lpValueName, -1) : std::wstring();
  auto normalized = EnsureWideStringData(dwType, reinterpret_cast<const BYTE*>(lpData), cbData);
  TraceApiEvent(L"RegSetKeyValueA",
                L"set_value",
                full,
                valueName,
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
  if (base.empty()) {
    return fpRegSetKeyValueA ? fpRegSetKeyValueA(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.PutKey(full);
    if (!g_store.PutValue(full, valueName, (uint32_t)dwType,
                          normalized.empty() ? nullptr : normalized.data(), (uint32_t)normalized.size())) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegEnumValueW(HKEY hKey,
                               DWORD dwIndex,
                               LPWSTR lpValueName,
                               LPDWORD lpcchValueName,
                               LPDWORD lpReserved,
                               LPDWORD lpType,
                               LPBYTE lpData,
                               LPDWORD lpcbData) {
  if (g_bypass) {
    return fpRegEnumValueW(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, lpType, lpData, lpcbData);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegEnumValueW", L"enum_value", keyPath, L"index", std::to_wstring(dwIndex));
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegEnumValueW(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, typeOut, lpData, lpcbData);
    std::wstring outName;
    if (rc == ERROR_SUCCESS && lpValueName && lpcchValueName) {
      outName.assign(lpValueName, lpValueName + *lpcchValueName);
    }
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueW", keyPath, dwIndex, outName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }
  if (lpReserved) {
    *lpReserved = 0;
  }

  HKEY real = RealHandleForFallback(hKey);
  auto merged = GetMergedValueNames(keyPath, real);
  if (dwIndex >= merged.names.size()) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueW", keyPath, dwIndex, L"", ERROR_NO_MORE_ITEMS, false, REG_NONE, nullptr, 0, false);
  }
  const std::wstring& name = merged.names[dwIndex];
  if (!lpcchValueName) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueW", keyPath, dwIndex, name, ERROR_INVALID_PARAMETER, false, REG_NONE, nullptr, 0, false);
  }
  DWORD neededName = (DWORD)name.size();
  if (!lpValueName) {
    *lpcchValueName = neededName;
  } else {
    if (*lpcchValueName <= neededName) {
      *lpcchValueName = neededName + 1;
      return TraceEnumReadResultAndReturn(
          L"RegEnumValueW", keyPath, dwIndex, name, ERROR_MORE_DATA, false, REG_NONE, nullptr, 0, false);
    }
    std::memcpy(lpValueName, name.c_str(), (neededName + 1) * sizeof(wchar_t));
    *lpcchValueName = neededName;
  }

  // Prefer local value if present.
  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(keyPath, name);
    if (v.has_value() && !v->isDeleted) {
      if (lpType) {
        *lpType = (DWORD)v->type;
      }
      if (!lpcbData) {
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueW", keyPath, dwIndex, name, ERROR_INVALID_PARAMETER, true, (DWORD)v->type, nullptr, 0, false);
      }
      DWORD needed = (DWORD)v->data.size();
      if (!lpData) {
        *lpcbData = needed;
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueW", keyPath, dwIndex, name, ERROR_SUCCESS, true, (DWORD)v->type, nullptr, needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueW", keyPath, dwIndex, name, ERROR_MORE_DATA, true, (DWORD)v->type, nullptr, needed, false);
      }
      if (needed) {
        std::memcpy(lpData, v->data.data(), needed);
      }
      *lpcbData = needed;
      return TraceEnumReadResultAndReturn(
          L"RegEnumValueW", keyPath, dwIndex, name, ERROR_SUCCESS, true, (DWORD)v->type, lpData, needed, false);
    }
  }

  // Otherwise return real data for this named value.
  if (!real) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueW", keyPath, dwIndex, name, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
  }
  BypassGuard guard;
  DWORD typeLocal = 0;
  LPDWORD typeOut = lpType ? lpType : &typeLocal;
  LONG rc = fpRegQueryValueExW(real, name.c_str(), nullptr, typeOut, lpData, lpcbData);
  DWORD cb = lpcbData ? *lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
  return TraceEnumReadResultAndReturn(
      L"RegEnumValueW", keyPath, dwIndex, name, rc, true, *typeOut, outData, cb, lpData == nullptr);
}

LONG WINAPI Hook_RegEnumValueA(HKEY hKey,
                               DWORD dwIndex,
                               LPSTR lpValueName,
                               LPDWORD lpcchValueName,
                               LPDWORD lpReserved,
                               LPDWORD lpType,
                               LPBYTE lpData,
                               LPDWORD lpcbData) {
  if (g_bypass) {
    return fpRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, lpType, lpData, lpcbData);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegEnumValueA", L"enum_value", keyPath, L"index", std::to_wstring(dwIndex));
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, typeOut, lpData, lpcbData);
    std::wstring outName;
    if (rc == ERROR_SUCCESS && lpValueName && lpcchValueName) {
      outName = AnsiToWide(lpValueName, (int)(*lpcchValueName));
    }
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueA", keyPath, dwIndex, outName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }
  if (lpReserved) {
    *lpReserved = 0;
  }

  HKEY real = RealHandleForFallback(hKey);
  auto merged = GetMergedValueNames(keyPath, real);
  if (dwIndex >= merged.names.size()) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueA", keyPath, dwIndex, L"", ERROR_NO_MORE_ITEMS, false, REG_NONE, nullptr, 0, false);
  }
  const std::wstring& nameW = merged.names[dwIndex];
  std::vector<uint8_t> nameBytes = WideToAnsiBytesForQuery(REG_SZ, std::vector<uint8_t>((const uint8_t*)nameW.c_str(),
                                                                                       (const uint8_t*)nameW.c_str() +
                                                                                           (nameW.size() + 1) * sizeof(wchar_t)));
  // nameBytes is ANSI+NUL
  if (!lpcchValueName) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_INVALID_PARAMETER, false, REG_NONE, nullptr, 0, false);
  }
  DWORD neededName = (DWORD)strlen(reinterpret_cast<const char*>(nameBytes.data()));
  if (!lpValueName) {
    *lpcchValueName = neededName;
  } else {
    if (*lpcchValueName <= neededName) {
      *lpcchValueName = neededName + 1;
      return TraceEnumReadResultAndReturn(
          L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_MORE_DATA, false, REG_NONE, nullptr, 0, false);
    }
    std::memcpy(lpValueName, nameBytes.data(), neededName + 1);
    *lpcchValueName = neededName;
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(keyPath, nameW);
    if (v.has_value() && !v->isDeleted) {
      DWORD type = (DWORD)v->type;
      if (lpType) {
        *lpType = type;
      }
      if (!lpcbData) {
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_INVALID_PARAMETER, true, type, nullptr, 0, false);
      }
      auto outBytes = WideToAnsiBytesForQuery(type, v->data);
      DWORD needed = (DWORD)outBytes.size();
      if (!lpData) {
        *lpcbData = needed;
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_SUCCESS, true, type, nullptr, needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceEnumReadResultAndReturn(
            L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_MORE_DATA, true, type, nullptr, needed, false);
      }
      if (needed) {
        std::memcpy(lpData, outBytes.data(), needed);
      }
      *lpcbData = needed;
      return TraceEnumReadResultAndReturn(
          L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_SUCCESS, true, type, lpData, needed, false);
    }
  }

  if (!real) {
    return TraceEnumReadResultAndReturn(
        L"RegEnumValueA", keyPath, dwIndex, nameW, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
  }
  BypassGuard guard;
  DWORD typeLocal = 0;
  LPDWORD typeOut = lpType ? lpType : &typeLocal;
  LONG rc = fpRegQueryValueExA(real, reinterpret_cast<const char*>(nameBytes.data()), nullptr, typeOut, lpData, lpcbData);
  DWORD cb = lpcbData ? *lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
  return TraceEnumReadResultAndReturn(
      L"RegEnumValueA", keyPath, dwIndex, nameW, rc, true, *typeOut, outData, cb, lpData == nullptr);
}

LONG WINAPI Hook_RegEnumKeyExW(HKEY hKey,
                               DWORD dwIndex,
                               LPWSTR lpName,
                               LPDWORD lpcchName,
                               LPDWORD lpReserved,
                               LPWSTR lpClass,
                               LPDWORD lpcchClass,
                               PFILETIME lpftLastWriteTime) {
  if (g_bypass) {
    return fpRegEnumKeyExW(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegEnumKeyExW", L"enum_key", keyPath, L"index", std::to_wstring(dwIndex));
  if (keyPath.empty()) {
    return fpRegEnumKeyExW(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
  }
  if (lpReserved) {
    *lpReserved = 0;
  }
  if (lpClass && lpcchClass && *lpcchClass) {
    lpClass[0] = 0;
    *lpcchClass = 0;
  }
  if (lpftLastWriteTime) {
    GetSystemTimeAsFileTime(lpftLastWriteTime);
  }

  HKEY real = RealHandleForFallback(hKey);
  auto merged = GetMergedSubKeyNames(keyPath, real);
  if (dwIndex >= merged.size()) {
    return ERROR_NO_MORE_ITEMS;
  }
  const std::wstring& nm = merged[dwIndex];
  if (!lpcchName) {
    return ERROR_INVALID_PARAMETER;
  }
  DWORD needed = (DWORD)nm.size();
  if (!lpName) {
    *lpcchName = needed;
    return ERROR_SUCCESS;
  }
  if (*lpcchName <= needed) {
    *lpcchName = needed + 1;
    return ERROR_MORE_DATA;
  }
  std::memcpy(lpName, nm.c_str(), (needed + 1) * sizeof(wchar_t));
  *lpcchName = needed;
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegEnumKeyExA(HKEY hKey,
                               DWORD dwIndex,
                               LPSTR lpName,
                               LPDWORD lpcchName,
                               LPDWORD lpReserved,
                               LPSTR lpClass,
                               LPDWORD lpcchClass,
                               PFILETIME lpftLastWriteTime) {
  if (g_bypass) {
    return fpRegEnumKeyExA(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegEnumKeyExA", L"enum_key", keyPath, L"index", std::to_wstring(dwIndex));
  if (keyPath.empty()) {
    return fpRegEnumKeyExA(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
  }
  if (lpReserved) {
    *lpReserved = 0;
  }
  if (lpClass && lpcchClass && *lpcchClass) {
    lpClass[0] = 0;
    *lpcchClass = 0;
  }
  if (lpftLastWriteTime) {
    GetSystemTimeAsFileTime(lpftLastWriteTime);
  }

  HKEY real = RealHandleForFallback(hKey);
  auto merged = GetMergedSubKeyNames(keyPath, real);
  if (dwIndex >= merged.size()) {
    return ERROR_NO_MORE_ITEMS;
  }
  const std::wstring& nmW = merged[dwIndex];
  std::vector<uint8_t> nmBytes = WideToAnsiBytesForQuery(REG_SZ, std::vector<uint8_t>((const uint8_t*)nmW.c_str(),
                                                                                    (const uint8_t*)nmW.c_str() +
                                                                                        (nmW.size() + 1) * sizeof(wchar_t)));
  if (!lpcchName) {
    return ERROR_INVALID_PARAMETER;
  }
  DWORD needed = (DWORD)strlen(reinterpret_cast<const char*>(nmBytes.data()));
  if (!lpName) {
    *lpcchName = needed;
    return ERROR_SUCCESS;
  }
  if (*lpcchName <= needed) {
    *lpcchName = needed + 1;
    return ERROR_MORE_DATA;
  }
  std::memcpy(lpName, nmBytes.data(), needed + 1);
  *lpcchName = needed;
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegEnumKeyW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, DWORD cchName) {
  TraceApiEvent(L"RegEnumKeyW", L"enum_key", KeyPathFromHandle(hKey), L"index", std::to_wstring(dwIndex));
  InternalDispatchGuard internalGuard;
  DWORD len = cchName;
  return Hook_RegEnumKeyExW(hKey, dwIndex, lpName, &len, nullptr, nullptr, nullptr, nullptr);
}

LONG WINAPI Hook_RegEnumKeyA(HKEY hKey, DWORD dwIndex, LPSTR lpName, DWORD cchName) {
  TraceApiEvent(L"RegEnumKeyA", L"enum_key", KeyPathFromHandle(hKey), L"index", std::to_wstring(dwIndex));
  InternalDispatchGuard internalGuard;
  DWORD len = cchName;
  return Hook_RegEnumKeyExA(hKey, dwIndex, lpName, &len, nullptr, nullptr, nullptr, nullptr);
}

LONG WINAPI Hook_RegQueryInfoKeyW(HKEY hKey,
                                  LPWSTR lpClass,
                                  LPDWORD lpcchClass,
                                  LPDWORD lpReserved,
                                  LPDWORD lpcSubKeys,
                                  LPDWORD lpcbMaxSubKeyLen,
                                  LPDWORD lpcbMaxClassLen,
                                  LPDWORD lpcValues,
                                  LPDWORD lpcbMaxValueNameLen,
                                  LPDWORD lpcbMaxValueLen,
                                  LPDWORD lpcbSecurityDescriptor,
                                  PFILETIME lpftLastWriteTime) {
  if (g_bypass) {
    return fpRegQueryInfoKeyW(hKey,
                              lpClass,
                              lpcchClass,
                              lpReserved,
                              lpcSubKeys,
                              lpcbMaxSubKeyLen,
                              lpcbMaxClassLen,
                              lpcValues,
                              lpcbMaxValueNameLen,
                              lpcbMaxValueLen,
                              lpcbSecurityDescriptor,
                              lpftLastWriteTime);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegQueryInfoKeyW", L"query_info", keyPath, L"-", L"-");
  if (keyPath.empty()) {
    return fpRegQueryInfoKeyW(hKey,
                              lpClass,
                              lpcchClass,
                              lpReserved,
                              lpcSubKeys,
                              lpcbMaxSubKeyLen,
                              lpcbMaxClassLen,
                              lpcValues,
                              lpcbMaxValueNameLen,
                              lpcbMaxValueLen,
                              lpcbSecurityDescriptor,
                              lpftLastWriteTime);
  }
  if (lpReserved) {
    *lpReserved = 0;
  }
  if (lpClass && lpcchClass && *lpcchClass) {
    lpClass[0] = 0;
    *lpcchClass = 0;
  }
  if (lpcbMaxClassLen) {
    *lpcbMaxClassLen = 0;
  }
  if (lpcbSecurityDescriptor) {
    *lpcbSecurityDescriptor = 0;
  }
  if (lpftLastWriteTime) {
    GetSystemTimeAsFileTime(lpftLastWriteTime);
  }

  HKEY real = RealHandleForFallback(hKey);
  auto subkeys = GetMergedSubKeyNames(keyPath, real);
  auto values = GetMergedValueNames(keyPath, real);

  if (lpcSubKeys) {
    *lpcSubKeys = (DWORD)subkeys.size();
  }
  if (lpcValues) {
    *lpcValues = (DWORD)values.names.size();
  }
  if (lpcbMaxSubKeyLen) {
    DWORD mx = 0;
    for (const auto& s : subkeys) {
      mx = std::max<DWORD>(mx, (DWORD)s.size());
    }
    *lpcbMaxSubKeyLen = mx;
  }
  if (lpcbMaxValueNameLen) {
    DWORD mx = 0;
    for (const auto& s : values.names) {
      mx = std::max<DWORD>(mx, (DWORD)s.size());
    }
    *lpcbMaxValueNameLen = mx;
  }
  if (lpcbMaxValueLen) {
    DWORD mx = 0;
    EnsureStoreOpen();
    {
      std::lock_guard<std::mutex> lock(g_storeMutex);
      for (const auto& r : g_store.ListValues(keyPath)) {
        if (!r.isDeleted) {
          mx = std::max<DWORD>(mx, (DWORD)r.data.size());
        }
      }
    }
    *lpcbMaxValueLen = mx;
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegQueryInfoKeyA(HKEY hKey,
                                  LPSTR lpClass,
                                  LPDWORD lpcchClass,
                                  LPDWORD lpReserved,
                                  LPDWORD lpcSubKeys,
                                  LPDWORD lpcbMaxSubKeyLen,
                                  LPDWORD lpcbMaxClassLen,
                                  LPDWORD lpcValues,
                                  LPDWORD lpcbMaxValueNameLen,
                                  LPDWORD lpcbMaxValueLen,
                                  LPDWORD lpcbSecurityDescriptor,
                                  PFILETIME lpftLastWriteTime) {
  if (g_bypass) {
    return fpRegQueryInfoKeyA(hKey,
                              lpClass,
                              lpcchClass,
                              lpReserved,
                              lpcSubKeys,
                              lpcbMaxSubKeyLen,
                              lpcbMaxClassLen,
                              lpcValues,
                              lpcbMaxValueNameLen,
                              lpcbMaxValueLen,
                              lpcbSecurityDescriptor,
                              lpftLastWriteTime);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  TraceApiEvent(L"RegQueryInfoKeyA", L"query_info", keyPath, L"-", L"-");
  if (keyPath.empty()) {
    return fpRegQueryInfoKeyA(hKey,
                              lpClass,
                              lpcchClass,
                              lpReserved,
                              lpcSubKeys,
                              lpcbMaxSubKeyLen,
                              lpcbMaxClassLen,
                              lpcValues,
                              lpcbMaxValueNameLen,
                              lpcbMaxValueLen,
                              lpcbSecurityDescriptor,
                              lpftLastWriteTime);
  }
  // For ANSI variant, just call W-hook (we don't return class anyway).
  InternalDispatchGuard internalGuard;
  return Hook_RegQueryInfoKeyW(hKey,
                               nullptr,
                               nullptr,
                               lpReserved,
                               lpcSubKeys,
                               lpcbMaxSubKeyLen,
                               lpcbMaxClassLen,
                               lpcValues,
                               lpcbMaxValueNameLen,
                               lpcbMaxValueLen,
                               lpcbSecurityDescriptor,
                               lpftLastWriteTime);
}

LONG WINAPI Hook_RegSetValueW(HKEY hKey, LPCWSTR lpSubKey, DWORD dwType, LPCWSTR lpData, DWORD cbData) {
  if (g_bypass) {
    return fpRegSetValueW(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegSetValueW",
                L"set_value",
                full,
                L"(Default)",
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, reinterpret_cast<const BYTE*>(lpData), cbData));
  if (base.empty()) {
    return fpRegSetValueW(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring valueName; // default

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.PutKey(full);
    if (!g_store.PutValue(full, valueName, (uint32_t)dwType, lpData, cbData)) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegSetValueA(HKEY hKey, LPCSTR lpSubKey, DWORD dwType, LPCSTR lpData, DWORD cbData) {
  if (g_bypass) {
    return fpRegSetValueA(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  auto normalized = EnsureWideStringData(dwType, reinterpret_cast<const BYTE*>(lpData), cbData);
  TraceApiEvent(L"RegSetValueA",
                L"set_value",
                full,
                L"(Default)",
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
  if (base.empty()) {
    return fpRegSetValueA(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring valueName;

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.PutKey(full);
    if (!g_store.PutValue(full, valueName, (uint32_t)dwType,
                          normalized.empty() ? nullptr : normalized.data(), (uint32_t)normalized.size())) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegQueryValueW(HKEY hKey, LPCWSTR lpSubKey, LPWSTR lpData, PLONG lpcbData) {
  if (g_bypass) {
    return fpRegQueryValueW(hKey, lpSubKey, lpData, lpcbData);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring sub = lpSubKey ? CanonicalizeSubKey(lpSubKey) : L"";
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  if (base.empty()) {
    LONG rc = fpRegQueryValueW(hKey, lpSubKey, lpData, lpcbData);
    DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueW", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
  }
  if (!lpcbData) {
    return TraceReadResultAndReturn(
        L"RegQueryValueW", full, L"(Default)", ERROR_INVALID_PARAMETER, true, REG_SZ, nullptr, 0, false);
  }
  full = sub.empty() ? base : JoinKeyPath(base, sub);
  std::wstring valueName;

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(full, valueName);
    if (v.has_value()) {
      if (v->isDeleted) {
        return TraceReadResultAndReturn(
            L"RegQueryValueW", full, L"(Default)", ERROR_FILE_NOT_FOUND, true, REG_SZ, nullptr, 0, false);
      }
      // Treat as string.
      LONG needed = (LONG)v->data.size();
      if (!lpData) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueW", full, L"(Default)", ERROR_SUCCESS, true, REG_SZ, nullptr, (DWORD)needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueW", full, L"(Default)", ERROR_MORE_DATA, true, REG_SZ, nullptr, (DWORD)needed, false);
      }
      if (needed) {
        std::memcpy(lpData, v->data.data(), (size_t)needed);
      }
      return TraceReadResultAndReturn(L"RegQueryValueW",
                                      full,
                                      L"(Default)",
                                      ERROR_SUCCESS,
                                      true,
                                      REG_SZ,
                                      reinterpret_cast<const BYTE*>(lpData),
                                      (DWORD)needed,
                                      false);
    }
  }

  HKEY realParent = RealHandleForFallback(hKey);
  if (!realParent) {
    // absolute fallback
    if (full.rfind(L"HKLM\\", 0) == 0) {
      HKEY opened = nullptr;
      BypassGuard guard;
      if (fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, full.substr(5).c_str(), 0, KEY_READ, &opened) == ERROR_SUCCESS) {
        realParent = opened;
        LONG rc = fpRegQueryValueW(realParent, nullptr, lpData, lpcbData);
        fpRegCloseKey(realParent);
        DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
        const BYTE* outData =
            (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
        return TraceReadResultAndReturn(
            L"RegQueryValueW", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
      }
    }
    return TraceReadResultAndReturn(
        L"RegQueryValueW", full, L"(Default)", ERROR_FILE_NOT_FOUND, true, REG_SZ, nullptr, 0, false);
  }
  BypassGuard guard;
  LONG rc = fpRegQueryValueW(realParent, lpSubKey, lpData, lpcbData);
  DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
  return TraceReadResultAndReturn(
      L"RegQueryValueW", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
}

LONG WINAPI Hook_RegQueryValueA(HKEY hKey, LPCSTR lpSubKey, LPSTR lpData, PLONG lpcbData) {
  if (g_bypass) {
    return fpRegQueryValueA(hKey, lpSubKey, lpData, lpcbData);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  if (base.empty()) {
    LONG rc = fpRegQueryValueA(hKey, lpSubKey, lpData, lpcbData);
    DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueA", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
  }
  if (!lpcbData) {
    return TraceReadResultAndReturn(
        L"RegQueryValueA", full, L"(Default)", ERROR_INVALID_PARAMETER, true, REG_SZ, nullptr, 0, false);
  }
  full = subW.empty() ? base : JoinKeyPath(base, subW);
  std::wstring valueName;

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(full, valueName);
    if (v.has_value()) {
      if (v->isDeleted) {
        return TraceReadResultAndReturn(
            L"RegQueryValueA", full, L"(Default)", ERROR_FILE_NOT_FOUND, true, REG_SZ, nullptr, 0, false);
      }
      auto outBytes = WideToAnsiBytesForQuery(REG_SZ, v->data);
      LONG needed = (LONG)outBytes.size();
      if (!lpData) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueA", full, L"(Default)", ERROR_SUCCESS, true, REG_SZ, nullptr, (DWORD)needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueA", full, L"(Default)", ERROR_MORE_DATA, true, REG_SZ, nullptr, (DWORD)needed, false);
      }
      if (needed) {
        std::memcpy(lpData, outBytes.data(), (size_t)needed);
      }
      return TraceReadResultAndReturn(L"RegQueryValueA",
                                      full,
                                      L"(Default)",
                                      ERROR_SUCCESS,
                                      true,
                                      REG_SZ,
                                      reinterpret_cast<const BYTE*>(lpData),
                                      (DWORD)needed,
                                      false);
    }
  }

  HKEY realParent = RealHandleForFallback(hKey);
  if (!realParent) {
    if (full.rfind(L"HKLM\\", 0) == 0) {
      HKEY opened = nullptr;
      BypassGuard guard;
      if (fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, full.substr(5).c_str(), 0, KEY_READ, &opened) == ERROR_SUCCESS) {
        realParent = opened;
        LONG rc = fpRegQueryValueA(realParent, lpSubKey, lpData, lpcbData);
        fpRegCloseKey(realParent);
        DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
        const BYTE* outData =
            (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
        return TraceReadResultAndReturn(
            L"RegQueryValueA", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
      }
    }
    return TraceReadResultAndReturn(
        L"RegQueryValueA", full, L"(Default)", ERROR_FILE_NOT_FOUND, true, REG_SZ, nullptr, 0, false);
  }
  BypassGuard guard;
  LONG rc = fpRegQueryValueA(realParent, lpSubKey, lpData, lpcbData);
  DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
  return TraceReadResultAndReturn(
      L"RegQueryValueA", full, L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
}

// --- ANSI hooks (Reg*ExA) ---

LONG WINAPI Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
  if (g_bypass) {
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegOpenKeyExA", L"open_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      *phkResult = nullptr;
      return ERROR_FILE_NOT_FOUND;
    }
    if (g_store.KeyExistsLocally(full)) {
      *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
      return ERROR_SUCCESS;
    }
  }

  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  {
    BypassGuard guard;
    LONG rc = ERROR_FILE_NOT_FOUND;
    if (realParent) {
      rc = fpRegOpenKeyExA(realParent, lpSubKey, ulOptions, samDesired, &realOut);
    } else {
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        rc = fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, absSub.c_str(), 0, samDesired, &realOut);
      }
    }
    if (rc != ERROR_SUCCESS) {
      *phkResult = nullptr;
      return rc;
    }
  }
  *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, realOut));
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegCreateKeyExA(HKEY hKey,
                                LPCSTR lpSubKey,
                                DWORD Reserved,
                                LPSTR lpClass,
                                DWORD dwOptions,
                                REGSAM samDesired,
                                const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                PHKEY phkResult,
                                LPDWORD lpdwDisposition) {
  if (g_bypass) {
    return fpRegCreateKeyExA(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegCreateKeyExA", L"create_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegCreateKeyExA(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.PutKey(full);
  }

  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  {
    BypassGuard guard;
    if (realParent) {
      fpRegOpenKeyExA(realParent, lpSubKey, 0, KEY_READ | (samDesired & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)), &realOut);
    } else {
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, absSub.c_str(), 0, KEY_READ, &realOut);
      }
    }
  }

  *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, realOut));
  if (lpdwDisposition) {
    *lpdwDisposition = REG_OPENED_EXISTING_KEY;
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegSetValueExA(HKEY hKey,
                               LPCSTR lpValueName,
                               DWORD Reserved,
                               DWORD dwType,
                               const BYTE* lpData,
                               DWORD cbData) {
  if (g_bypass) {
    return fpRegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? AnsiToWide(lpValueName, -1) : std::wstring();
  auto normalized = EnsureWideStringData(dwType, lpData, cbData);
  TraceApiEvent(L"RegSetValueExA",
                L"set_value",
                keyPath,
                valueName,
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
  if (keyPath.empty()) {
    return fpRegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(keyPath)) {
      g_store.PutKey(keyPath);
    }
    if (!g_store.PutValue(keyPath, valueName, (uint32_t)dwType,
                          normalized.empty() ? nullptr : normalized.data(), (uint32_t)normalized.size())) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegQueryValueExA(HKEY hKey,
                                 LPCSTR lpValueName,
                                 LPDWORD lpReserved,
                                 LPDWORD lpType,
                                 LPBYTE lpData,
                                 LPDWORD lpcbData) {
  if (g_bypass) {
    return fpRegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? AnsiToWide(lpValueName, -1) : std::wstring();
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegQueryValueExA(hKey, lpValueName, lpReserved, typeOut, lpData, lpcbData);
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueExA", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(keyPath, valueName);
    if (v.has_value()) {
      if (v->isDeleted) {
        return TraceReadResultAndReturn(
            L"RegQueryValueExA", keyPath, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
      }
      if (!lpcbData) {
        return TraceReadResultAndReturn(
            L"RegQueryValueExA", keyPath, valueName, ERROR_INVALID_PARAMETER, true, (DWORD)v->type, nullptr, 0, false);
      }
      DWORD type = (DWORD)v->type;
      if (lpType) {
        *lpType = type;
      }
      auto outBytes = WideToAnsiBytesForQuery(type, v->data);
      DWORD needed = (DWORD)outBytes.size();
      if (!lpData) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueExA", keyPath, valueName, ERROR_SUCCESS, true, type, nullptr, needed, true);
      }
      if (*lpcbData < needed) {
        *lpcbData = needed;
        return TraceReadResultAndReturn(
            L"RegQueryValueExA", keyPath, valueName, ERROR_MORE_DATA, true, type, nullptr, needed, false);
      }
      if (needed) {
        std::memcpy(lpData, outBytes.data(), needed);
      }
      *lpcbData = needed;
      return TraceReadResultAndReturn(
          L"RegQueryValueExA", keyPath, valueName, ERROR_SUCCESS, true, type, lpData, needed, false);
    }
  }

  HKEY real = RealHandleForFallback(hKey);
  if (auto* vk = AsVirtual(hKey)) {
    if (!vk->real) {
      std::wstring sub;
      if (vk->keyPath.rfind(L"HKLM\\", 0) == 0) {
        sub = vk->keyPath.substr(5);
      }
      if (!sub.empty()) {
        HKEY opened = nullptr;
        BypassGuard guard;
        if (fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &opened) == ERROR_SUCCESS) {
          vk->real = opened;
          real = opened;
        }
      }
    }
    real = vk->real ? vk->real : nullptr;
  }
  if (!real) {
    return TraceReadResultAndReturn(
        L"RegQueryValueExA", keyPath, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
  }
  BypassGuard guard;
  DWORD typeLocal = 0;
  LPDWORD typeOut = lpType ? lpType : &typeLocal;
  LONG rc = fpRegQueryValueExA(real, lpValueName, lpReserved, typeOut, lpData, lpcbData);
  DWORD cb = lpcbData ? *lpcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
  return TraceReadResultAndReturn(
      L"RegQueryValueExA", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
}

LONG WINAPI Hook_RegDeleteValueA(HKEY hKey, LPCSTR lpValueName) {
  if (g_bypass) {
    return fpRegDeleteValueA(hKey, lpValueName);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  std::wstring valueName = lpValueName ? AnsiToWide(lpValueName, -1) : std::wstring();
  TraceApiEvent(L"RegDeleteValueA", L"delete_value", keyPath, valueName, L"-");
  if (keyPath.empty()) {
    return fpRegDeleteValueA(hKey, lpValueName);
  }
  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (!g_store.DeleteValue(keyPath, valueName)) {
      return ERROR_WRITE_FAULT;
    }
  }
  return ERROR_SUCCESS;
}

LONG WINAPI Hook_RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey) {
  if (g_bypass) {
    return fpRegDeleteKeyA(hKey, lpSubKey);
  }
  std::wstring base = KeyPathFromHandle(hKey);
  std::wstring subW = lpSubKey ? CanonicalizeSubKey(AnsiToWide(lpSubKey, -1)) : L"";
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegDeleteKeyA", L"delete_key", full, L"-", L"-");
  if (base.empty()) {
    return fpRegDeleteKeyA(hKey, lpSubKey);
  }
  if (subW.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
  full = JoinKeyPath(base, subW);
  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.DeleteKeyTree(full);
  }
  return ERROR_SUCCESS;
}

} // namespace

template <typename TDetour, typename TOriginal>
static bool CreateHookApiTyped(LPCWSTR moduleName, LPCSTR procName, TDetour detour, TOriginal* original) {
  return MH_CreateHookApi(
             moduleName,
             procName,
             reinterpret_cast<LPVOID>(detour),
             reinterpret_cast<LPVOID*>(original)) ==
         MH_OK;
}

bool InstallRegistryHooks() {
  if (MH_Initialize() != MH_OK) {
    return false;
  }

  auto ok = true;
  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyExW", &Hook_RegOpenKeyExW, &fpRegOpenKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyExW", &Hook_RegCreateKeyExW, &fpRegCreateKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCloseKey", &Hook_RegCloseKey, &fpRegCloseKey);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueExW", &Hook_RegSetValueExW, &fpRegSetValueExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueExW", &Hook_RegQueryValueExW, &fpRegQueryValueExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteValueW", &Hook_RegDeleteValueW, &fpRegDeleteValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteKeyW", &Hook_RegDeleteKeyW, &fpRegDeleteKeyW);

  // Optional on older systems.
  (void)CreateHookApiTyped(L"advapi32", "RegDeleteKeyExW", &Hook_RegDeleteKeyExW, &fpRegDeleteKeyExW);

  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyExA", &Hook_RegOpenKeyExA, &fpRegOpenKeyExA);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyExA", &Hook_RegCreateKeyExA, &fpRegCreateKeyExA);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueExA", &Hook_RegSetValueExA, &fpRegSetValueExA);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueExA", &Hook_RegQueryValueExA, &fpRegQueryValueExA);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteValueA", &Hook_RegDeleteValueA, &fpRegDeleteValueA);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteKeyA", &Hook_RegDeleteKeyA, &fpRegDeleteKeyA);

  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyW", &Hook_RegOpenKeyW, &fpRegOpenKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyA", &Hook_RegOpenKeyA, &fpRegOpenKeyA);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyW", &Hook_RegCreateKeyW, &fpRegCreateKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyA", &Hook_RegCreateKeyA, &fpRegCreateKeyA);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueW", &Hook_RegQueryValueW, &fpRegQueryValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueA", &Hook_RegQueryValueA, &fpRegQueryValueA);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueW", &Hook_RegSetValueW, &fpRegSetValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueA", &Hook_RegSetValueA, &fpRegSetValueA);

  ok &= CreateHookApiTyped(L"advapi32", "RegEnumValueW", &Hook_RegEnumValueW, &fpRegEnumValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumValueA", &Hook_RegEnumValueA, &fpRegEnumValueA);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyExW", &Hook_RegEnumKeyExW, &fpRegEnumKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyExA", &Hook_RegEnumKeyExA, &fpRegEnumKeyExA);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyW", &Hook_RegEnumKeyW, &fpRegEnumKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyA", &Hook_RegEnumKeyA, &fpRegEnumKeyA);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryInfoKeyW", &Hook_RegQueryInfoKeyW, &fpRegQueryInfoKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryInfoKeyA", &Hook_RegQueryInfoKeyA, &fpRegQueryInfoKeyA);

  // Optional on older systems.
  (void)CreateHookApiTyped(L"advapi32", "RegSetKeyValueW", &Hook_RegSetKeyValueW, &fpRegSetKeyValueW);
  (void)CreateHookApiTyped(L"advapi32", "RegSetKeyValueA", &Hook_RegSetKeyValueA, &fpRegSetKeyValueA);

  if (!ok) {
    MH_Uninitialize();
    return false;
  }
  return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

void RemoveRegistryHooks() {
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
}

}
