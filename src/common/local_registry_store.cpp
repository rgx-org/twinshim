#include "common/local_registry_store.h"

#include "common/utf8.h"

#include <sqlite3.h>

#include <ctime>
#include <cstring>
#include <set>

namespace hklmwrap {

static int64_t NowUnixSeconds() {
  // Good enough for change ordering; doesn't need to be monotonic.
  return (int64_t)time(nullptr);
}

static std::vector<std::wstring> KeyPrefixes(const std::wstring& keyPath);

LocalRegistryStore::LocalRegistryStore() = default;

LocalRegistryStore::~LocalRegistryStore() {
  Close();
}

bool LocalRegistryStore::Open(const std::wstring& dbPath) {
  Close();
  std::string utf8 = WideToUtf8(dbPath);
  if (utf8.empty()) {
    return false;
  }

  int rc = sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    Close();
    return false;
  }

  Exec("PRAGMA journal_mode=WAL;");
  Exec("PRAGMA synchronous=NORMAL;");
  Exec("PRAGMA foreign_keys=ON;");
  return EnsureSchema();
}

void LocalRegistryStore::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool LocalRegistryStore::Exec(const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (err) {
    sqlite3_free(err);
  }
  return rc == SQLITE_OK;
}

bool LocalRegistryStore::EnsureSchema() {
  return Exec(
             "CREATE TABLE IF NOT EXISTS keys("
             "  key_path TEXT PRIMARY KEY,"
             "  is_deleted INTEGER NOT NULL DEFAULT 0,"
             "  updated_at INTEGER NOT NULL"
             ");") &&
         Exec(
             "CREATE TABLE IF NOT EXISTS values_tbl("
             "  key_path TEXT NOT NULL,"
             "  value_name TEXT NOT NULL,"
             "  type INTEGER NOT NULL,"
             "  data BLOB,"
             "  is_deleted INTEGER NOT NULL DEFAULT 0,"
             "  updated_at INTEGER NOT NULL,"
             "  PRIMARY KEY(key_path, value_name)"
             ");") &&
         Exec("CREATE INDEX IF NOT EXISTS idx_values_key ON values_tbl(key_path);");
}

bool LocalRegistryStore::PutKey(const std::wstring& keyPath) {
  if (!db_) {
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO keys(key_path, is_deleted, updated_at) VALUES(?,0,?) "
                    "ON CONFLICT(key_path) DO UPDATE SET is_deleted=0, updated_at=excluded.updated_at;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }

  // Undelete all prefixes as well (recreating a child implies parents exist).
  auto prefixes = KeyPrefixes(keyPath);
  const auto now = NowUnixSeconds();
  bool ok = true;
  for (const auto& p : prefixes) {
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text16(st, 1, p.c_str(), (int)(p.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
      ok = false;
      break;
    }
  }
  sqlite3_finalize(st);
  return ok;
}

bool LocalRegistryStore::DeleteKeyTree(const std::wstring& keyPath) {
  if (!db_) {
    return false;
  }

  Exec("BEGIN IMMEDIATE;");

  // Mark exact key and any known subkeys as deleted.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO keys(key_path, is_deleted, updated_at) VALUES(?,1,?) "
                      "ON CONFLICT(key_path) DO UPDATE SET is_deleted=1, updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      Exec("ROLLBACK;");
      return false;
    }
    sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, NowUnixSeconds());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      Exec("ROLLBACK;");
      return false;
    }
  }

  // Mark values under prefix as deleted.
  {
    std::wstring like = keyPath;
    like.append(L"\\%");
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE values_tbl SET is_deleted=1, updated_at=? WHERE key_path=? OR key_path LIKE ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      Exec("ROLLBACK;");
      return false;
    }
    sqlite3_bind_int64(st, 1, NowUnixSeconds());
    sqlite3_bind_text16(st, 2, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    sqlite3_bind_text16(st, 3, like.c_str(), (int)(like.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      Exec("ROLLBACK;");
      return false;
    }
  }

  Exec("COMMIT;");
  return true;
}

