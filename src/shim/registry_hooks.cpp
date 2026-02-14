#include "shim/registry_hooks.h"
#include "shim/registry_hooks_trace.h"
#include "shim/registry_hooks_utils.h"

#include "common/local_registry_store.h"
#include "common/path_util.h"

#include <MinHook.h>

#include <cstring>
#include <cstdio>
#include <atomic>
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
std::mutex g_realKeysMutex;
std::unordered_map<HKEY, std::wstring> g_realKeys;

thread_local bool g_bypass = false;
std::atomic<bool> g_minHookInitialized{false};
std::atomic<bool> g_hooksEnabled{false};

bool ShouldInstallExtendedHooks() {
  wchar_t modeBuf[64]{};
  DWORD modeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_HOOK_MODE", modeBuf, (DWORD)(sizeof(modeBuf) / sizeof(modeBuf[0])));
  if (!modeLen || modeLen >= (sizeof(modeBuf) / sizeof(modeBuf[0]))) {
    // Default to full ANSI+W coverage to avoid mixed-callsite handle issues
    // where a virtual handle created by *W is consumed by an unhooked *A API.
    return true;
  }
  std::wstring mode(modeBuf, modeBuf + modeLen);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t ch) { return (wchar_t)std::towlower(ch); });
  if (mode == L"core" || mode == L"minimal" || mode == L"wide" || mode == L"unicode") {
    return false;
  }
  return mode == L"all" || mode == L"full" || mode == L"extended";
}

bool ShouldDisableHooks() {
  wchar_t modeBuf[64]{};
  DWORD modeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_HOOK_MODE", modeBuf, (DWORD)(sizeof(modeBuf) / sizeof(modeBuf[0])));
  if (!modeLen || modeLen >= (sizeof(modeBuf) / sizeof(modeBuf[0]))) {
    return false;
  }
  std::wstring mode(modeBuf, modeBuf + modeLen);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t ch) { return (wchar_t)std::towlower(ch); });
  return mode == L"off" || mode == L"none" || mode == L"disabled";
}

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

bool IsHKLMRoot(HKEY h) {
  return h == HKEY_LOCAL_MACHINE;
}

LocalRegistryStore g_store;
std::once_flag g_openOnce;
std::mutex g_storeMutex;

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
  {
    std::lock_guard<std::mutex> lock(g_realKeysMutex);
    auto it = g_realKeys.find(hKey);
    if (it != g_realKeys.end()) {
      return it->second;
    }
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

void RegisterRealKey(HKEY key, const std::wstring& path) {
  if (!key || key == HKEY_LOCAL_MACHINE) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_realKeysMutex);
  g_realKeys[key] = path;
}

void UnregisterRealKey(HKEY key) {
  if (!key) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_realKeysMutex);
  g_realKeys.erase(key);
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


void DeleteVirtualKey(VirtualKey* vk) {
  if (!vk) {
    return;
  }
  // Keep virtual key objects alive for the process lifetime while hooks are
  // active. Concurrent hook calls can still observe the handle value after a
  // close on another thread; deleting here can cause a use-after-free.
}

void DestroyAllVirtualKeys() {
  std::vector<VirtualKey*> toFree;
  {
    std::lock_guard<std::mutex> lock(g_virtualKeysMutex);
    toFree.reserve(g_virtualKeys.size());
    for (auto* vk : g_virtualKeys) {
      toFree.push_back(vk);
    }
    g_virtualKeys.clear();
  }

  for (auto* vk : toFree) {
    if (!vk) {
      continue;
    }
    if (vk->real) {
      BypassGuard guard;
      fpRegCloseKey(vk->real);
      vk->real = nullptr;
    }
    delete vk;
  }
}

#include "shim/registry_hooks_hooks_core.inl"


#include "shim/registry_hooks_hooks_legacy.inl"

