// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crDesk/server/account_store.h"
#include <sys/stat.h>
#include <unistd.h>
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "third_party/sqlite/sqlite3.h"
namespace crdesk::server {
namespace {
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    result_ = sqlite3_prepare_v2(db, sql, -1, &value_, nullptr);
    if (result_ != SQLITE_OK) LogError("prepare", result_);
  }
  ~Statement() { sqlite3_finalize(value_); }
  void Bind(int index, std::string_view value) {
    if (value_ && result_ == SQLITE_OK) {
      result_ = sqlite3_bind_text(value_, index, value.data(), value.size(), SQLITE_TRANSIENT);
      if (result_ != SQLITE_OK) LogError("bind", result_);
    }
  }
  int Step() {
    if (result_ != SQLITE_OK) return result_;
    const int result = value_ ? sqlite3_step(value_) : SQLITE_ERROR;
    if (result != SQLITE_ROW && result != SQLITE_DONE) LogError("step", result);
    return result;
  }
  std::string Text(int index) {
    const auto* value = sqlite3_column_text(value_, index);
    return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(value_, index)) : std::string();
  }
 private:
  void LogError(const char* stage, int result) {
    // Do not log SQL, bound values or sqlite3_errmsg(), which may contain data.
    LOG(ERROR) << "crDeskServer: account database statement failed; stage=" << stage
               << " sqlite_code=" << result << " extended_code=" << sqlite3_extended_errcode(db_);
  }
  sqlite3* db_;
  int result_ = SQLITE_OK;
  sqlite3_stmt* value_ = nullptr;
};
}
AccountStore::AccountStore() = default;
AccountStore::~AccountStore() { if (db_) sqlite3_close(db_); }
bool AccountStore::Open(const base::FilePath& path) {
  if (!path.IsAbsolute() || base::IsLink(path) || base::IsLink(path.DirName())) return false;
  // Chromium builds SQLite with SQLITE_OMIT_AUTOINIT. This standalone server
  // uses the SQLite API directly, so initialize it before opening a database.
  if (sqlite3_initialize() != SQLITE_OK) return false;
  umask(0077);
  if (!base::CreateDirectory(path.DirName())) return false;
  struct stat directory{};
  if (lstat(path.DirName().value().c_str(), &directory) || !S_ISDIR(directory.st_mode) ||
      directory.st_uid != getuid() || (directory.st_mode & 0777) != 0700) return false;
  if (sqlite3_open_v2(path.value().c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, nullptr) != SQLITE_OK) return false;
  if (!base::SetPosixFilePermissions(path, 0600)) return false;
  return sqlite3_exec(db_, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
      "CREATE TABLE IF NOT EXISTS records(kind TEXT NOT NULL,id TEXT NOT NULL,owner TEXT NOT NULL,data TEXT NOT NULL,PRIMARY KEY(kind,id));"
      "CREATE INDEX IF NOT EXISTS records_owner ON records(kind,owner);"
      "CREATE TABLE IF NOT EXISTS events(sequence INTEGER PRIMARY KEY,owner TEXT NOT NULL,created INTEGER NOT NULL,data TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS events_owner ON events(owner,sequence);"
      "CREATE INDEX IF NOT EXISTS events_created ON events(created);"
      "CREATE TABLE IF NOT EXISTS browser_sessions(hash TEXT PRIMARY KEY NOT NULL,account TEXT NOT NULL,method TEXT NOT NULL,created INTEGER NOT NULL,expires INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS browser_sessions_account ON browser_sessions(account,created);"
      "CREATE INDEX IF NOT EXISTS browser_sessions_expiry ON browser_sessions(expires);", nullptr, nullptr, nullptr) == SQLITE_OK;
}
bool AccountStore::CreateBrowserSession(const std::string& hash, const std::string& account,
                                         const std::string& method) {
  if (hash.empty() || account.empty()) return false;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
  const auto now = base::Time::Now();
  Statement insert(db_, "INSERT INTO browser_sessions(hash,account,method,created,expires) VALUES(?,?,?,?,?)");
  insert.Bind(1, hash); insert.Bind(2, account); insert.Bind(3, method);
  insert.Bind(4, base::NumberToString(now.InMillisecondsSinceUnixEpoch()));
  insert.Bind(5, base::NumberToString((now + base::Days(30)).InMillisecondsSinceUnixEpoch()));
  bool ok = insert.Step() == SQLITE_DONE;
  // Bound remembered browsers per account. Expiry is fixed, never sliding.
  Statement trim(db_, "DELETE FROM browser_sessions WHERE account=? AND hash NOT IN (SELECT hash FROM browser_sessions WHERE account=? ORDER BY created DESC,rowid DESC LIMIT 32)");
  trim.Bind(1, account); trim.Bind(2, account);
  if (ok) ok = trim.Step() == SQLITE_DONE;
  if (ok && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false;
}
std::optional<base::DictValue> AccountStore::BrowserSession(const std::string& hash) {
  if (hash.empty()) return std::nullopt;
  Statement query(db_, "SELECT account,method FROM browser_sessions WHERE hash=? AND expires>? AND EXISTS(SELECT 1 FROM records WHERE kind='account' AND id=browser_sessions.account)");
  query.Bind(1, hash);
  query.Bind(2, base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  if (query.Step() != SQLITE_ROW) return std::nullopt;
  base::DictValue session;
  session.Set("account", query.Text(0)); session.Set("method", query.Text(1));
  return session;
}
bool AccountStore::RemoveBrowserSession(const std::string& hash) {
  Statement query(db_, "DELETE FROM browser_sessions WHERE hash=?");
  query.Bind(1, hash); return query.Step() == SQLITE_DONE;
}
void AccountStore::PruneBrowserSessions() {
  Statement query(db_, "DELETE FROM browser_sessions WHERE expires<=?");
  query.Bind(1, base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  query.Step();
}
bool AccountStore::AppendEvent(const std::string& owner, base::DictValue event) {
  if (owner.empty()) return false;
  const auto now = base::Time::Now().InMillisecondsSinceUnixEpoch();
  const auto created = static_cast<int64_t>(event.FindDouble("time").value_or(static_cast<double>(now)));
  event.Set("time", static_cast<double>(created));
  auto json = base::WriteJson(event);
  if (!json || json->size() > 4096) return false;
  Statement insert(db_, "INSERT INTO events(owner,created,data) VALUES(?,?,?)");
  insert.Bind(1, owner); insert.Bind(2, base::NumberToString(created)); insert.Bind(3, *json);
  if (insert.Step() != SQLITE_DONE) return false;
  Statement trim(db_, "DELETE FROM events WHERE owner=? AND sequence <= (SELECT sequence FROM events WHERE owner=? ORDER BY sequence DESC LIMIT 1 OFFSET 500)");
  trim.Bind(1, owner); trim.Bind(2, owner);
  return trim.Step() == SQLITE_DONE;
}
base::ListValue AccountStore::Events(const std::string& owner) {
  Statement query(db_, "SELECT data FROM events WHERE owner=? AND created>=? ORDER BY sequence DESC LIMIT 100");
  query.Bind(1, owner);
  query.Bind(2, base::NumberToString((base::Time::Now() - base::Days(30)).InMillisecondsSinceUnixEpoch()));
  base::ListValue result;
  while (query.Step() == SQLITE_ROW) {
    auto row = base::JSONReader::ReadDict(query.Text(0), base::JSON_PARSE_RFC);
    if (row) result.Append(std::move(*row));
  }
  return result;
}
void AccountStore::PruneEvents() {
  Statement query(db_, "DELETE FROM events WHERE created<?");
  query.Bind(1, base::NumberToString((base::Time::Now() - base::Days(30)).InMillisecondsSinceUnixEpoch()));
  query.Step();
}
std::optional<base::DictValue> AccountStore::Get(std::string_view kind, std::string_view id) {
  Statement query(db_, "SELECT data FROM records WHERE kind=? AND id=?");
  query.Bind(1, kind); query.Bind(2, id);
  if (query.Step() != SQLITE_ROW) return std::nullopt;
  return base::JSONReader::ReadDict(query.Text(0), base::JSON_PARSE_RFC);
}
base::ListValue AccountStore::List(std::string_view kind, std::string_view owner) {
  Statement query(db_, "SELECT data FROM records WHERE kind=? AND owner=? ORDER BY id LIMIT 128");
  query.Bind(1, kind); query.Bind(2, owner);
  base::ListValue list;
  while (query.Step() == SQLITE_ROW) {
    auto data = base::JSONReader::ReadDict(query.Text(0), base::JSON_PARSE_RFC);
    if (data) list.Append(std::move(*data));
  }
  return list;
}
bool AccountStore::Put(std::string_view kind, std::string_view id, std::string_view owner,
                       const base::DictValue& data, bool insert_only) {
  auto json = base::WriteJson(data);
  if (!json || json->size() > (kind == "avatar" ? 48 * 1024 : 16384)) return false;
  if (!insert_only) {
    // Chromium builds SQLite with SQLITE_OMIT_UPSERT. Update only records owned
    // by this account, then insert if absent. A conflicting owner must still
    // fail the primary-key constraint; never replace or reassign that record.
    Statement update(db_, "UPDATE records SET data=? WHERE kind=? AND id=? AND owner=?");
    update.Bind(1, *json); update.Bind(2, kind); update.Bind(3, id); update.Bind(4, owner);
    if (update.Step() != SQLITE_DONE) return false;
    if (sqlite3_changes(db_) == 1) return true;
  }
  Statement query(db_, "INSERT INTO records(kind,id,owner,data) VALUES(?,?,?,?)");
  query.Bind(1, kind); query.Bind(2, id); query.Bind(3, owner); query.Bind(4, *json);
  return query.Step() == SQLITE_DONE && sqlite3_changes(db_) == 1;
}
bool AccountStore::Remove(std::string_view kind, std::string_view id, std::string_view owner) {
  Statement query(db_, "DELETE FROM records WHERE kind=? AND id=? AND owner=?");
  query.Bind(1, kind); query.Bind(2, id); query.Bind(3, owner);
  return query.Step() == SQLITE_DONE && sqlite3_changes(db_) == 1;
}
bool AccountStore::PutDeviceAvatar(const std::string& id, const std::string& owner,
                                    const base::DictValue& device, const base::DictValue& avatar) {
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
  bool ok = Put("device", id, owner, device);
  if (ok && !avatar.empty()) ok = Put("avatar", id, owner, avatar);
  if (ok && avatar.empty()) {
    Statement remove(db_, "DELETE FROM records WHERE kind='avatar' AND id=? AND owner=?");
    remove.Bind(1, id); remove.Bind(2, owner); ok = remove.Step() == SQLITE_DONE;
  }
  if (ok && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false;
}
bool AccountStore::CreatePasskeyAccount(const std::string& account, const std::string& name,
                                        const std::string& credential, const base::DictValue& key) {
  // Commit the identity and its first credential together, or neither.
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
  auto profile = base::DictValue().Set("account", account).Set("name", name);
  if (Put("account", account, account, profile, true) &&
      Put("passkey", credential, account, key, true) &&
      sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false;
}
std::string AccountStore::GoogleAccount(std::string_view subject, std::string_view email) {
  // Google subject, never the email address, is the account identity.
  if (auto existing = Get("google", subject)) {
    const auto* id = existing->FindString("account");
    if (!id || id->empty()) return {};
    auto profile = base::DictValue().Set("account", *id).Set("email", email);
    return Put("account", *id, *id, profile) ? *id : std::string();
  }
  const auto id = base::HexEncode(base::RandBytesAsString(32));
  auto account = base::DictValue().Set("account", id).Set("email", email);
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return {};
  if (Put("account", id, id, account, true) && Put("google", subject, id, account, true) &&
      sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) return id;
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return {};
}
}