static std::vector<std::wstring> KeyPrefixes(const std::wstring& keyPath) {
  // e.g. HKLM\A\B -> [HKLM\A\B, HKLM\A, HKLM]
  std::vector<std::wstring> out;
  std::wstring cur = keyPath;
  while (true) {
    out.push_back(cur);
    auto pos = cur.find_last_of(L'\\');
    if (pos == std::wstring::npos) {
      break;
    }
    cur = cur.substr(0, pos);
    if (cur.empty()) {
      break;
    }
  }
  return out;
}

bool LocalRegistryStore::IsKeyDeleted(const std::wstring& keyPath) {
  if (!db_) {
    return false;
  }
  for (const auto& p : KeyPrefixes(keyPath)) {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT is_deleted FROM keys WHERE key_path=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text16(st, 1, p.c_str(), (int)(p.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      int deleted = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
      if (deleted != 0) {
        return true;
      }
    } else {
      sqlite3_finalize(st);
    }
  }
  return false;
}

bool LocalRegistryStore::KeyExistsLocally(const std::wstring& keyPath) {
  if (!db_) {
    return false;
  }
  if (IsKeyDeleted(keyPath)) {
    return false;
  }

  // Key row.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM keys WHERE key_path=? AND is_deleted=0 LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW) {
      return true;
    }
  }
  // Any value under the key.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM values_tbl WHERE key_path=? AND is_deleted=0 LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW;
  }
}

