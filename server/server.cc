// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crDesk/server/server.h"
#include "crDesk/avatar_format.h"
#include "crDesk/cloud_http.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "crypto/hmac.h"
#include "crypto/hash.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"

namespace crdesk::server {
namespace {
constexpr size_t kMaxMessage = 64 * 1024;
constexpr size_t kMaxOutgoingMessage = 256 * 1024;
constexpr size_t kMaxSignal = 48 * 1024;
constexpr auto kTraffic = net::DefineNetworkTrafficAnnotation("crdesk_server_signaling", R"(
  semantics {
    sender: "crDesk rendezvous server"
    description: "Replies to authenticated devices through a local reverse proxy, reporting permitted device presence and relaying connection signaling. Media and files are not relayed by this service."
    trigger: "An administrator runs crDeskServer and a registered device connects."
    data: "Device identifiers, avatar thumbnails, presence, session signaling and temporary TURN credentials."
    destination: OTHER
    destination_other: "Administrator-configured reverse proxy and connected crDesk devices."
  }
  policy {
    cookies_allowed: NO
    setting: "Standalone service enabled and configured by its administrator."
    policy_exception_justification: "Not part of Chrome; this is an administrator-operated crDesk service."
  }
)");

base::DictValue Message(std::string_view type) {
  base::DictValue result;
  result.Set("type", type);
  return result;
}

std::string RandomId() {
  return base::ToLowerASCII(base::HexEncode(base::RandBytesAsString(32)));
}

// Correlation IDs are deliberately small and safe to reflect to another peer.
bool RequestId(const std::string* value) {
  return value && !value->empty() && value->size() <= 64 &&
      std::ranges::all_of(*value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
      });
}
}  // namespace

Server::Server(ServerConfig config) : config_(std::move(config)) {}
Server::~Server() = default;

int Server::Start() {
  if (!config_.accounts.origin.empty()) {
    accounts_ = std::make_unique<AccountAuth>(config_.accounts);
    if (!accounts_->Start()) return net::ERR_FAILED;
    accounts_->SetStatusProvider(base::BindRepeating(&Server::AccountStatus, base::Unretained(this)));
  }
  auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  int result = socket->Listen(net::IPEndPoint(net::IPAddress::IPv4Localhost(), config_.port),
                              128, std::nullopt);
  if (result != net::OK) return result;
  http_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  timer_.Start(FROM_HERE, base::Seconds(1), this, &Server::Tick);
  LOG(INFO) << "crDeskServer: listening on 127.0.0.1:" << config_.port
            << "; devices=" << config_.devices.size();
  return net::OK;
}

void Server::OnConnect(int id) {
  if (connections_.size() >= static_cast<size_t>(config_.max_connections)) {
    http_->Close(id);
    return;
  }
  connections_.try_emplace(id);
  http_->SetReceiveBufferSize(id, 128 * 1024);
  http_->SetSendBufferSize(id, 512 * 1024);
  http_->SetCloseOnWriteBufferOverflow(id, true);
}

void Server::OnHttpRequest(int id, const net::HttpServerRequestInfo& request) {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.closing) return;
  it->second.closing = true;
  if (accounts_ && request.path.starts_with("/auth/")) {
    accounts_->Handle(request, base::BindOnce(&Server::ReplyHttp, weak_factory_.GetWeakPtr(), id));
    return;
  }
  net::HttpServerResponseInfo response(request.method == "GET" && request.path == "/healthz"
      ? net::HTTP_OK : net::HTTP_NOT_FOUND);
  response.AddHeader("Cache-Control", "no-store");
  response.SetBody(request.method == "GET" && request.path == "/healthz"
      ? "{\"status\":\"ok\"}\n" : "{\"error\":\"not_found\"}\n", "application/json");
  ReplyHttp(id, std::move(response));
}

void Server::ReplyHttp(int id, net::HttpServerResponseInfo response) {
  if (!connections_.contains(id)) return;
  response.AddHeader("Connection", "close");
  if (!http_->SendRawWithCompletion(id, response.Serialize(), kTraffic,
        base::BindOnce([](base::WeakPtr<Server> server, int connection) {
          if (server) server->http_->Close(connection);
        }, weak_factory_.GetWeakPtr(), id))) http_->Close(id);
}

