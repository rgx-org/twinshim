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

static bool BindWideText(sqlite3_stmt* st, int index1, const std::wstring& text) {
  std::string utf8 = WideToUtf8(text);
  if (!text.empty() && utf8.empty()) {
    return false;
  }
  return sqlite3_bind_text(st, index1, utf8.data(), (int)utf8.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

static std::wstring ColumnWideText(sqlite3_stmt* st, int col) {
  const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
  int bytes = sqlite3_column_bytes(st, col);
  if (!p || bytes <= 0) {
    return {};
  }
  return Utf8ToWide(std::string(p, p + bytes));
}

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
    // The store uses WAL mode for better concurrent read/write behavior.
    // Best-effort checkpoint on clean shutdown so changes are merged back into
    // the main DB file and the -wal sidecar can be truncated.
    (void)sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
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
    if (!BindWideText(st, 1, p)) {
      ok = false;
      break;
    }
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
    if (!BindWideText(st, 1, keyPath)) {
      sqlite3_finalize(st);
      Exec("ROLLBACK;");
      return false;
    }
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
    if (!BindWideText(st, 2, keyPath) || !BindWideText(st, 3, like)) {
      sqlite3_finalize(st);
      Exec("ROLLBACK;");
      return false;
    }
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
    if (!BindWideText(st, 1, p)) {
      sqlite3_finalize(st);
      return false;
    }
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
    if (!BindWideText(st, 1, keyPath)) {
      sqlite3_finalize(st);
      return false;
    }
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
    if (!BindWideText(st, 1, keyPath)) {
      sqlite3_finalize(st);
      return false;
    }
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
  if (!BindWideText(st, 1, keyPath) || !BindWideText(st, 2, valueName)) {
    sqlite3_finalize(st);
    return false;
  }
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
  if (!BindWideText(st, 1, keyPath) || !BindWideText(st, 2, valueName)) {
    sqlite3_finalize(st);
    return false;
  }
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
  if (!BindWideText(st, 1, keyPath) || !BindWideText(st, 2, valueName)) {
    sqlite3_finalize(st);
    return std::nullopt;
  }
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
  if (!BindWideText(st, 1, keyPath)) {
    sqlite3_finalize(st);
    return rows;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    uint32_t type = (uint32_t)sqlite3_column_int(st, 1);
    const void* blob = sqlite3_column_blob(st, 2);
    int blobSize = sqlite3_column_bytes(st, 2);
    int deleted = sqlite3_column_int(st, 3);

    ValueRow r;
    r.valueName = ColumnWideText(st, 0);
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
  if (!BindWideText(st, 1, like)) {
    sqlite3_finalize(st);
    return subkeys;
  }

  std::set<std::wstring> uniq;
  const std::wstring prefix = keyPath + L"\\";
  while (sqlite3_step(st) == SQLITE_ROW) {
    int deleted = sqlite3_column_int(st, 1);
    if (deleted != 0) {
      continue;
    }
    std::wstring full = ColumnWideText(st, 0);
    if (full.empty()) {
      continue;
    }
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
    uint32_t type = (uint32_t)sqlite3_column_int(st, 2);
    const void* blob = sqlite3_column_blob(st, 3);
    int blobSize = sqlite3_column_bytes(st, 3);

    ExportRow r;
    r.keyPath = ColumnWideText(st, 0);
    r.valueName = ColumnWideText(st, 1);
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
