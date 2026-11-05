// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crDesk/server/server_config.h"

#include <algorithm>
#include <fcntl.h>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/json/json_reader.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "url/gurl.h"

namespace crdesk::server {
namespace {
bool KnownFields(const base::DictValue& dict,
                 std::initializer_list<std::string_view> fields) {
  for (const auto entry : dict) {
    if (std::ranges::find(fields, entry.first) == fields.end()) return false;
  }
  return true;
}

bool Identifier(std::string_view id) {
  return !id.empty() && id.size() <= 64 &&
      std::ranges::all_of(id, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
      });
}

bool Urls(const base::ListValue* values, bool turn,
          std::vector<std::string>& urls) {
  if (!values || values->size() > 8) return false;
  for (const auto& value : *values) {
    const auto* url = value.GetIfString();
    if (!url || url->size() > 512 ||
        !std::ranges::all_of(*url, [](unsigned char c) {
          return c > 32 && c < 127 && c != '@' && c != '\\';
        })) return false;
    const auto colon = url->find(':');
    if (colon == std::string::npos || colon + 1 == url->size()) return false;
    const auto scheme = url->substr(0, colon);
    if (turn ? (scheme != "turn" && scheme != "turns") : scheme != "stun")
      return false;
    urls.push_back(*url);
  }
  return true;
}
}  // namespace

