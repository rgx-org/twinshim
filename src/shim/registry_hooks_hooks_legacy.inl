// --- Old (non-Ex) APIs and extra operations ---

LONG WINAPI Hook_RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
  TraceApiEvent(L"RegOpenKeyW", L"open_key", KeyPathFromHandle(hKey), L"-", L"-");
  InternalDispatchGuard internalGuard;
  return Hook_RegOpenKeyExW(hKey, lpSubKey, 0, KEY_READ, phkResult);
}

LONG WINAPI Hook_RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
  TraceApiEvent(L"RegOpenKeyA", L"open_key", KeyPathFromHandle(hKey), L"-", L"-");
  InternalDispatchGuard internalGuard;
  return Hook_RegOpenKeyExA(hKey, lpSubKey, 0, KEY_READ, phkResult);
}

LONG WINAPI Hook_RegCreateKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
  TraceApiEvent(L"RegCreateKeyW", L"create_key", KeyPathFromHandle(hKey), L"-", L"-");
  InternalDispatchGuard internalGuard;
  DWORD disp = 0;
  return Hook_RegCreateKeyExW(hKey, lpSubKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, phkResult, &disp);
}

LONG WINAPI Hook_RegCreateKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
  TraceApiEvent(L"RegCreateKeyA", L"create_key", KeyPathFromHandle(hKey), L"-", L"-");
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
  if (base.empty()) {
    return fpRegSetKeyValueW ? fpRegSetKeyValueW(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::wstring subRaw;
  if (!TryReadWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring valueName;
  if (!TryReadWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = sub.empty() ? base : JoinKeyPath(base, sub);
  TraceApiEvent(L"RegSetKeyValueW",
                L"set_value",
                full,
                valueName,
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, reinterpret_cast<const BYTE*>(lpData), cbData));

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
  if (base.empty()) {
    return fpRegSetKeyValueA ? fpRegSetKeyValueA(hKey, lpSubKey, lpValueName, dwType, lpData, cbData) : ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring valueName;
  if (!TryAnsiToWideString(lpValueName, valueName)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);
  auto normalized = EnsureWideStringData(dwType, reinterpret_cast<const BYTE*>(lpData), cbData);
  TraceApiEvent(L"RegSetKeyValueA",
                L"set_value",
                full,
                valueName,
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
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
      if (!TryAnsiToWideString(lpValueName, outName)) {
        outName = L"<invalid>";
      }
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
  if (base.empty()) {
    return fpRegSetValueW(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring subRaw;
  if (!TryReadWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = sub.empty() ? base : JoinKeyPath(base, sub);
  TraceApiEvent(L"RegSetValueW",
                L"set_value",
                full,
                L"(Default)",
                FormatRegType(dwType) + L":" + FormatValuePreview(dwType, reinterpret_cast<const BYTE*>(lpData), cbData));
  std::wstring valueName;

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
  if (base.empty()) {
    return fpRegSetValueA(hKey, lpSubKey, dwType, lpData, cbData);
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);
  auto normalized = EnsureWideStringData(dwType, reinterpret_cast<const BYTE*>(lpData), cbData);
  TraceApiEvent(L"RegSetValueA",
                L"set_value",
                full,
                L"(Default)",
                FormatRegType(dwType) + L":" +
                    FormatValuePreview(dwType, normalized.empty() ? nullptr : normalized.data(), (DWORD)normalized.size()));
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
  if (base.empty()) {
    LONG rc = fpRegQueryValueW(hKey, lpSubKey, lpData, lpcbData);
    DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueW", L"(native)", L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
  }
  std::wstring subRaw;
  if (!TryReadWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring sub = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = sub.empty() ? base : JoinKeyPath(base, sub);
  if (!lpcbData) {
    return TraceReadResultAndReturn(
        L"RegQueryValueW", full, L"(Default)", ERROR_INVALID_PARAMETER, true, REG_SZ, nullptr, 0, false);
  }
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
  if (base.empty()) {
    LONG rc = fpRegQueryValueA(hKey, lpSubKey, lpData, lpcbData);
    DWORD cb = lpcbData ? (DWORD)*lpcbData : 0;
    const BYTE* outData = (rc == ERROR_SUCCESS && lpData && lpcbData) ? reinterpret_cast<const BYTE*>(lpData) : nullptr;
    return TraceReadResultAndReturn(
        L"RegQueryValueA", L"(native)", L"(Default)", rc, true, REG_SZ, outData, cb, lpData == nullptr);
  }
  std::wstring subRaw;
  if (!TryAnsiToWideString(lpSubKey, subRaw)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::wstring subW = subRaw.empty() ? L"" : CanonicalizeSubKey(subRaw);
  std::wstring full = subW.empty() ? base : JoinKeyPath(base, subW);
  if (!lpcbData) {
    return TraceReadResultAndReturn(
        L"RegQueryValueA", full, L"(Default)", ERROR_INVALID_PARAMETER, true, REG_SZ, nullptr, 0, false);
  }
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