#include "shim/registry_hooks_hooks_ansi.inl"

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
  g_hooksEnabled.store(false, std::memory_order_release);
  if (ShouldDisableHooks()) {
    g_minHookInitialized.store(false, std::memory_order_release);
    return true;
  }

  if (MH_Initialize() != MH_OK) {
    return false;
  }
  g_minHookInitialized.store(true, std::memory_order_release);

  const bool extended = ShouldInstallExtendedHooks();

  auto ok = true;
  // Core W/Unicode hooks (default): include all common handle consumers so
  // virtual HKEY handles never leak into unhooked advapi32 entry points.
  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyExW", &Hook_RegOpenKeyExW, &fpRegOpenKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyExW", &Hook_RegCreateKeyExW, &fpRegCreateKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCloseKey", &Hook_RegCloseKey, &fpRegCloseKey);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueExW", &Hook_RegSetValueExW, &fpRegSetValueExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueExW", &Hook_RegQueryValueExW, &fpRegQueryValueExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteValueW", &Hook_RegDeleteValueW, &fpRegDeleteValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegDeleteKeyW", &Hook_RegDeleteKeyW, &fpRegDeleteKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyW", &Hook_RegOpenKeyW, &fpRegOpenKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyW", &Hook_RegCreateKeyW, &fpRegCreateKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueW", &Hook_RegQueryValueW, &fpRegQueryValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegSetValueW", &Hook_RegSetValueW, &fpRegSetValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumValueW", &Hook_RegEnumValueW, &fpRegEnumValueW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyExW", &Hook_RegEnumKeyExW, &fpRegEnumKeyExW);
  ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyW", &Hook_RegEnumKeyW, &fpRegEnumKeyW);
  ok &= CreateHookApiTyped(L"advapi32", "RegQueryInfoKeyW", &Hook_RegQueryInfoKeyW, &fpRegQueryInfoKeyW);

  // Optional on older systems.
  (void)CreateHookApiTyped(L"advapi32", "RegSetKeyValueW", &Hook_RegSetKeyValueW, &fpRegSetKeyValueW);

  // Optional on older systems.
  (void)CreateHookApiTyped(L"advapi32", "RegDeleteKeyExW", &Hook_RegDeleteKeyExW, &fpRegDeleteKeyExW);

  if (extended) {
    ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyExA", &Hook_RegOpenKeyExA, &fpRegOpenKeyExA);
    ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyExA", &Hook_RegCreateKeyExA, &fpRegCreateKeyExA);
    ok &= CreateHookApiTyped(L"advapi32", "RegSetValueExA", &Hook_RegSetValueExA, &fpRegSetValueExA);
    ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueExA", &Hook_RegQueryValueExA, &fpRegQueryValueExA);
    ok &= CreateHookApiTyped(L"advapi32", "RegDeleteValueA", &Hook_RegDeleteValueA, &fpRegDeleteValueA);
    ok &= CreateHookApiTyped(L"advapi32", "RegDeleteKeyA", &Hook_RegDeleteKeyA, &fpRegDeleteKeyA);

    ok &= CreateHookApiTyped(L"advapi32", "RegOpenKeyA", &Hook_RegOpenKeyA, &fpRegOpenKeyA);
    ok &= CreateHookApiTyped(L"advapi32", "RegCreateKeyA", &Hook_RegCreateKeyA, &fpRegCreateKeyA);
    ok &= CreateHookApiTyped(L"advapi32", "RegQueryValueA", &Hook_RegQueryValueA, &fpRegQueryValueA);
    ok &= CreateHookApiTyped(L"advapi32", "RegSetValueA", &Hook_RegSetValueA, &fpRegSetValueA);

    ok &= CreateHookApiTyped(L"advapi32", "RegEnumValueA", &Hook_RegEnumValueA, &fpRegEnumValueA);
    ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyExA", &Hook_RegEnumKeyExA, &fpRegEnumKeyExA);
    ok &= CreateHookApiTyped(L"advapi32", "RegEnumKeyA", &Hook_RegEnumKeyA, &fpRegEnumKeyA);
    ok &= CreateHookApiTyped(L"advapi32", "RegQueryInfoKeyA", &Hook_RegQueryInfoKeyA, &fpRegQueryInfoKeyA);

    // Optional on older systems.
    (void)CreateHookApiTyped(L"advapi32", "RegSetKeyValueA", &Hook_RegSetKeyValueA, &fpRegSetKeyValueA);
  }

  if (!ok) {
    MH_Uninitialize();
    g_minHookInitialized.store(false, std::memory_order_release);
    return false;
  }

  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    MH_Uninitialize();
    g_minHookInitialized.store(false, std::memory_order_release);
    return false;
  }

  g_hooksEnabled.store(true, std::memory_order_release);
  return true;
}

bool AreRegistryHooksActive() {
  return g_hooksEnabled.load(std::memory_order_acquire);
}

void RemoveRegistryHooks() {
  if (g_hooksEnabled.exchange(false, std::memory_order_acq_rel)) {
    MH_DisableHook(MH_ALL_HOOKS);
  }
  if (g_minHookInitialized.exchange(false, std::memory_order_acq_rel)) {
    MH_Uninitialize();
  }
  DestroyAllVirtualKeys();
}

}
