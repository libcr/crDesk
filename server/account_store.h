// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRDESK_SERVER_ACCOUNT_STORE_H_
#define CRDESK_SERVER_ACCOUNT_STORE_H_
#include <optional>
#include <string>
#include "base/files/file_path.h"
#include "base/values.h"
struct sqlite3;
namespace crdesk::server {
// Single-instance SQLite store. All callers run on the server IO sequence.
// Bounded records and prepared statements; login challenges stay in memory.
class AccountStore {
 public:
  AccountStore();
  ~AccountStore();
  bool Open(const base::FilePath& path);
  std::optional<base::DictValue> Get(std::string_view kind, std::string_view id);
  base::ListValue List(std::string_view kind, std::string_view owner);
  bool Put(std::string_view kind, std::string_view id, std::string_view owner,
           const base::DictValue& data, bool insert_only = false);
  bool Remove(std::string_view kind, std::string_view id, std::string_view owner);
  bool PutDeviceAvatar(const std::string& id, const std::string& owner,
                       const base::DictValue& device, const base::DictValue& avatar);
  bool CreatePasskeyAccount(const std::string& account, const std::string& name,
                            const std::string& credential, const base::DictValue& key);
  std::string GoogleAccount(std::string_view subject, std::string_view email);
  bool AppendEvent(const std::string& owner, base::DictValue event);
  base::ListValue Events(const std::string& owner);
  void PruneEvents();
  bool CreateBrowserSession(const std::string& hash, const std::string& account,
                            const std::string& method);
  std::optional<base::DictValue> BrowserSession(const std::string& hash);
  bool RemoveBrowserSession(const std::string& hash);
  void PruneBrowserSessions();
 private:
  sqlite3* db_ = nullptr;
};
}
#endif
