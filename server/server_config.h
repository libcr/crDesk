// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRDESK_SERVER_SERVER_CONFIG_H_
#define CRDESK_SERVER_SERVER_CONFIG_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace crdesk::server {

struct DeviceConfig {
  std::string name;
  std::string account;
  std::string group, note;
  std::vector<std::string> tags;
  int order = 0;
  int avatar = 0;
  std::string avatar_hash;
  std::vector<uint8_t> secret;
  std::set<std::string> allowed_hosts;
};

struct AccountConfig {
  std::string origin;
  std::string rp_id;
  base::FilePath database;
  std::string google_client_id;
  std::string google_client_secret;
};

struct ServerConfig {
  AccountConfig accounts;
  int port = 9080;
  int max_connections = 256;
  std::map<std::string, DeviceConfig> devices;
  std::vector<std::string> stun_urls;
  std::vector<std::string> turn_urls;
  std::string turn_secret;
  int turn_ttl_seconds = 600;
};

bool LoadConfig(const base::FilePath& path, ServerConfig& config,
                std::string& error);

}  // namespace crdesk::server
#endif  // CRDESK_SERVER_SERVER_CONFIG_H_