void Server::OnWebSocketRequest(int id, const net::HttpServerRequestInfo& request) {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.closing) return;
  const bool browser = request.path == "/auth/remote/ws";
  std::string browser_session;
  if (browser && accounts_ && request.GetHeaderValue("origin") == config_.accounts.origin)
    browser_session = accounts_->BrowserSessionHash(request);
  std::string decoded_key;
  if (it->second.websocket || request.method != "GET" || (!browser && request.path != "/v1/ws") ||
      !request.peer.address().IsLoopback() ||
      (browser ? browser_session.empty() || !accounts_ || accounts_->BrowserAccount(browser_session).empty()
               : !request.GetHeaderValue("origin").empty()) ||
      request.GetHeaderValue("sec-websocket-version") != "13" ||
      !base::Base64Decode(request.GetHeaderValue("sec-websocket-key"), &decoded_key) ||
      decoded_key.size() != 16 || !request.data.empty()) {
    Drop(id, "invalid-upgrade");
    return;
  }
  // No compression negotiation or decompression work on either endpoint.
  auto handshake = request;
  handshake.headers.erase("sec-websocket-extensions");
  it->second.websocket = true;
  it->second.browser = browser;
  it->second.browser_session = std::move(browser_session);
  it->second.nonce = RandomId();
  auto challenge = Message("challenge");
  challenge.Set("version", 1);
  challenge.Set("nonce", it->second.nonce);
  challenge.Set("features", base::ListValue().Append("fast_reconnect_v1"));
  http_->AcceptWebSocket(id, handshake, kTraffic);
  http_->SetWebSocketMessageLimits(id, kMaxMessage, 256);
  Send(id, std::move(challenge));
}

void Server::OnWebSocketMessage(int id, std::string data) {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.closing) return;
  auto& connection = it->second;
  const auto now = base::TimeTicks::Now();
  const double seconds = (now - connection.budget_time).InSecondsF();
  connection.budget_time = now;
  connection.messages = std::min(128.0, connection.messages + seconds * 64);
  connection.bytes = std::min(512.0 * 1024, connection.bytes + seconds * 256 * 1024);
  connection.requests = std::min(6.0, connection.requests + seconds / 10);
  if (!connection.websocket || data.size() > kMaxMessage ||
      connection.messages < 1 || connection.bytes < data.size()) {
    Drop(id, "message-limit");
    return;
  }
  connection.messages -= 1;
  connection.bytes -= data.size();
  auto message = base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC);
  if (!message || !message->FindString("type")) {
    Drop(id, "invalid-message");
    return;
  }
  connection.last_message = now;
  if (connection.browser && (!accounts_ || accounts_->BrowserAccount(connection.browser_session).empty())) {
    Drop(id, "browser-session-expired"); return;
  }
  if (connection.device.empty() && connection.browser) AuthenticateBrowser(id, *message);
  else if (connection.device.empty()) Authenticate(id, *message);
  else HandleMessage(id, *message);
}

void Server::AuthenticateBrowser(int id, const base::DictValue& message) {
  auto& connection = connections_.at(id);
  const auto* ticket = message.FindString("ticket");
  const auto* type = message.FindString("type");
  auto account = accounts_ && ticket && type && *type == "authenticate_browser" &&
      base::TimeTicks::Now() - connection.opened < base::Seconds(10)
      ? accounts_->ConsumeRemoteTicket(*ticket, connection.browser_session) : std::string();
  const auto count = std::ranges::count_if(connections_, [&](const auto& entry) {
    return entry.second.browser && entry.second.browser_account == account;
  });
  if (account.empty() || count >= 4) {
    Error(id, "authentication_failed"); Drop(id, "browser-authentication-failed"); return;
  }
  connection.browser_account = std::move(account);
  connection.device = "web-" + RandomId().substr(0, 32);
  connection.nonce.clear();
  Send(id, Message("ready").Set("version", 1).Set("device_id", connection.device)
      .Set("heartbeat_seconds", 15).Set("idle_timeout_seconds", 45));
}