bool LocalRegistryStore::PutValue(const std::wstring& keyPath,
                                 const std::wstring& valueName,
                                 uint32_t type,
                                 const void* data,
                                 uint32_t dataSize) {
  if (!db_) {
    return false;
  }
  PutKey(keyPath);

  sqlite3_stmt* st = nullptr;
  const char* sql =
      "INSERT INTO values_tbl(key_path, value_name, type, data, is_deleted, updated_at) VALUES(?,?,?,?,0,?) "
      "ON CONFLICT(key_path, value_name) DO UPDATE SET type=excluded.type, data=excluded.data, is_deleted=0, "
      "updated_at=excluded.updated_at;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  sqlite3_bind_text16(st, 2, valueName.c_str(), (int)(valueName.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, (int)type);
  if (data && dataSize) {
    sqlite3_bind_blob(st, 4, data, (int)dataSize, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(st, 4);
  }
  sqlite3_bind_int64(st, 5, NowUnixSeconds());
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

bool LocalRegistryStore::DeleteValue(const std::wstring& keyPath, const std::wstring& valueName) {
  if (!db_) {
    return false;
  }
  PutKey(keyPath);

  sqlite3_stmt* st = nullptr;
  const char* sql =
      "INSERT INTO values_tbl(key_path, value_name, type, data, is_deleted, updated_at) VALUES(?,?,0,NULL,1,?) "
      "ON CONFLICT(key_path, value_name) DO UPDATE SET is_deleted=1, updated_at=excluded.updated_at;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  sqlite3_bind_text16(st, 2, valueName.c_str(), (int)(valueName.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, NowUnixSeconds());
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

std::optional<StoredValue> LocalRegistryStore::GetValue(const std::wstring& keyPath, const std::wstring& valueName) {
  if (!db_) {
    return std::nullopt;
  }
  if (IsKeyDeleted(keyPath)) {
    StoredValue tombstone;
    tombstone.isDeleted = true;
    return tombstone;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT type, data, is_deleted FROM values_tbl WHERE key_path=? AND value_name=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  sqlite3_bind_text16(st, 2, valueName.c_str(), (int)(valueName.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    return std::nullopt;
  }
  StoredValue v;
  v.type = (uint32_t)sqlite3_column_int(st, 0);
  const void* blob = sqlite3_column_blob(st, 1);
  int blobSize = sqlite3_column_bytes(st, 1);
  int deleted = sqlite3_column_int(st, 2);
  v.isDeleted = deleted != 0;
  if (blob && blobSize > 0) {
    v.data.resize((size_t)blobSize);
    std::memcpy(v.data.data(), blob, (size_t)blobSize);
  }
  sqlite3_finalize(st);
  return v;
}

std::vector<LocalRegistryStore::ValueRow> LocalRegistryStore::ListValues(const std::wstring& keyPath) {
  std::vector<ValueRow> rows;
  if (!db_) {
    return rows;
  }
  if (IsKeyDeleted(keyPath)) {
    return rows;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT value_name, type, data, is_deleted FROM values_tbl WHERE key_path=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return rows;
  }
  sqlite3_bind_text16(st, 1, keyPath.c_str(), (int)(keyPath.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const void* n16 = sqlite3_column_text16(st, 0);
    int nBytes = sqlite3_column_bytes16(st, 0);
    uint32_t type = (uint32_t)sqlite3_column_int(st, 1);
    const void* blob = sqlite3_column_blob(st, 2);
    int blobSize = sqlite3_column_bytes(st, 2);
    int deleted = sqlite3_column_int(st, 3);

    ValueRow r;
    if (n16 && nBytes > 0) {
      r.valueName.assign((const wchar_t*)n16, (size_t)nBytes / sizeof(wchar_t));
    }
    r.type = type;
    r.isDeleted = deleted != 0;
    if (blob && blobSize > 0) {
      r.data.resize((size_t)blobSize);
      std::memcpy(r.data.data(), blob, (size_t)blobSize);
    }
    rows.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return rows;
}

std::vector<std::wstring> LocalRegistryStore::ListImmediateSubKeys(const std::wstring& keyPath) {
  std::vector<std::wstring> subkeys;
  if (!db_) {
    return subkeys;
  }
  if (IsKeyDeleted(keyPath)) {
    return subkeys;
  }

  std::wstring like = keyPath;
  like.append(L"\\%");

  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT key_path, is_deleted FROM keys WHERE key_path LIKE ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return subkeys;
  }
  sqlite3_bind_text16(st, 1, like.c_str(), (int)(like.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);

  std::set<std::wstring> uniq;
  const std::wstring prefix = keyPath + L"\\";
  while (sqlite3_step(st) == SQLITE_ROW) {
    int deleted = sqlite3_column_int(st, 1);
    if (deleted != 0) {
      continue;
    }
    const void* k16 = sqlite3_column_text16(st, 0);
    int kBytes = sqlite3_column_bytes16(st, 0);
    if (!k16 || kBytes <= 0) {
      continue;
    }
    std::wstring full((const wchar_t*)k16, (size_t)kBytes / sizeof(wchar_t));
    if (full.size() <= prefix.size()) {
      continue;
    }
    if (full.rfind(prefix, 0) != 0) {
      continue;
    }
    std::wstring rem = full.substr(prefix.size());
    auto pos = rem.find(L'\\');
    std::wstring child = (pos == std::wstring::npos) ? rem : rem.substr(0, pos);
    if (!child.empty()) {
      uniq.insert(child);
    }
  }
  sqlite3_finalize(st);

  subkeys.assign(uniq.begin(), uniq.end());
  return subkeys;
}

std::vector<LocalRegistryStore::ExportRow> LocalRegistryStore::ExportAll() {
  std::vector<ExportRow> rows;
  if (!db_) {
    return rows;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT key_path, value_name, type, data FROM values_tbl WHERE is_deleted=0 ORDER BY key_path, value_name;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return rows;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const void* k16 = sqlite3_column_text16(st, 0);
    const void* n16 = sqlite3_column_text16(st, 1);
    int kBytes = sqlite3_column_bytes16(st, 0);
    int nBytes = sqlite3_column_bytes16(st, 1);
    uint32_t type = (uint32_t)sqlite3_column_int(st, 2);
    const void* blob = sqlite3_column_blob(st, 3);
    int blobSize = sqlite3_column_bytes(st, 3);

    ExportRow r;
    if (k16 && kBytes > 0) {
      r.keyPath.assign((const wchar_t*)k16, (size_t)kBytes / sizeof(wchar_t));
    }
    if (n16 && nBytes > 0) {
      r.valueName.assign((const wchar_t*)n16, (size_t)nBytes / sizeof(wchar_t));
    }
    r.type = type;
    if (blob && blobSize > 0) {
      r.data.resize((size_t)blobSize);
      std::memcpy(r.data.data(), blob, (size_t)blobSize);
    }
    rows.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return rows;
}

}
