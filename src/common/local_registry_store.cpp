#include "common/local_registry_store.h"

#include "common/utf8.h"

#include <sqlite3.h>

#include <ctime>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>

namespace hklmwrap {

static int64_t NowUnixSeconds() {
  // Good enough for change ordering; doesn't need to be monotonic.
  return (int64_t)time(nullptr);
}

static std::vector<std::wstring> KeyPrefixes(const std::wstring& keyPath);

static std::wstring CaseFoldWide(const std::wstring& s) {
  std::wstring out;
  out.resize(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    out[i] = (wchar_t)towlower(s[i]);
  }
  return out;
}

static bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
  if (prefix.size() > s.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if ((wchar_t)towlower(s[i]) != (wchar_t)towlower(prefix[i])) {
      return false;
    }
  }
  return true;
}

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

  // Support concurrent wrapper + hklmreg access (WAL allows readers during writes,
  // but writers can contend). Give operations a chance to wait instead of
  // immediately failing with SQLITE_BUSY.
  (void)sqlite3_busy_timeout(db_, 5000);

  // Enable extended result codes so callers can distinguish SQLITE_BUSY variants
  // when debugging. (We still treat them as failure in this layer.)
  (void)sqlite3_extended_result_codes(db_, 1);

  Exec("PRAGMA journal_mode=WAL;");
  Exec("PRAGMA synchronous=NORMAL;");
  Exec("PRAGMA foreign_keys=ON;");

  // Keep WAL sidecars from growing without bound in long-running sessions.
  // This doesn't affect visibility (readers can always see committed WAL pages),
  // but improves steady-state behavior.
  (void)sqlite3_wal_autocheckpoint(db_, 256);
  return EnsureSchema();
}

void LocalRegistryStore::Close() {
  if (db_) {
    // The store uses WAL mode for better concurrent read/write behavior.
    // Best-effort checkpoint on clean shutdown so changes are merged back into
    // the main DB file and the -wal sidecar can be truncated.
    //
    // Don't allow a busy handler to stall shutdown if another process is actively
    // reading/writing.
    (void)sqlite3_busy_timeout(db_, 0);
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

  const auto now = NowUnixSeconds();

  // Registry keys are case-insensitive. Prefer updating any existing row that
  // matches case-insensitively; only insert if nothing matches.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE keys SET is_deleted=0, updated_at=? WHERE key_path=? COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_int64(st, 1, now);
    if (!BindWideText(st, 2, keyPath)) {
      sqlite3_finalize(st);
      return false;
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      return false;
    }

    if (sqlite3_changes(db_) == 0) {
      sqlite3_stmt* stIns = nullptr;
      const char* sqlIns = "INSERT INTO keys(key_path, is_deleted, updated_at) VALUES(?,0,?) "
                           "ON CONFLICT(key_path) DO UPDATE SET is_deleted=0, updated_at=excluded.updated_at;";
      if (sqlite3_prepare_v2(db_, sqlIns, -1, &stIns, nullptr) != SQLITE_OK) {
        return false;
      }
      if (!BindWideText(stIns, 1, keyPath)) {
        sqlite3_finalize(stIns);
        return false;
      }
      sqlite3_bind_int64(stIns, 2, now);
      rc = sqlite3_step(stIns);
      sqlite3_finalize(stIns);
      if (rc != SQLITE_DONE) {
        return false;
      }
    }
  }

  // Undelete ancestor prefixes only if they already exist (to avoid creating
  // a bunch of implicit parent keys that weren't explicitly written).
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE keys SET is_deleted=0, updated_at=? WHERE key_path=? COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    auto prefixes = KeyPrefixes(keyPath);
    for (size_t idx = 1; idx < prefixes.size(); idx++) {
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
      sqlite3_bind_int64(st, 1, now);
      if (!BindWideText(st, 2, prefixes[idx])) {
        sqlite3_finalize(st);
        return false;
      }
      int rc = sqlite3_step(st);
      if (rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        return false;
      }
    }
    sqlite3_finalize(st);
  }

  return true;
}

