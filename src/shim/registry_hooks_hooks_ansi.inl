// --- ANSI hooks (Reg*ExA) ---

LONG WINAPI Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
  if (g_bypass) {
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring base = KeyPathFromHandle(hKey);
  if (base.empty()) {
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }

  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  LONG realRc = ERROR_INVALID_HANDLE;
  {
    BypassGuard guard;
    realRc = fpRegOpenKeyExA(realParent ? realParent : hKey, lpSubKey, ulOptions, samDesired, &realOut);
  }

  std::wstring subRaw;
  std::wstring subW;
  std::wstring full = base;
  if (TryAnsiToWideString(lpSubKey, subRaw)) {
    subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
    full = subW.empty() ? base : JoinKeyPath(base, subW);
  }
  TraceApiEvent(L"RegOpenKeyExA", L"open_key", full, L"-", L"-");

  if (realRc == ERROR_SUCCESS && realOut) {
    RegisterRealKey(realOut, full.empty() ? base : full);
    *phkResult = realOut;
    return ERROR_SUCCESS;
  }

  if (subW.empty() && !subRaw.empty()) {
    *phkResult = nullptr;
    return realRc;
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.KeyExistsLocally(full)) {
      *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
      return ERROR_SUCCESS;
    }
    if (g_store.IsKeyDeleted(full)) {
      *phkResult = nullptr;
      return ERROR_FILE_NOT_FOUND;
    }
  }

  *phkResult = nullptr;
  return realRc;
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
  if (base.empty()) {
    return fpRegCreateKeyExA(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    *phkResult = nullptr;
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  if (IsHKLMRoot(hKey) && subW.empty()) {
    return fpRegCreateKeyExA(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  TraceApiEvent(L"RegCreateKeyExA", L"create_key", full, L"-", L"-");

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

  if (realOut) {
    RegisterRealKey(realOut, full);
    *phkResult = realOut;
  } else {
    *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
  }
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
  if (keyPath.empty()) {
    return fpRegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }
  std::wstring valueName;
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  auto normalized = EnsureWideStringData(dwType, lpData, cbData);
  TraceApiEvent(L"RegSetValueExA",
                L"set_value",
                keyPath,
                valueName,
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
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
  std::wstring valueName;
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegQueryValueExA(hKey, lpValueName, lpReserved, typeOut, lpData, lpcbData);
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueExA", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
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
  if (keyPath.empty()) {
    return fpRegDeleteValueA(hKey, lpValueName);
  }
  std::wstring valueName;
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  TraceApiEvent(L"RegDeleteValueA", L"delete_value", keyPath, valueName, L"-");
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
  if (base.empty()) {
    return fpRegDeleteKeyA(hKey, lpSubKey);
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);
  TraceApiEvent(L"RegDeleteKeyA", L"delete_key", full, L"-", L"-");
  if (subW.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    g_store.DeleteKeyTree(full);
  }
  return ERROR_SUCCESS;
}