void Server::Authenticate(int id, const base::DictValue& message) {
  const auto* device = message.FindString("device_id");
  const auto* proof = message.FindString("proof");
  const auto* type = message.FindString("type");
  // Temporary browser identities can never authenticate on the native endpoint.
  auto configured = device && !device->starts_with("web-") ? Device(*device) : std::nullopt;
  std::vector<uint8_t> signature;
  const auto& connection = connections_.at(id);
  if (!type || *type != "authenticate" || message.FindInt("version") != 1 ||
      !configured || !proof || proof->size() != 64 ||
      !base::HexStringToBytes(*proof, &signature) ||
      base::TimeTicks::Now() - connection.opened > base::Seconds(10) ||
      !crypto::hmac::Verify(crypto::hash::kSha256, configured->secret,
          base::as_byte_span("crdesk-server-v1\n" + connection.nonce + "\n" + *device), signature)) {
    Error(id, "authentication_failed");
    Drop(id, "authentication-failed");
    return;
  }
  std::string client_instance;
  uint64_t generation = 0;
  if (message.contains("client_instance") || message.contains("connection_generation") || message.contains("reconnect_proof")) {
    const auto* instance = message.FindString("client_instance");
    const auto* sequence = message.FindString("connection_generation");
    const auto* reconnect_proof = message.FindString("reconnect_proof");
    std::vector<uint8_t> instance_bytes, reconnect_signature;
    if (!instance || instance->size() != 64 || !base::HexStringToBytes(*instance, &instance_bytes) ||
        *instance != base::ToLowerASCII(*instance) || !sequence || sequence->size() > 20 ||
        !base::StringToUint64(*sequence, &generation) || !generation || base::NumberToString(generation) != *sequence ||
        !reconnect_proof || reconnect_proof->size() != 64 || !base::HexStringToBytes(*reconnect_proof, &reconnect_signature) ||
        !crypto::hmac::Verify(crypto::hash::kSha256, configured->secret,
            base::as_byte_span("crdesk-reconnect-v1\n" + connection.nonce + "\n" + *device + "\n" + *instance + "\n" + *sequence), reconnect_signature)) {
      Error(id, "authentication_failed");
      Drop(id, "invalid-reconnect-proof");
      return;
    }
    client_instance = *instance;
  }
  int replaced = -1;
  if (const auto online = online_.find(*device); online != online_.end()) {
    const auto& previous = connections_.at(online->second);
    // Only a newer connection from the same running broker may take over.
    // Copied device configs and older, delayed handshakes must not kick it out.
    if (client_instance.empty() || previous.client_instance != client_instance ||
        generation <= previous.connection_generation) {
      Error(id, "already_online");
      Drop(id, "duplicate-device");
      return;
    }
    replaced = online->second;
  }
  connections_.at(id).device = *device;
  connections_.at(id).credential_tag = base::HexEncode(crypto::hash::Sha256(configured->secret));
  connections_.at(id).client_instance = std::move(client_instance);
  connections_.at(id).connection_generation = generation;
  connections_.at(id).nonce.clear();
  online_.insert_or_assign(*device, id);
  if (replaced != -1) {
    connections_.at(replaced).closing = true;
    LOG(INFO) << "crDeskServer: replacing cloud connection; device=" << *device
              << " generation=" << generation;
    // Install the new owner before closing the old socket. OnClose still ends
    // the old sessions, but cannot erase the new presence or mark it offline.
    http_->Close(replaced);
    if (!connections_.contains(id)) return;
  }
  if (accounts_) accounts_->SetOnline(*device, true, message.FindString("platform") ? *message.FindString("platform") : std::string());
  LOG(INFO) << "crDeskServer: device online; device=" << *device;
  auto ready = Message("ready");
  ready.Set("version", 1);
  auto features = base::ListValue().Append("connection_check").Append("account_tools").Append("fast_reconnect_v1");
  if (accounts_) { features.Append("avatars_v1"); features.Append("access_tokens_v1"); }
  ready.Set("features", std::move(features));
  const auto own_avatar = Device(*device);
  if (own_avatar) { ready.Set("avatar", own_avatar->avatar); ready.Set("avatar_hash", own_avatar->avatar_hash); }
  ready.Set("device_id", *device);
  ready.Set("heartbeat_seconds", 15);
  ready.Set("idle_timeout_seconds", 45);
  Send(id, std::move(ready));
  PublishPresence(*device);
}

std::optional<DeviceConfig> Server::Device(const std::string& id) {
  for (const auto& [connection_id, connection] : connections_) {
    if (connection.browser && !connection.device.empty() && connection.device == id) {
      DeviceConfig browser; browser.name = "Web browser"; browser.account = connection.browser_account;
      return browser;
    }
  }
  if (accounts_) return accounts_->Device(id);
  auto it = config_.devices.find(id);
  return it == config_.devices.end() ? std::nullopt : std::optional<DeviceConfig>(it->second);
}
std::vector<std::string> Server::Peers(const std::string& id) {
  for (const auto& [connection_id, connection] : connections_)
    if (accounts_ && connection.browser && !connection.device.empty() && connection.device == id)
      return accounts_->BrowserDevices(connection.browser_account);
  if (accounts_) return accounts_->Peers(id);
  auto device = Device(id); if (!device) return {};
  return {device->allowed_hosts.begin(), device->allowed_hosts.end()};
}
bool Server::Allowed(const std::string& viewer, const std::string& host) {
  if (viewer == host) return false;
  auto a = Device(viewer), b = Device(host);
  if (!a || !b) return false;
  return accounts_ ? !a->account.empty() && a->account == b->account : a->allowed_hosts.contains(host);
}
base::DictValue Server::DeviceStatus(const std::string& device) {
  auto status = base::DictValue();
  status.Set("id", device);
  auto info = Device(device);
  status.Set("name", info ? info->name : device);
  if (info) {
    status.Set("avatar", info->avatar); status.Set("avatar_hash", info->avatar_hash);
    status.Set("group", info->group); status.Set("note", info->note); status.Set("order", info->order);
    base::ListValue tags; for (const auto& tag : info->tags) tags.Append(tag);
    status.Set("tags", std::move(tags));
  }
  const auto online = online_.find(device);
  status.Set("online", online != online_.end());
  status.Set("sharing", online != online_.end() && connections_.at(online->second).sharing);
  return status;
}

