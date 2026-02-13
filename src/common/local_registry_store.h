#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace hklmwrap {

struct StoredValue {
  bool isDeleted = false;
  uint32_t type = 0;
  std::vector<uint8_t> data;
};

class LocalRegistryStore {
public:
  LocalRegistryStore();
  ~LocalRegistryStore();

  LocalRegistryStore(const LocalRegistryStore&) = delete;
  LocalRegistryStore& operator=(const LocalRegistryStore&) = delete;

  bool Open(const std::wstring& dbPath);
  void Close();

  bool PutKey(const std::wstring& keyPath);
  bool DeleteKeyTree(const std::wstring& keyPath);
  bool IsKeyDeleted(const std::wstring& keyPath);
  bool KeyExistsLocally(const std::wstring& keyPath);

  bool PutValue(const std::wstring& keyPath, const std::wstring& valueName, uint32_t type, const void* data, uint32_t dataSize);
  bool DeleteValue(const std::wstring& keyPath, const std::wstring& valueName);
  std::optional<StoredValue> GetValue(const std::wstring& keyPath, const std::wstring& valueName);

  struct ValueRow {
    std::wstring valueName;
    bool isDeleted = false;
    uint32_t type = 0;
    std::vector<uint8_t> data;
  };
  std::vector<ValueRow> ListValues(const std::wstring& keyPath);
  std::vector<std::wstring> ListImmediateSubKeys(const std::wstring& keyPath);

  struct ExportRow {
    std::wstring keyPath;
    std::wstring valueName;
    uint32_t type = 0;
    std::vector<uint8_t> data;
  };
  std::vector<ExportRow> ExportAll();

private:
  bool EnsureSchema();
  bool Exec(const char* sql);
  bool PrepareAndStep(const char* sql);

  sqlite3* db_ = nullptr;
};

}
