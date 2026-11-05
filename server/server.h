// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRDESK_SERVER_SERVER_H_
#define CRDESK_SERVER_SERVER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "crDesk/server/server_config.h"
#include "crDesk/server/account_auth.h"
#include "net/server/http_server.h"

namespace crdesk::server {

// All state and socket callbacks run on the main IO sequence. No shared
// database, worker process, or public administration endpoint is required.
class Server final : public net::HttpServer::Delegate {
 public:
  explicit Server(ServerConfig config);
  ~Server() override;
  int Start();

 private:
  struct Connection {
    bool websocket = false;
    bool sharing = false;
    bool closing = false;
    std::string nonce;
    std::string device;
    std::string credential_tag;
    std::string client_instance;
    uint64_t connection_generation = 0;
    base::TimeTicks opened = base::TimeTicks::Now();
    base::TimeTicks last_message = opened;
    base::TimeTicks budget_time = opened;
    base::TimeTicks last_check;
    base::TimeTicks last_report, last_failure, last_avatar;
    double messages = 128;
    double bytes = 512 * 1024;
    double requests = 6;
  };
  struct Session {
    int viewer;
    int host;
    std::string request_id;
    std::string token_id;
    bool token_hello = false;
    bool accepted = false;
    base::TimeTicks created = base::TimeTicks::Now();
    std::string account, subject;
    bool connected = false;
    bool reported_failure = false;
    std::string route;
    int route_changes = 0;
  };

  void OnConnect(int id) override;
  void OnHttpRequest(int id, const net::HttpServerRequestInfo& request) override;
  void OnWebSocketRequest(int id, const net::HttpServerRequestInfo& request) override;
  void OnWebSocketMessage(int id, std::string data) override;
  void OnClose(int id) override;
  void Authenticate(int id, const base::DictValue& message);
  void HandleMessage(int id, const base::DictValue& message);
  void Connect(int id, const base::DictValue& message);
  void AccessTokenMessage(int id, const std::string& type, const base::DictValue& message);
  void SessionMessage(int id, const std::string& type, const base::DictValue& message);
  void EndSession(const std::string& session_id, const std::string& reason);
  void SendIceServers(int id, const std::string& session_id, const std::string& check_id = {});
  void Send(int id, base::DictValue message);
  void Error(int id, const std::string& code, const std::string& request_id = {});
  void Drop(int id, const char* reason);
  void Tick();
  void PublishPresence(const std::string& device);
  base::DictValue DeviceStatus(const std::string& device);
  std::optional<DeviceConfig> Device(const std::string& id);
  std::vector<std::string> Peers(const std::string& id);
  bool Allowed(const std::string& viewer, const std::string& host);
  void ReplyHttp(int id, net::HttpServerResponseInfo response);
  base::DictValue AccountStatus(const std::string& account);
  void ProbeDependencies();
  void ProbeDone(std::string name, int error, int status, std::string body);

  const ServerConfig config_;
  std::map<int, Connection> connections_;
  std::map<std::string, int> online_;
  std::map<std::string, Session> sessions_;
  struct AccessGrant {
    std::string device, credential_tag, secret_hash;
    base::TimeTicks deadline;
  };
  std::map<std::string, AccessGrant> access_grants_;
  std::unique_ptr<AccountAuth> accounts_;
  std::unique_ptr<net::HttpServer> http_;
  base::RepeatingTimer timer_;
  base::TimeTicks started_ = base::TimeTicks::Now(), last_probe_;
  std::unique_ptr<CloudHttp> google_probe_, https_probe_;
  base::DictValue dependency_status_;
  base::WeakPtrFactory<Server> weak_factory_{this};
};
}  // namespace crdesk::server
#endif  // CRDESK_SERVER_SERVER_H_