bool LoadConfig(const base::FilePath& path, ServerConfig& config,
                std::string& error) {
  // Read the checked descriptor, never re-open a path after checking its mode.
  error = "Config must be an absolute, regular file owned by this user or root, with mode 0600 or 0400.";
  if (!path.IsAbsolute()) return false;
  base::ScopedFD fd(HANDLE_EINTR(open(path.value().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)));
  struct stat info{};
  if (!fd.is_valid() || fstat(fd.get(), &info) || !S_ISREG(info.st_mode) ||
      (info.st_uid != getuid() && info.st_uid != 0) ||
      ((info.st_mode & 0777) != 0600 && (info.st_mode & 0777) != 0400))
    return false;
  error = "Cannot read config, or config exceeds 1 MiB.";
  std::string text;
  char buffer[4096];
  for (;;) {
    const ssize_t count = HANDLE_EINTR(read(fd.get(), buffer, sizeof(buffer)));
    if (count < 0) return false;
    if (!count) break;
    text.append(buffer, static_cast<size_t>(count));
    if (text.size() > 1024 * 1024) return false;
  }
  auto root = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  error = "Invalid config fields or version; expected version 1.";
  if (!root || !KnownFields(*root, {"version", "listen_port", "max_connections",
                                    "devices", "stun_urls", "turn", "accounts"}) ||
      root->FindInt("version") != 1) return false;
  ServerConfig parsed;
  if (root->contains("listen_port")) {
    parsed.port = root->FindInt("listen_port").value_or(0);
  }
  if (root->contains("max_connections")) {
    parsed.max_connections = root->FindInt("max_connections").value_or(0);
  }
  error = "listen_port must be 1024..65535 and max_connections must be 2..1024.";
  if (parsed.port < 1024 || parsed.port > 65535 ||
      parsed.max_connections < 2 || parsed.max_connections > 1024) return false;
  if (const auto* auth = root->FindDict("accounts")) {
    const auto* origin = auth->FindString("origin");
    const auto* database = auth->FindString("database");
    error = "accounts requires an HTTPS domain origin, an absolute database path and both Google client fields (or neither).";
    if (!KnownFields(*auth, {"origin", "database", "google_client_id", "google_client_secret"}) || !origin || !database) return false;
    GURL url(*origin);
    if (!url.is_valid() || !url.SchemeIs("https") || url.HostIsIPAddress() || url.host().find('.') == std::string::npos ||
        url.has_username() || url.has_password() || url.has_query() || url.has_ref() || url.path() != "/") return false;
    parsed.accounts.origin = url.GetWithEmptyPath().spec();
    parsed.accounts.origin.pop_back();
    parsed.accounts.rp_id = url.host();
    parsed.accounts.database = base::FilePath(*database);
    if (!parsed.accounts.database.IsAbsolute() || parsed.accounts.database.ReferencesParent()) return false;
    if (const auto* field = auth->FindString("google_client_id")) parsed.accounts.google_client_id = *field;
    if (const auto* field = auth->FindString("google_client_secret")) parsed.accounts.google_client_secret = *field;
    if ((auth->contains("google_client_id") && !auth->FindString("google_client_id")) ||
        (auth->contains("google_client_secret") && !auth->FindString("google_client_secret")) ||
        (!parsed.accounts.google_client_id.empty() && !parsed.accounts.google_client_id.ends_with(".apps.googleusercontent.com")) ||
        parsed.accounts.google_client_id.empty() != parsed.accounts.google_client_secret.empty() ||
        parsed.accounts.google_client_id.size() > 512 || parsed.accounts.google_client_secret.size() > 512) return false;
  } else if (root->contains("accounts")) return false;
  const auto* devices = root->FindList("devices");
  error = "devices must contain 1..128 unique devices with unique 32-byte hex secrets.";
  if (!devices || (devices->empty() && parsed.accounts.origin.empty()) || devices->size() > 128) return false;
  std::set<std::vector<uint8_t>> secrets;
  for (const auto& value : *devices) {
    const auto* item = value.GetIfDict();
    if (!item || !KnownFields(*item, {"id", "name", "secret", "allowed_hosts"})) return false;
    const auto* id = item->FindString("id");
    const auto* name = item->FindString("name");
    const auto* secret = item->FindString("secret");
    const auto* allowed = item->FindList("allowed_hosts");
    DeviceConfig device;
    // With 128 devices and 64-byte names, even JSON's six-byte escaping of
    // every name character keeps a full discovery snapshot below 64 KiB.
    if (!id || !Identifier(*id) || !name || name->empty() || name->size() > 64 ||
        !base::IsStringUTF8(*name) ||
        std::ranges::any_of(*name, [](unsigned char c) { return c < 32 || c == 127; }) ||
        !secret || secret->size() != 64 || !base::HexStringToBytes(*secret, &device.secret) ||
        !std::ranges::any_of(device.secret, [](uint8_t c) { return c != 0; }) ||
        !secrets.insert(device.secret).second || !allowed || allowed->size() > 127)
      return false;
    device.name = *name;
    for (const auto& host : *allowed) {
      const auto* target = host.GetIfString();
      if (!target || !Identifier(*target) || *target == *id ||
          !device.allowed_hosts.insert(*target).second) return false;
    }
    if (!parsed.devices.emplace(*id, std::move(device)).second) return false;
  }
  error = "allowed_hosts references an unknown device.";
  for (const auto& [id, device] : parsed.devices) {
    for (const auto& host : device.allowed_hosts) {
      if (!parsed.devices.contains(host)) return false;
    }
  }
  error = "Invalid stun_urls or turn configuration.";
  if (root->contains("stun_urls") && !Urls(root->FindList("stun_urls"), false, parsed.stun_urls)) return false;
  if (root->contains("turn")) {
    const auto* turn = root->FindDict("turn");
    if (!turn || !KnownFields(*turn, {"urls", "shared_secret", "ttl_seconds"}) ||
        !Urls(turn->FindList("urls"), true, parsed.turn_urls) || parsed.turn_urls.empty()) return false;
    const auto* secret = turn->FindString("shared_secret");
    if (!secret || secret->size() < 32 || secret->size() > 256 ||
        !std::ranges::all_of(*secret, [](unsigned char c) { return c >= 33 && c < 127; })) return false;
    parsed.turn_secret = *secret;
    if (turn->contains("ttl_seconds")) parsed.turn_ttl_seconds = turn->FindInt("ttl_seconds").value_or(0);
    if (parsed.turn_ttl_seconds < 60 || parsed.turn_ttl_seconds > 3600) return false;
  }
  config = std::move(parsed);
  error.clear();
  return true;
}
}  // namespace crdesk::server
