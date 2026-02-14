// --- Hooks ---

LONG WINAPI Hook_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
  if (g_bypass) {
    return fpRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  if (!phkResult) {
    return ERROR_INVALID_PARAMETER;
  }

  std::wstring base = KeyPathFromHandle(hKey);
  if (base.empty()) {
    return fpRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  std::wstring rawSub;
  if (!TryReadWideString(lpSubKey, rawSub)) {
    *phkResult = nullptr;
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = rawSub.empty() ? L"" : CanonicalizeSubKey(rawSub);
  if (IsHKLMRoot(hKey) && sub.empty()) {
    return fpRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  }
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegOpenKeyExW", L"open_key", full, L"-", L"-");

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      *phkResult = nullptr;
      return ERROR_FILE_NOT_FOUND;
    }
  }

  HKEY realParent = RealHandleForFallback(hKey);
  HKEY realOut = nullptr;
  LONG realRc = ERROR_FILE_NOT_FOUND;
  {
    BypassGuard guard;
    if (realParent) {
      realRc = fpRegOpenKeyExW(realParent, lpSubKey, ulOptions, samDesired, &realOut);
    } else {
      std::wstring absSub;
      if (full.rfind(L"HKLM\\", 0) == 0) {
        absSub = full.substr(5);
      }
      if (!absSub.empty()) {
        realRc = fpRegOpenKeyExW(HKEY_LOCAL_MACHINE, absSub.c_str(), 0, samDesired, &realOut);
      }
    }
  }

  if (realRc == ERROR_SUCCESS && realOut) {
    RegisterRealKey(realOut, full);
    *phkResult = realOut;
    return ERROR_SUCCESS;
  }

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.KeyExistsLocally(full)) {
      *phkResult = reinterpret_cast<HKEY>(NewVirtualKey(full, nullptr));
      return ERROR_SUCCESS;
    }
  }

  *phkResult = nullptr;
  return realRc;
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
  if (base.empty()) {
    return fpRegCreateKeyExW(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  std::wstring rawSub;
  if (!TryReadWideString(lpSubKey, rawSub)) {
    *phkResult = nullptr;
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = rawSub.empty() ? L"" : CanonicalizeSubKey(rawSub);
  if (IsHKLMRoot(hKey) && sub.empty()) {
    return fpRegCreateKeyExW(
        hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
  }
  std::wstring full = base.empty() ? (sub.empty() ? L"(native)" : sub) : (sub.empty() ? base : JoinKeyPath(base, sub));
  TraceApiEvent(L"RegCreateKeyExW", L"create_key", full, L"-", L"-");

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(full)) {
      g_store.PutKey(full);
    } else {
      g_store.PutKey(full);
    }
  }

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
  UnregisterRealKey(hKey);
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
  if (keyPath.empty()) {
    return fpRegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
  }
  std::wstring valueName;
  if (!TryReadWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  TraceApiEvent(L"RegSetValueExW",
                L"set_value",
                keyPath,
                valueName,
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, lpData, cbData));

  EnsureStoreOpen();
  {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (g_store.IsKeyDeleted(keyPath)) {
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
  std::wstring valueName;
  if (keyPath.empty()) {
    DWORD typeLocal = 0;
    LPDWORD typeOut = lpType ? lpType : &typeLocal;
    LONG rc = fpRegQueryValueExW(hKey, lpValueName, lpReserved, typeOut, lpData, lpcbData);
    DWORD cb = lpcbData ? *lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? lpData : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueExW", keyPath, valueName, rc, true, *typeOut, outData, cb, lpData == nullptr);
  }
  if (!TryReadWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
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
  if (keyPath.empty()) {
    return fpRegDeleteValueW(hKey, lpValueName);
  }
  std::wstring valueName;
  if (!TryReadWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  TraceApiEvent(L"RegDeleteValueW", L"delete_value", keyPath, valueName, L"-");

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
  if (base.empty()) {
    return fpRegDeleteKeyW(hKey, lpSubKey);
  }
  std::wstring subRaw;
  if (!TryReadWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = sub.empty() ? base : JoinKeyPath(base, sub);
  TraceApiEvent(L"RegDeleteKeyW", L"delete_key", full, L"-", L"-");
  if (sub.empty()) {
    return ERROR_INVALID_PARAMETER;
  }
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
  std::wstring full = base.empty() ? L"(native)" : base;
  if (!base.empty()) {
    std::wstring subRaw;
    if (!TryReadWideString(lpSubKey, subRaw)) {
      return ERROR_INVALID_PARAMETER;
    }
    std::wstring sub = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
    full = sub.empty() ? base : JoinKeyPath(base, sub);
  }
  TraceApiEvent(L"RegDeleteKeyExW", L"delete_key", full, L"-", L"-");
  (void)samDesired;
  (void)Reserved;
  InternalDispatchGuard internalGuard;
  return Hook_RegDeleteKeyW(hKey, lpSubKey);
}