bool LocalRegistryStore::DeleteKeyTree(const std::wstring& keyPath) {
  if (!db_) {
    return false;
  }

  Exec("BEGIN IMMEDIATE;");

  // Mark exact key as deleted (case-insensitive).
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE keys SET is_deleted=1, updated_at=? WHERE key_path=? COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      Exec("ROLLBACK;");
      return false;
    }
    sqlite3_bind_int64(st, 1, NowUnixSeconds());
    if (!BindWideText(st, 2, keyPath)) {
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

    if (sqlite3_changes(db_) == 0) {
      sqlite3_stmt* stIns = nullptr;
      const char* sqlIns = "INSERT INTO keys(key_path, is_deleted, updated_at) VALUES(?,1,?) "
                           "ON CONFLICT(key_path) DO UPDATE SET is_deleted=1, updated_at=excluded.updated_at;";
      if (sqlite3_prepare_v2(db_, sqlIns, -1, &stIns, nullptr) != SQLITE_OK) {
        Exec("ROLLBACK;");
        return false;
      }
      if (!BindWideText(stIns, 1, keyPath)) {
        sqlite3_finalize(stIns);
        Exec("ROLLBACK;");
        return false;
      }
      sqlite3_bind_int64(stIns, 2, NowUnixSeconds());
      rc = sqlite3_step(stIns);
      sqlite3_finalize(stIns);
      if (rc != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return false;
      }
    }
  }

  // Mark values under prefix as deleted.
  {
    std::wstring like = keyPath;
    like.append(L"\\%");
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE values_tbl SET is_deleted=1, updated_at=? WHERE key_path=? COLLATE NOCASE OR (key_path COLLATE NOCASE) LIKE ?;";
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
    const char* sql = "SELECT MAX(is_deleted) FROM keys WHERE key_path=? COLLATE NOCASE;";
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
    const char* sql = "SELECT 1 FROM keys WHERE key_path=? COLLATE NOCASE AND is_deleted=0 LIMIT 1;";
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
    const char* sql = "SELECT 1 FROM values_tbl WHERE key_path=? COLLATE NOCASE AND is_deleted=0 LIMIT 1;";
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

  const auto now = NowUnixSeconds();

  // Update any existing row matching case-insensitively; only insert if nothing matches.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE values_tbl SET type=?, data=?, is_deleted=0, updated_at=? "
        "WHERE key_path=? COLLATE NOCASE AND value_name=? COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_int(st, 1, (int)type);
    if (data && dataSize) {
      sqlite3_bind_blob(st, 2, data, (int)dataSize, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(st, 2);
    }
    sqlite3_bind_int64(st, 3, now);
    if (!BindWideText(st, 4, keyPath) || !BindWideText(st, 5, valueName)) {
      sqlite3_finalize(st);
      return false;
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      return false;
    }
    if (sqlite3_changes(db_) != 0) {
      return true;
    }
  }

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
  sqlite3_bind_int64(st, 5, now);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE;
}