void Server::PublishPresence(const std::string& device) {
  std::vector<int> recipients;
  for (const auto& [viewer, id] : online_) {
    if (Allowed(viewer, device)) recipients.push_back(id);
  }
  for (const auto& [id, connection] : connections_)
    if (connection.browser && !connection.device.empty() && Allowed(connection.device, device)) recipients.push_back(id);
  // Sending can synchronously close a slow connection. Never keep iterators
  // into connections_, online_, or sessions_ across socket writes.
  for (int id : recipients) {
    auto event = Message("device_changed");
    event.Set("device", DeviceStatus(device));
    Send(id, std::move(event));
  }
}

void Server::HandleMessage(int id, const base::DictValue& message) {
  const std::string type = *message.FindString("type");
  if (connections_.at(id).browser && type != "ping" && type != "list_devices" &&
      type != "connect" && type != "signal" && type != "close" && type != "ice_servers" && type != "session_report") {
    Error(id, "not_allowed"); return;
  }
  if (type == "ping") {
    Send(id, Message("pong"));
  } else if (type == "list_devices") {
    base::ListValue devices;
    for (const auto& target : Peers(connections_.at(id).device))
      devices.Append(DeviceStatus(target));
    auto response = Message("devices");
    response.Set("devices", std::move(devices));
    Send(id, std::move(response));
  } else if (type == "avatar_update") {
    auto& connection = connections_.at(id); const auto now = base::TimeTicks::Now();
    const auto index = message.FindInt("avatar"); const auto* hash = message.FindString("hash");
    const auto* data = message.FindString("data");
    bool ok = false;
    if (accounts_ && index && hash && data &&
        (connection.last_avatar.is_null() || now - connection.last_avatar >= base::Seconds(2))) {
      connection.last_avatar = now;
      ok = accounts_->SetAvatar(connection.device, *index, *hash, *data);
    }
    Send(id, Message("avatar_saved").Set("ok", ok));
  } else if (type == "avatar_get") {
    const auto* target = message.FindString("device"); const auto* hash = message.FindString("hash");
    if (!accounts_ || !target || !hash || !IsAvatarHash(*hash) || !Allowed(connections_.at(id).device, *target)) return;
    const auto device = Device(*target);
    if (!device || device->avatar_hash != *hash) return;
    Send(id, Message("avatar_data").Set("device", *target).Set("hash", *hash).Set("data", accounts_->AvatarData(*target, *hash)));
  } else if (type == "diagnostic_ice") {
    const auto* request = message.FindString("request_id");
    if (!RequestId(request)) { Error(id, "invalid_request"); return; }
    auto& connection = connections_.at(id);
    const auto now = base::TimeTicks::Now();
    if (!connection.last_check.is_null() && now - connection.last_check < base::Seconds(30)) {
      Error(id, "rate_limited", *request); return;
    }
    connection.last_check = now;
    SendIceServers(id, {}, *request);
  } else if (type == "diagnostic_report") {
    auto& connection = connections_.at(id);
    const auto now = base::TimeTicks::Now();
    if (!accounts_ || (!connection.last_report.is_null() && now - connection.last_report < base::Seconds(30))) return;
    const auto* rows = message.FindList("results");
    if (!rows || rows->size() > 16) return;
    base::ListValue clean;
    for (const auto& value : *rows) {
      if (!value.is_dict()) return;
      const auto& row = value.GetDict();
      const auto* stage = row.FindString("stage"); const auto* result = row.FindString("result");
      if (!stage || !result || stage->empty() || result->empty() || stage->size() > 32 || result->size() > 40 ||
          !base::ContainsOnlyChars(*stage, "abcdefghijklmnopqrstuvwxyz_") ||
          !base::ContainsOnlyChars(*result, "abcdefghijklmnopqrstuvwxyz_")) return;
      auto entry = base::DictValue().Set("stage", *stage).Set("result", *result);
      for (const char* key : {"probe", "count", "error_code", "net_error", "http_status", "relay_endpoints", "side0_error", "side1_error"})
        if (auto number = row.FindInt(key)) entry.Set(key, *number);
      clean.Append(std::move(entry));
    }
    connection.last_report = now;
    accounts_->SaveDiagnostics(connection.device, std::move(clean));
  } else if (type == "publish") {
    auto sharing = message.FindBool("sharing");
    if (!sharing) { Error(id, "invalid_request"); return; }
    const std::string device = connections_.at(id).device;
    const bool changed = connections_.at(id).sharing != *sharing;
    connections_.at(id).sharing = *sharing;
    std::vector<std::string> ended;
    if (!*sharing) {
      std::erase_if(access_grants_, [&](const auto& item) { return item.second.device == device; });
      for (const auto& [session_id, session] : sessions_) {
        if (session.host == id) ended.push_back(session_id);
      }
    }
    for (const auto& session_id : ended) EndSession(session_id, "sharing_stopped");
    auto response = Message("published");
    response.Set("sharing", *sharing);
    Send(id, std::move(response));
    if (changed) PublishPresence(device);
  } else if (type == "access_token_create" || type == "access_token_revoke") {
    AccessTokenMessage(id, type, message);
  } else if (type == "connect") {
    Connect(id, message);
  } else if (type == "accept" || type == "reject" || type == "signal" ||
             type == "close" || type == "ice_servers" || type == "session_report") {
    SessionMessage(id, type, message);
  } else {
    Error(id, "unknown_message");
  }
}

