#include "common/local_registry_store.h"
#include "common/utf8.h"
#include "test_tmp.h"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace twinshim;

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

TEST_CASE("LocalRegistryStore key/value lookups are case-insensitive", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring keyImport = L"HKLM\\Software\\ExampleVendor\\ExampleApp";
  const std::wstring valueImport = L"InstallDir";
  const std::vector<uint8_t> payload = {0x10, 0x20, 0x30};

  REQUIRE(store.PutValue(keyImport, valueImport, REG_BINARY, payload.data(), static_cast<uint32_t>(payload.size())));

  // Different casing should still find the same logical key/value.
  const std::wstring keyQuery = L"hklm\\SOFTWARE\\examplevendor\\EXAMPLEAPP";
  const std::wstring valueQuery = L"installdir";

  CHECK(store.KeyExistsLocally(keyQuery));
  {
    const auto v = store.GetValue(keyQuery, valueQuery);
    REQUIRE(v.has_value());
    CHECK_FALSE(v->isDeleted);
    CHECK(v->type == REG_BINARY);
    CHECK(v->data == payload);
  }

  {
    const auto rows = store.ListValues(keyQuery);
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows[0].isDeleted);
    CHECK(rows[0].data == payload);
  }

  // Tombstones should also be case-insensitive.
  REQUIRE(store.DeleteValue(keyQuery, valueQuery));
  {
    const auto v = store.GetValue(L"HKLM\\software\\ExampleVendor\\exampleapp", L"INSTALLDIR");
    REQUIRE(v.has_value());
    CHECK(v->isDeleted);
  }

  // Key deletion should be case-insensitive.
  REQUIRE(store.DeleteKeyTree(L"HKLM\\SOFTWARE\\EXAMPLEVENDOR"));
  CHECK(store.IsKeyDeleted(keyImport));
}

TEST_CASE("LocalRegistryStore treats parent keys with visible descendants as existing", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring childKey = L"HKLM\\Software\\ExampleVendor\\ExampleApp";
  const std::wstring parentKey = L"HKLM\\Software\\ExampleVendor";
  const std::wstring valueName = L"InstallDir";
  const std::vector<uint8_t> payload = {0x42};

  REQUIRE(store.PutValue(childKey, valueName, REG_BINARY, payload.data(), static_cast<uint32_t>(payload.size())));

  CHECK(store.KeyExistsLocally(parentKey));
  CHECK(store.KeyExistsLocally(L"HKLM\\Software"));

  REQUIRE(store.DeleteKeyTree(parentKey));
  CHECK_FALSE(store.KeyExistsLocally(parentKey));
}

TEST_CASE("PutValue normalizes key_path to canonical casing from keys table", "[store]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring key1 = L"HKLM\\Software\\CaseTest";
  const std::wstring key2 = L"HKLM\\SOFTWARE\\CaseTest";
  const uint8_t byteA = 0xAA;
  const uint8_t byteB = 0xBB;

  // First write establishes the canonical casing.
  REQUIRE(store.PutValue(key1, L"Alpha", REG_BINARY, &byteA, 1));
  // Second write uses different casing but should normalize to key1's casing.
  REQUIRE(store.PutValue(key2, L"Beta", REG_BINARY, &byteB, 1));

  const auto rows = store.ExportAll();
  int keyOnlyCount = 0;
  bool hasAlpha = false, hasBeta = false;
  std::wstring exportedKeyPath;
  for (const auto& r : rows) {
    // Case-insensitive match for our key.
    bool isOurKey = false;
    if (r.keyPath.size() == key1.size()) {
      isOurKey = true;
      for (size_t i = 0; i < r.keyPath.size(); i++) {
        if ((wchar_t)towlower(r.keyPath[i]) != (wchar_t)towlower(key1[i])) {
          isOurKey = false;
          break;
        }
      }
    }
    if (!isOurKey) continue;

    if (r.isKeyOnly) {
      keyOnlyCount++;
      exportedKeyPath = r.keyPath;
    }
    if (!r.isKeyOnly && r.valueName == L"Alpha") hasAlpha = true;
    if (!r.isKeyOnly && r.valueName == L"Beta") hasBeta = true;
  }
  // Key should appear exactly once.
  CHECK(keyOnlyCount == 1);
  // Both values should be present under the same key.
  CHECK(hasAlpha);
  CHECK(hasBeta);
  // The canonical casing should be the first-inserted form.
  CHECK(exportedKeyPath == key1);
}

