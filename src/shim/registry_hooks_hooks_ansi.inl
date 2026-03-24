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
    BypassGuard guard;
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }

  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    *phkResult = nullptr;
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW;
  std::wstring full = base;
  subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  if (IsHKLMRoot(hKey) && subW.empty()) {
    BypassGuard guard;
    return fpRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  full = subW.empty() ? base : JoinKeyPath(base, subW);
  if (IsRegistryTraceEnabledForApi(L"RegOpenKeyExA")) {
    TraceApiEvent(L"RegOpenKeyExA", L"open_key", full, L"-", L"-");
  }

  EnsureStoreOpen();
  bool localExists = false;
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      *phkResult = nullptr;
      return ERROR_FILE_NOT_FOUND;
    }
    localExists = g_store.KeyExistsLocally(full);
  }

  if (!ShouldReadThrough()) {
    if (localExists) {
      *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
      return ERROR_SUCCESS;
    }
    *phkResult = nullptr;
    return ERROR_FILE_NOT_FOUND;
  }

  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  LONG realRc = ERROR_INVALID_HANDLE;
  {
    BypassGuard guard;
    realRc = fpRegOpenKeyExA(realParent ? realParent : hKey, lpSubKey, ulOptions, samDesired, &realOut);
  }

  if (realRc == ERROR_SUCCESS && realOut) {
    RegisterRealKey(realOut, full);
    *phkResult = realOut;
    return ERROR_SUCCESS;
  }

  if (localExists) {
    *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
    return ERROR_SUCCESS;
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
    BypassGuard guard;
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
    BypassGuard guard;
    return fpRegCreateKeyExA(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  std::wstring full = base.empty() ? (subW.empty() ? L"(native)" : subW) : (subW.empty() ? base : JoinKeyPath(base, subW));
  if (IsRegistryTraceEnabledForApi(L"RegCreateKeyExA")) {
    TraceApiEvent(L"RegCreateKeyExA", L"create_key", full, L"-", L"-");
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
    BypassGuard guard;
    return fpRegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }
  std::wstring valueName;
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  auto normalized = EnsureWideStringData(dwType, lpData, cbData);
  if (IsRegistryTraceEnabledForApi(L"RegSetValueExA")) {
    TraceApiEvent(L"RegSetValueExA",
                  L"set_value",
                  keyPath,
                  valueName,
                  FormatRegType(dwType) + L":" +
                      FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
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
  std::wstring valueName;
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = ERROR_GEN_FAILURE;
    {
      BypassGuard guard;
      rc = fpRegQueryValueExA(hKey, lpValueName, lpReserved, typeOut, lpData, lpcbData);
    }
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

  if (!ShouldReadThrough()) {
    return TraceReadResultAndReturn(
        L"RegQueryValueExA", keyPath, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
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

LSTATUS WINAPI Hook_RegGetValueA(HKEY hKey,
                                 LPCSTR lpSubKey,
                                 LPCSTR lpValue,
                                 DWORD dwFlags,
                                 LPDWORD pdwType,
                                 PVOID pvData,
                                 LPDWORD pcbData) {
  if (g_bypass) {
    return fpRegGetValueA(hKey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
  }

  std::wstring base = KeyPathFromHandle(hKey);
  if (base.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = pdwType ? pdwType : &typeLocal;
    LSTATUS rc = ERROR_GEN_FAILURE;
    {
      BypassGuard guard;
      rc = fpRegGetValueA(hKey, lpSubKey, lpValue, dwFlags, typeOut, pvData, pcbData);
    }
    DWORD cb = pcbData ? *pcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && pvData && pcbData) ? reinterpret_cast<const BYTE*>(pvData) : nullptr;
    return TraceReadResultAndReturn(L"RegGetValueA", base, L"", rc, true, *typeOut, outData, cb, pvData == nullptr);
  }

  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);

  std::wstring valueName;
  if (!TryAnsiToWideString(lpValue, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (IsRegistryTraceEnabledForApi(L"RegGetValueA")) {
    TraceApiEvent(L"RegGetValueA", L"query_value", full, valueName.empty() ? L"(Default)" : valueName, L"-");
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    auto v = g_store.GetValue(full, valueName);
    if (v.has_value()) {
      if (v->isDeleted) {
        return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
      }

      const DWORD storedType = (DWORD)v->type;
      const DWORD typeMask = (dwFlags & 0x0000FFFF);
      if (!TypeAllowedByRrfMask(storedType, typeMask)) {
        return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_UNSUPPORTED_TYPE, true, storedType, nullptr, 0, false);
      }

      if (pdwType) {
        *pdwType = storedType;
      }
      if (!pcbData) {
        return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_INVALID_PARAMETER, true, storedType, nullptr, 0, false);
      }

      const std::vector<uint8_t> outBytes = WideToAnsiBytesForQuery(storedType, v->data);
      DWORD needed = (DWORD)outBytes.size();
      if (!pvData) {
        *pcbData = needed;
        return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_SUCCESS, true, storedType, nullptr, needed, true);
      }
      if (*pcbData < needed) {
        *pcbData = needed;
        return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_MORE_DATA, true, storedType, nullptr, needed, false);
      }
      if (needed) {
        std::memcpy(pvData, outBytes.data(), needed);
      }
      *pcbData = needed;
      return TraceReadResultAndReturn(L"RegGetValueA",
                                      full,
                                      valueName,
                                      ERROR_SUCCESS,
                                      true,
                                      storedType,
                                      reinterpret_cast<const BYTE*>(pvData),
                                      needed,
                                      false);
    }
  }

  if (!ShouldReadThrough()) {
    return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, ERROR_FILE_NOT_FOUND, false, REG_NONE, nullptr, 0, false);
  }

  HKEY realParent = RealHandleForFallback(hKey);
  DWORD typeLocal = 0;
  LPDWORD typeOut = pdwType ? pdwType : &typeLocal;
  LSTATUS rc = ERROR_FILE_NOT_FOUND;
  {
    BypassGuard guard;
    if (realParent) {
      rc = fpRegGetValueA(realParent, lpSubKey, lpValue, dwFlags, typeOut, pvData, pcbData);
    } else {
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        std::vector<uint8_t> absSubBytes = WideToAnsiBytesForQuery(REG_SZ, std::vector<uint8_t>((const uint8_t*)absSub.c_str(),
                                                                                                 (const uint8_t*)absSub.c_str() +
                                                                                                     (absSub.size() + 1) * sizeof(wchar_t)));
        rc = fpRegGetValueA(HKEY_LOCAL_MACHINE, reinterpret_cast<const char*>(absSubBytes.data()), lpValue, dwFlags, typeOut, pvData, pcbData);
      }
    }
  }
  DWORD cb = pcbData ? *pcbData : 0;
  const BYTE* outData = (rc == ERROR_SUCCESS && pvData && pcbData) ? reinterpret_cast<const BYTE*>(pvData) : nullptr;
  return TraceReadResultAndReturn(L"RegGetValueA", full, valueName, rc, true, *typeOut, outData, cb, pvData == nullptr);
}

LONG WINAPI Hook_RegDeleteValueA(HKEY hKey, LPCSTR lpValueName) {
  if (g_bypass) {
    return fpRegDeleteValueA(hKey, lpValueName);
  }
  std::wstring keyPath = KeyPathFromHandle(hKey);
  if (keyPath.empty()) {
    BypassGuard guard;
    return fpRegDeleteValueA(hKey, lpValueName);
  }
  std::wstring valueName;
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (IsRegistryTraceEnabledForApi(L"RegDeleteValueA")) {
    TraceApiEvent(L"RegDeleteValueA", L"delete_value", keyPath, valueName, L"-");
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
  if (base.empty()) {
    BypassGuard guard;
    return fpRegDeleteKeyA(hKey, lpSubKey);
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);
  if (IsRegistryTraceEnabledForApi(L"RegDeleteKeyA")) {
    TraceApiEvent(L"RegDeleteKeyA", L"delete_key", full, L"-", L"-");
  }
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