void Server::AccessTokenMessage(int id, const std::string& type, const base::DictValue& message) {
  auto& connection = connections_.at(id);
  if (type == "access_token_revoke") {
    const auto* token = message.FindString("token_id");
    auto it = token ? access_grants_.find(*token) : access_grants_.end();
    if (it != access_grants_.end() && it->second.device == connection.device) access_grants_.erase(it);
    return;
  }
  const auto* request = message.FindString("request_id");
  if (!RequestId(request)) { Error(id, "invalid_request"); return; }
  const int minutes = message.FindInt("minutes").value_or(0);
  if (!accounts_ || !connection.sharing || (minutes != 15 && minutes != 60 && minutes != 720 && minutes != 1440)) {
    Error(id, "not_allowed", *request); return;
  }
  if (connection.requests < 1) { Error(id, "rate_limited", *request); return; }
  connection.requests -= 1;
  const auto now = base::TimeTicks::Now();
  std::erase_if(access_grants_, [&](const auto& item) { return item.second.deadline <= now; });
  const auto count = std::ranges::count_if(access_grants_, [&](const auto& item) { return item.second.device == connection.device; });
  if (count >= 64 || access_grants_.size() >= 16384) { Error(id, "token_limit", *request); return; }
  std::string token;
  do { token = base::HexEncode(base::RandBytesAsString(16)); } while (access_grants_.contains(token));
  const auto secret = RandomId();
  access_grants_.emplace(token, AccessGrant{connection.device, connection.credential_tag,
      base::HexEncode(crypto::hash::Sha256(secret)), now + base::Minutes(minutes)});
  // The routing capability is independent of the end-to-end authentication
  // key, which is generated and consumed only by the sharing host.
  Send(id, Message("access_token_created").Set("request_id", *request).Set("id", token)
      .Set("secret", secret).Set("issued_ms", base::NumberToString(
          (base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds())));
}

void Server::Connect(int id, const base::DictValue& message) {
  const auto* request_id = message.FindString("request_id");
  const auto* target = message.FindString("target");
  if (!RequestId(request_id) || !target) { Error(id, "invalid_request"); return; }
  auto& connection = connections_.at(id);
  if (connection.requests < 1) { Error(id, "rate_limited", *request_id); return; }
  connection.requests -= 1;
  if (connection.browser && (!Allowed(connection.device, *target) ||
      message.contains("token_id") || message.contains("token_secret"))) {
    Error(id, "not_allowed", *request_id); return;
  }
  const auto* token_id = message.FindString("token_id");
  const auto* token_secret = message.FindString("token_secret");
  bool token_allowed = false;
  if (token_id && token_secret && token_secret->size() == 64 && connection.device != *target) {
    const auto grant = access_grants_.find(*token_id);
    const auto device = Device(*target);
    token_allowed = grant != access_grants_.end() && device && grant->second.device == *target &&
        grant->second.deadline > base::TimeTicks::Now() &&
        grant->second.credential_tag == base::HexEncode(crypto::hash::Sha256(device->secret)) &&
        grant->second.secret_hash == base::HexEncode(crypto::hash::Sha256(*token_secret));
  }
  if ((message.contains("token_id") || message.contains("token_secret")) ? !token_allowed : !Allowed(connection.device, *target)) {
    Error(id, "not_allowed", *request_id);
    return;
  }
  const auto host = online_.find(*target);
  if (host == online_.end() || !connections_.at(host->second).sharing ||
      connections_.at(host->second).closing) {
    Error(id, "unavailable", *request_id);
    return;
  }
  size_t viewer_sessions = 0;
  for (const auto& [session_id, session] : sessions_) {
    if (session.viewer == id) {
      ++viewer_sessions;
      if (session.request_id == *request_id) { Error(id, "duplicate_request", *request_id); return; }
    }
    if (session.host == host->second) { Error(id, "busy", *request_id); return; }
  }
  if (viewer_sessions >= 4) { Error(id, "session_limit", *request_id); return; }
  const int host_id = host->second;
  const std::string viewer = connection.device;
  std::string session_id;
  do { session_id = RandomId(); } while (sessions_.contains(session_id));
  Session session; session.viewer = id; session.host = host_id; session.request_id = *request_id;
  if (token_allowed) session.token_id = *token_id;
  auto stored = sessions_.emplace(session_id, std::move(session)).first;
  auto info = Device(viewer);
  stored->second.account = info ? info->account : std::string();
  stored->second.subject = (info ? info->name : viewer) + " → " + Device(*target)->name;
  if (accounts_) accounts_->RecordEvent(stored->second.account, "connection", "requested", stored->second.subject, {}, session_id);
  auto pending = Message("pending");
  pending.Set("session_id", session_id);
  pending.Set("request_id", *request_id);
  pending.Set("timeout_seconds", 30);
  Send(id, std::move(pending));
  if (!sessions_.contains(session_id)) return;
  auto incoming = Message("incoming");
  incoming.Set("session_id", session_id);
  incoming.Set("viewer", viewer);
  incoming.Set("name", Device(viewer)->name);
  incoming.Set("timeout_seconds", 30);
  Send(host_id, std::move(incoming));
}

void Server::SessionMessage(int id, const std::string& type, const base::DictValue& message) {
  const auto* supplied_id = message.FindString("session_id");
  auto it = supplied_id ? sessions_.find(*supplied_id) : sessions_.end();
  if (it == sessions_.end() || (it->second.host != id && it->second.viewer != id)) {
    Error(id, "unknown_session");
    return;
  }
  const std::string session_id = it->first;
  const Session session = it->second;
  if (!session.accepted && base::TimeTicks::Now() - session.created >= base::Seconds(30)) {
    EndSession(session_id, "timeout");
    return;
  }
  if (type == "close") {
    EndSession(session_id, "peer_closed");
  } else if (type == "accept" || type == "reject") {
    if (id != session.host || session.accepted) { Error(id, "invalid_state"); return; }
    if (type == "reject") { EndSession(session_id, "rejected"); return; }
    it->second.accepted = true;
    if (accounts_) accounts_->RecordEvent(session.account, "connection", "accepted", session.subject, {}, session_id);
    auto accepted = Message("accepted");
    accepted.Set("session_id", session_id);
    Send(session.viewer, accepted.Clone());
    if (sessions_.contains(session_id)) Send(session.host, std::move(accepted));
    LOG(INFO) << "crDeskServer: signaling session accepted";
  } else if (!session.accepted) {
    Error(id, "invalid_state");
  } else if (type == "session_report") {
    // Only the authenticated viewer can report its own connection. This is
    // client-reported evidence, not server verification of the media path.
    if (id != session.viewer) return;
    const auto* state = message.FindString("state");
    const auto* route = message.FindString("route");
    if (!state) return;
    if (*state == "failed") {
      const auto* reason = message.FindString("reason");
      if (!reason || session.reported_failure ||
          (*reason != "authentication" && *reason != "pairing_authentication" && *reason != "capture" &&
           *reason != "encoding" && *reason != "protocol" && *reason != "sdp" && *reason != "webrtc_init" &&
           *reason != "data_channel" && *reason != "signaling_timeout" && *reason != "ice" && *reason != "invalid_address_key")) return;
      it->second.reported_failure = true;
      if (accounts_) accounts_->RecordEvent(session.account, "connection", "failed", session.subject, "client_" + *reason, session_id);
      return;
    }
    if (*state != "connected" && *state != "disconnected") return;
    if (*state == "connected" && !it->second.connected) {
      it->second.connected = true;
      if (accounts_) accounts_->RecordEvent(session.account, "connection", "connected", session.subject, "client_report", session_id);
    }
    if (*state == "disconnected" && it->second.connected) {
      if (accounts_) accounts_->RecordEvent(session.account, "connection", "disconnected", session.subject, "client_report", session_id);
      EndSession(session_id, "peer_closed"); return;
    }
    if (route && (*route == "direct" || *route == "relay") && *route != it->second.route && it->second.route_changes < 8) {
      it->second.route = *route; ++it->second.route_changes;
      if (accounts_) accounts_->RecordEvent(session.account, "connection", *route, session.subject, "client_report", session_id);
    }
  } else if (type == "ice_servers") {
    SendIceServers(id, session_id);
  } else if (type == "signal") {
    const auto* payload = message.FindString("payload");
    if (!payload || payload->empty() || payload->size() > kMaxSignal) {
      Error(id, "invalid_payload");
      return;
    }
    if (id == session.viewer && !session.token_id.empty() && !session.token_hello) {
      const auto hello = base::JSONReader::ReadDict(*payload, base::JSON_PARSE_RFC);
      const auto* identity = hello ? hello->FindString("identity") : nullptr;
      const auto grant = access_grants_.find(session.token_id);
      if (!identity || *identity != "token:" + session.token_id || !hello->FindString("hello") ||
          grant == access_grants_.end() || grant->second.deadline <= base::TimeTicks::Now()) {
        EndSession(session_id, "rejected"); return;
      }
      it->second.token_hello = true;
    }
    auto signal = Message("signal");
    signal.Set("session_id", session_id);
    signal.Set("from", connections_.at(id).device);
    signal.Set("payload", *payload);
    // Adding the authenticated sender can make a near-limit message larger
    // than the incoming envelope. Reject the sender's payload, not the peer.
    const auto encoded = base::WriteJson(signal);
    if (!encoded || encoded->size() > kMaxMessage) {
      Error(id, "invalid_payload");
      return;
    }
    Send(session.host == id ? session.viewer : session.host, std::move(signal));
  }
}

void Server::SendIceServers(int id, const std::string& session_id, const std::string& check_id) {
  base::ListValue servers;
  if (!config_.stun_urls.empty()) {
    base::ListValue urls;
    for (const auto& url : config_.stun_urls) urls.Append(url);
    base::DictValue stun;
    stun.Set("urls", std::move(urls));
    servers.Append(std::move(stun));
  }
  if (!config_.turn_urls.empty()) {
    const auto expiry = base::Time::Now() + base::Seconds(config_.turn_ttl_seconds);
    const std::string username = base::NumberToString((expiry - base::Time::UnixEpoch()).InSeconds()) +
        ":" + connections_.at(id).device;
    // coturn's TURN REST authentication specifies HMAC-SHA1. This is only
    // for temporary relay credentials; device authentication uses SHA-256.
    const auto mac = crypto::hmac::SignSha1(base::as_byte_span(config_.turn_secret),
                                           base::as_byte_span(username));
    base::ListValue urls;
    for (const auto& url : config_.turn_urls) urls.Append(url);
    base::DictValue turn;
    turn.Set("urls", std::move(urls));
    turn.Set("username", username);
    turn.Set("credential", base::Base64Encode(mac));
    turn.Set("ttl_seconds", config_.turn_ttl_seconds);
    servers.Append(std::move(turn));
  }
  auto response = Message(check_id.empty() ? "ice_servers" : "diagnostic_ice");
  if (!check_id.empty()) response.Set("request_id", check_id);
  response.Set("session_id", session_id);
  response.Set("servers", std::move(servers));
  Send(id, std::move(response));
}

void Server::EndSession(const std::string& session_id, const std::string& reason) {
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return;
  const Session session = it->second;
  sessions_.erase(it);
  if (accounts_) accounts_->RecordEvent(session.account, "connection", "ended", session.subject, reason, session_id);
  auto closed = Message("closed");
  closed.Set("session_id", session_id);
  closed.Set("reason", reason);
  Send(session.viewer, closed.Clone());
  Send(session.host, std::move(closed));
}

void Server::Send(int id, base::DictValue message) {
  const auto it = connections_.find(id);
  if (it == connections_.end() || !it->second.websocket || it->second.closing) return;
  auto json = base::WriteJson(message);
  if (!json || json->size() > kMaxOutgoingMessage) { Drop(id, "outgoing-message-limit"); return; }
  http_->SendOverWebSocket(id, *json, kTraffic);
}

void Server::Error(int id, const std::string& code, const std::string& request_id) {
  if (accounts_ && !request_id.empty()) {
    auto connection = connections_.find(id);
    auto device = connection == connections_.end() ? std::nullopt : Device(connection->second.device);
    if (device && (connection->second.last_failure.is_null() ||
        base::TimeTicks::Now() - connection->second.last_failure >= base::Seconds(5))) {
      connection->second.last_failure = base::TimeTicks::Now();
      accounts_->RecordEvent(device->account, "connection", "failed", device->name, code);
    }
  }
  auto error = Message("error");
  error.Set("code", code);
  if (!request_id.empty()) error.Set("request_id", request_id);
  Send(id, std::move(error));
}

void Server::Drop(int id, const char* reason) {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.closing) return;
  it->second.closing = true;
  LOG(WARNING) << "crDeskServer: connection rejected; reason=" << reason;
  // HttpServer defers socket destruction, so stopping the read loop here is
  // safe. Do not let an already rejected peer continue processing its backlog.
  http_->Close(id);
}

