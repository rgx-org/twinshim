#include <windows.h>

#include <string>
#include <vector>

namespace {

int FailWithCode(int code) {
  return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2 || !argv || !argv[1] || argv[1][0] == L'\0') {
    return FailWithCode(2);
  }

  const std::wstring suffix = argv[1];
  const std::wstring subKey = L"Software\\twinshim-workflow\\" + suffix;
  const std::wstring valueName = L"WorkflowValue";
  const std::wstring valueText = L"wrapped-ok";

  HKEY key = nullptr;
  LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                            subKey.c_str(),
                            0,
                            nullptr,
                            REG_OPTION_NON_VOLATILE,
                            KEY_READ | KEY_WRITE,
                            nullptr,
                            &key,
                            nullptr);
  if (rc != ERROR_SUCCESS || !key) {
    return FailWithCode(10);
  }

  const DWORD setSize = static_cast<DWORD>((valueText.size() + 1) * sizeof(wchar_t));
  rc = RegSetValueExW(key,
                      valueName.c_str(),
                      0,
                      REG_SZ,
                      reinterpret_cast<const BYTE*>(valueText.c_str()),
                      setSize);
  if (rc != ERROR_SUCCESS) {
    RegCloseKey(key);
    return FailWithCode(11);
  }

  DWORD type = 0;
  DWORD cbData = 0;
  rc = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &cbData);
  if (rc != ERROR_SUCCESS || type != REG_SZ || cbData < sizeof(wchar_t)) {
    RegCloseKey(key);
    return FailWithCode(12);
  }

  std::vector<BYTE> bytes(cbData);
  rc = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, bytes.data(), &cbData);
  if (rc != ERROR_SUCCESS || type != REG_SZ || cbData < sizeof(wchar_t)) {
    RegCloseKey(key);
    return FailWithCode(13);
  }

  const auto* textOut = reinterpret_cast<const wchar_t*>(bytes.data());
  const size_t wcharCount = cbData / sizeof(wchar_t);
  std::wstring queried(textOut, textOut + wcharCount);
  const size_t terminator = queried.find(L'\0');
  if (terminator != std::wstring::npos) {
    queried.resize(terminator);
  }

  RegCloseKey(key);

  if (queried != valueText) {
    return FailWithCode(14);
  }
  return 0;
}