TEST_CASE("ExportAll groups case-variant key paths from pre-existing DB rows", "[store]") {
  const std::wstring dbPath = MakeTempDbPath();

  // Create the store and one value normally.
  {
    LocalRegistryStore store;
    REQUIRE(store.Open(dbPath));
    const std::wstring key = L"HKLM\\Software\\RawTest";
    const uint8_t byteA = 0xAA;
    REQUIRE(store.PutValue(key, L"Normal", REG_BINARY, &byteA, 1));
  }

  // Inject a row with different key_path casing directly via SQLite,
  // simulating pre-fix data or external DB manipulation.
  {
    const std::string dbPathUtf8 = WideToUtf8(dbPath);
    REQUIRE_FALSE(dbPathUtf8.empty());
    sqlite3* rawDb = nullptr;
    REQUIRE(sqlite3_open_v2(dbPathUtf8.c_str(), &rawDb, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const char* sql =
        "INSERT INTO values_tbl(key_path, value_name, type, data, is_deleted, updated_at) "
        "VALUES('HKLM\\SOFTWARE\\RawTest', 'Injected', 3, X'BB', 0, 9999999999);";
    char* err = nullptr;
    int rc = sqlite3_exec(rawDb, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    REQUIRE(rc == SQLITE_OK);
    sqlite3_close(rawDb);
  }

  // Re-open store and export.
  LocalRegistryStore store;
  REQUIRE(store.Open(dbPath));
  const auto rows = store.ExportAll();

  int keyOnlyCount = 0;
  bool hasNormal = false, hasInjected = false;
  for (const auto& r : rows) {
    // Case-insensitive match for our key.
    std::wstring lower = r.keyPath;
    for (auto& ch : lower) ch = (wchar_t)towlower(ch);
    if (lower != L"hklm\\software\\rawtest") continue;

    if (r.isKeyOnly) keyOnlyCount++;
    if (!r.isKeyOnly && r.valueName == L"Normal") hasNormal = true;
    if (!r.isKeyOnly && r.valueName == L"Injected") hasInjected = true;
  }
  // Even though key_path has different casings in the DB, ExportAll should group them.
  CHECK(keyOnlyCount == 1);
  CHECK(hasNormal);
  CHECK(hasInjected);
}

TEST_CASE("LocalRegistryStore WAL changes are visible across concurrent opens", "[store][wal]") {
  const std::wstring dbPath = MakeTempDbPath();

  LocalRegistryStore writer;
  REQUIRE(writer.Open(dbPath));

  const std::wstring key = L"HKLM\\Software\\WalTest";
  const std::wstring name = L"Value";
  const uint8_t byteA = 0x11;
  const uint8_t byteB = 0x22;

  REQUIRE(writer.PutValue(key, name, REG_BINARY, &byteA, 1));

  // Open a second connection while the first is still open; it should be able to
  // see committed data even if it's still in the WAL sidecar.
  LocalRegistryStore reader;
  REQUIRE(reader.Open(dbPath));
  {
    const auto v = reader.GetValue(key, name);
    REQUIRE(v.has_value());
    CHECK_FALSE(v->isDeleted);
    CHECK(v->data == std::vector<uint8_t>{byteA});
  }

  REQUIRE(writer.PutValue(key, name, REG_BINARY, &byteB, 1));
  {
    const auto v = reader.GetValue(key, name);
    REQUIRE(v.has_value());
    CHECK_FALSE(v->isDeleted);
    CHECK(v->data == std::vector<uint8_t>{byteB});
  }
}

TEST_CASE("LocalRegistryStore waits through writer contention (busy_timeout)", "[store][wal]") {
  const std::wstring dbPath = MakeTempDbPath();

  // Ensure schema exists.
  {
    LocalRegistryStore init;
    REQUIRE(init.Open(dbPath));
  }

  // Open a separate raw SQLite connection and hold a write transaction open.
  sqlite3* lockDb = nullptr;
  const std::string dbPathUtf8 = WideToUtf8(dbPath);
  REQUIRE_FALSE(dbPathUtf8.empty());
  REQUIRE(sqlite3_open_v2(dbPathUtf8.c_str(), &lockDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) == SQLITE_OK);
  REQUIRE(lockDb != nullptr);
  (void)sqlite3_exec(lockDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

  // Begin an IMMEDIATE transaction to acquire the write lock.
  REQUIRE(sqlite3_exec(lockDb, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK);

  LocalRegistryStore writer;
  REQUIRE(writer.Open(dbPath));

  const std::wstring key = L"HKLM\\Software\\BusyTest";
  const std::wstring name = L"X";
  const uint8_t payload = 0x7F;

  bool putOk = false;
  auto start = std::chrono::steady_clock::now();
  std::thread t([&] {
    putOk = writer.PutValue(key, name, REG_BINARY, &payload, 1);
  });

  // Hold the lock briefly to force the PutValue to hit SQLITE_BUSY and wait.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE(sqlite3_exec(lockDb, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);

  t.join();
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

  // The operation should succeed once the lock is released (rather than failing immediately).
  CHECK(putOk);
  // Sanity: it should have waited at least a little.
  CHECK(elapsedMs >= 50);

  sqlite3_close(lockDb);

  // Verify the write actually landed.
  const auto v = writer.GetValue(key, name);
  REQUIRE(v.has_value());
  CHECK_FALSE(v->isDeleted);
  CHECK(v->data == std::vector<uint8_t>{payload});
}