void Server::OnClose(int id) {
  const auto it = connections_.find(id);
  if (it == connections_.end()) return;
  const std::string device = it->second.device;
  connections_.erase(it);
  const auto online = online_.find(device);
  const bool was_owner = online != online_.end() && online->second == id;
  if (was_owner) online_.erase(online);
  if (was_owner && accounts_) accounts_->SetOnline(device, false);
  std::vector<std::string> ended;
  for (const auto& [session_id, session] : sessions_) {
    if (session.host == id || session.viewer == id) ended.push_back(session_id);
  }
  for (const auto& session_id : ended) EndSession(session_id, "peer_offline");
  if (was_owner) {
    LOG(INFO) << "crDeskServer: device offline; device=" << device;
    PublishPresence(device);
  }
}

base::DictValue Server::AccountStatus(const std::string& account) {
  int online = 0, sessions = 0;
  for (const auto& [device, id] : online_) {
    auto info = Device(device);
    if (info && info->account == account) ++online;
  }
  for (const auto& [id, session] : sessions_) if (session.account == account) ++sessions;
  base::ListValue transports;
  for (const auto& url : config_.turn_urls) {
    const std::string type = url.starts_with("turns:") ? "TURN TLS" :
        url.find("transport=tcp") != std::string::npos ? "TURN TCP" : "TURN UDP";
    if (!transports.contains(type)) transports.Append(type);
  }
  return base::DictValue().Set("revision", "account-tools-v1")
      .Set("uptime", (base::TimeTicks::Now() - started_).InSecondsF())
      .Set("online", online).Set("sessions", sessions).Set("stun", !config_.stun_urls.empty())
      .Set("turn", std::move(transports)).Set("dependencies", dependency_status_.Clone());
}
void Server::ProbeDependencies() {
  if (!accounts_ || (!last_probe_.is_null() && base::TimeTicks::Now() - last_probe_ < base::Minutes(5))) return;
  last_probe_ = base::TimeTicks::Now();
  if (!config_.accounts.google_client_id.empty()) {
    google_probe_ = std::make_unique<CloudHttp>();
    google_probe_->Start("https://www.googleapis.com/oauth2/v3/certs", "", "",
        base::BindOnce(&Server::ProbeDone, weak_factory_.GetWeakPtr(), "google"));
  }
  https_probe_ = std::make_unique<CloudHttp>();
  https_probe_->Start(config_.accounts.origin + "/auth/", "", "",
      base::BindOnce(&Server::ProbeDone, weak_factory_.GetWeakPtr(), "https"));
}
void Server::ProbeDone(std::string name, int error, int status, std::string body) {
  auto& probe = name == "google" ? google_probe_ : https_probe_;
  auto row = base::DictValue().Set("time", base::Time::Now().InMillisecondsFSinceUnixEpochIgnoringNull())
      .Set("net_error", error).Set("http_status", status);
  bool valid = error == net::OK && status == 200;
  if (valid && name == "google") {
    auto json = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
    valid = json && json->FindList("keys") && !json->FindList("keys")->empty();
  }
  row.Set("ok", valid);
  if (probe && !probe->certificate_expiry().is_null())
    row.Set("certificate_expiry", probe->certificate_expiry().InMillisecondsFSinceUnixEpochIgnoringNull());
  dependency_status_.Set(name, std::move(row));
  probe.reset();
}