bool LocalRegistryStore::DeleteValue(const std::wstring& keyPath, const std::wstring& valueName) {
  if (!db_) {
    return false;
  }
  PutKey(keyPath);

  const auto now = NowUnixSeconds();

  // Update any existing row matching case-insensitively; only insert if nothing matches.
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE values_tbl SET is_deleted=1, updated_at=? WHERE key_path=? COLLATE NOCASE AND value_name=? COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_int64(st, 1, now);
    if (!BindWideText(st, 2, keyPath) || !BindWideText(st, 3, valueName)) {
      sqlite3_finalize(st);
      return false;
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      return false;
    }
    if (sqlite3_changes(db_) != 0) {
      return true;
    }
  }

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
  sqlite3_bind_int64(st, 3, now);
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
  const char* sql =
      "SELECT type, data, is_deleted FROM values_tbl "
      "WHERE key_path=? COLLATE NOCASE AND value_name=? COLLATE NOCASE "
      "ORDER BY updated_at DESC LIMIT 1;";
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
  const char* sql =
      "SELECT value_name, type, data, is_deleted, updated_at FROM values_tbl "
      "WHERE key_path=? COLLATE NOCASE "
      "ORDER BY value_name COLLATE NOCASE ASC, updated_at DESC;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return rows;
  }
  if (!BindWideText(st, 1, keyPath)) {
    sqlite3_finalize(st);
    return rows;
  }

  std::set<std::wstring> seenFolded;
  while (sqlite3_step(st) == SQLITE_ROW) {
    uint32_t type = (uint32_t)sqlite3_column_int(st, 1);
    const void* blob = sqlite3_column_blob(st, 2);
    int blobSize = sqlite3_column_bytes(st, 2);
    int deleted = sqlite3_column_int(st, 3);

    ValueRow r;
    r.valueName = ColumnWideText(st, 0);
    const auto folded = CaseFoldWide(r.valueName);
    if (seenFolded.find(folded) != seenFolded.end()) {
      continue;
    }
    seenFolded.insert(folded);
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
  const char* sql = "SELECT key_path, is_deleted FROM keys WHERE (key_path COLLATE NOCASE) LIKE ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return subkeys;
  }
  if (!BindWideText(st, 1, like)) {
    sqlite3_finalize(st);
    return subkeys;
  }

  std::map<std::wstring, std::wstring> foldedToDisplay;
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
    if (!StartsWithNoCase(full, prefix)) {
      continue;
    }
    std::wstring rem = full.substr(prefix.size());
    auto pos = rem.find(L'\\');
    std::wstring child = (pos == std::wstring::npos) ? rem : rem.substr(0, pos);
    if (!child.empty()) {
      const auto folded = CaseFoldWide(child);
      if (foldedToDisplay.find(folded) == foldedToDisplay.end()) {
        foldedToDisplay.emplace(folded, child);
      }
    }
  }
  sqlite3_finalize(st);

  subkeys.reserve(foldedToDisplay.size());
  for (auto& kv : foldedToDisplay) {
    subkeys.push_back(std::move(kv.second));
  }
  return subkeys;
}

std::vector<LocalRegistryStore::ExportRow> LocalRegistryStore::ExportAll() {
  std::vector<ExportRow> rows;
  if (!db_) {
    return rows;
  }

  // Gather values.
  struct ValueExport {
    std::wstring valueName;
    uint32_t type = 0;
    std::vector<uint8_t> data;
  };
  std::map<std::wstring, std::vector<ValueExport>> valuesByKey;
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT key_path, value_name, type, data FROM values_tbl WHERE is_deleted=0 ORDER BY key_path, value_name;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return rows;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      ValueExport v;
      const void* blob = sqlite3_column_blob(st, 3);
      int blobSize = sqlite3_column_bytes(st, 3);
      std::wstring keyPath = ColumnWideText(st, 0);
      v.valueName = ColumnWideText(st, 1);
      v.type = (uint32_t)sqlite3_column_int(st, 2);
      if (blob && blobSize > 0) {
        v.data.resize((size_t)blobSize);
        std::memcpy(v.data.data(), blob, (size_t)blobSize);
      }
      if (!keyPath.empty()) {
        valuesByKey[std::move(keyPath)].push_back(std::move(v));
      }
    }
    sqlite3_finalize(st);
  }

  // Gather explicitly-created keys.
  std::set<std::wstring> keys;
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT key_path FROM keys WHERE is_deleted=0 ORDER BY key_path;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return rows;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      std::wstring keyPath = ColumnWideText(st, 0);
      if (!keyPath.empty()) {
        keys.insert(std::move(keyPath));
      }
    }
    sqlite3_finalize(st);
  }

  // Include any keys present only via values (for backward compatibility).
  for (const auto& kv : valuesByKey) {
    keys.insert(kv.first);
  }

  // Emit one key header row per key, followed by its values.
  for (const auto& keyPath : keys) {
    if (IsKeyDeleted(keyPath)) {
      continue;
    }
    ExportRow keyOnly;
    keyOnly.keyPath = keyPath;
    keyOnly.isKeyOnly = true;
    rows.push_back(std::move(keyOnly));

    auto it = valuesByKey.find(keyPath);
    if (it == valuesByKey.end()) {
      continue;
    }
    for (const auto& v : it->second) {
      ExportRow r;
      r.keyPath = keyPath;
      r.valueName = v.valueName;
      r.type = v.type;
      r.data = v.data;
      rows.push_back(std::move(r));
    }
  }

  return rows;
}

}
