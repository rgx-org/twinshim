#include "common/local_registry_store.h"
#include "test_tmp.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

using namespace hklmwrap;

namespace {

#ifndef REG_BINARY
constexpr uint32_t REG_BINARY = 3;
#endif

std::wstring MakeTempDbPath() {
  auto base = testutil::GetTestTempDir("db");
  REQUIRE_FALSE(base.empty());

  static size_t counter = 0;
  counter++;

  auto path = base / ("store-" + std::to_string(counter) + ".sqlite");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path.wstring();
}

} // namespace

TEST_CASE("LocalRegistryStore preserves embedded NUL in key/value names", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  std::wstring key = L"HKLM\\Soft";
  key.push_back(L'\0');
  key += L"Ware\\Case";

  std::wstring valueName = L"Na";
  valueName.push_back(L'\0');
  valueName += L"me";

  REQUIRE(key.size() == 19);
  REQUIRE(valueName.size() == 5);
  const std::vector<uint8_t> payload = {0x41, 0x00, 0x42, 0x00, 0x00};

  REQUIRE(store.PutValue(key, valueName, REG_BINARY, payload.data(), static_cast<uint32_t>(payload.size())));

  const auto value = store.GetValue(key, valueName);
  REQUIRE(value.has_value());
  CHECK_FALSE(value->isDeleted);
  CHECK(value->type == REG_BINARY);
  CHECK(value->data == payload);

  const auto rows = store.ListValues(key);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].valueName == valueName);
  CHECK(rows[0].data == payload);
}

TEST_CASE("LocalRegistryStore keeps keys distinct and handles tombstones", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring keyA = L"HKLM\\Software\\One";
  const std::wstring keyB = L"HKLM\\Software\\Two";
  const std::wstring value = L"X";
  const uint8_t byteA = 0xAA;
  const uint8_t byteB = 0xBB;

  REQUIRE(store.PutValue(keyA, value, REG_BINARY, &byteA, 1));
  REQUIRE(store.PutValue(keyB, value, REG_BINARY, &byteB, 1));

  auto a = store.GetValue(keyA, value);
  auto b = store.GetValue(keyB, value);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->data == std::vector<uint8_t>{byteA});
  CHECK(b->data == std::vector<uint8_t>{byteB});

  REQUIRE(store.DeleteValue(keyA, value));
  a = store.GetValue(keyA, value);
  REQUIRE(a.has_value());
  CHECK(a->isDeleted);

  REQUIRE(store.DeleteKeyTree(L"HKLM\\Software"));
  CHECK(store.IsKeyDeleted(keyA));
  CHECK(store.IsKeyDeleted(keyB));
}

TEST_CASE("LocalRegistryStore export includes keys with no values", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring baseKey = L"HKLM\\SOFTWARE\\ExampleVendor\\ExampleApp";
  const std::wstring emptyA = baseKey + L"\\EmptyA";
  const std::wstring emptyB = baseKey + L"\\EmptyB";

  REQUIRE(store.PutKey(emptyA));
  REQUIRE(store.PutKey(emptyB));

  const std::wstring valueName = L"InstallDir";
  const std::vector<uint8_t> payload = {0x41, 0x42, 0x43};
  REQUIRE(store.PutValue(baseKey, valueName, REG_BINARY, payload.data(), static_cast<uint32_t>(payload.size())));

  const auto rows = store.ExportAll();
  REQUIRE_FALSE(rows.empty());

  auto hasKeyOnly = [&](const std::wstring& key) {
    for (const auto& r : rows) {
      if (r.keyPath == key && r.isKeyOnly) {
        return true;
      }
    }
    return false;
  };
  auto hasValue = [&](const std::wstring& key, const std::wstring& name) {
    for (const auto& r : rows) {
      if (r.keyPath == key && !r.isKeyOnly && r.valueName == name) {
        return true;
      }
    }
    return false;
  };
  auto hasAnyRowForKey = [&](const std::wstring& key) {
    for (const auto& r : rows) {
      if (r.keyPath == key) {
        return true;
      }
    }
    return false;
  };

  CHECK(hasKeyOnly(baseKey));
  CHECK(hasKeyOnly(emptyA));
  CHECK(hasKeyOnly(emptyB));
  CHECK(hasValue(baseKey, valueName));

  // Creating a key/value under HKLM\SOFTWARE\... should not implicitly create/export HKLM\SOFTWARE.
  CHECK_FALSE(hasAnyRowForKey(L"HKLM"));
  CHECK_FALSE(hasAnyRowForKey(L"HKLM\\SOFTWARE"));
}