void Server::Tick() {
  if (accounts_) accounts_->Tick();
  ProbeDependencies();
  std::vector<int> revoked;
  if (accounts_) for (const auto& [id, connection] : connections_)
    if (connection.browser && accounts_->BrowserAccount(connection.browser_session).empty()) revoked.push_back(id);
  if (accounts_) for (const auto& [device, id] : online_) {
    const auto configured = Device(device);
    if (!configured || connections_.at(id).credential_tag != base::HexEncode(crypto::hash::Sha256(configured->secret))) revoked.push_back(id);
  }
  for (int id : revoked) http_->Close(id);
  const bool devices_changed = accounts_ && accounts_->TakeDeviceChanges();
  if (!revoked.empty() || devices_changed) {
    std::vector<int> viewers;
    for (const auto& [device, id] : online_) viewers.push_back(id);
    for (const auto& [id, connection] : connections_)
      if (connection.browser && !connection.device.empty()) viewers.push_back(id);
    for (int id : viewers) if (connections_.contains(id)) HandleMessage(id, Message("list_devices"));
  }
  const auto now = base::TimeTicks::Now();
  std::erase_if(access_grants_, [&](const auto& item) { return item.second.deadline <= now; });
  std::vector<int> expired;
  for (const auto& [id, connection] : connections_) {
    if ((connection.device.empty() && now - connection.opened >= (connection.closing ? base::Seconds(45) : base::Seconds(10))) ||
        (!connection.device.empty() && now - connection.last_message >= base::Seconds(45)))
      expired.push_back(id);
  }
  for (int id : expired) http_->Close(id);
  std::vector<std::string> pending;
  for (const auto& [session_id, session] : sessions_) {
    if (!session.accepted && now - session.created >= base::Seconds(30)) pending.push_back(session_id);
  }
  for (const auto& session_id : pending) EndSession(session_id, "timeout");
}
}  // namespace crdesk::server
