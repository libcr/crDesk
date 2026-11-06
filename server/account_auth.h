// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRDESK_SERVER_ACCOUNT_AUTH_H_
#define CRDESK_SERVER_ACCOUNT_AUTH_H_
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "crDesk/server/account_store.h"
#include "crDesk/server/server_config.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
namespace crdesk { class CloudHttp; }
namespace crdesk::server {
class AccountAuth {
 public:
  using Reply = base::OnceCallback<void(net::HttpServerResponseInfo)>;
  explicit AccountAuth(AccountConfig config);
  ~AccountAuth();
  bool Start();
  void Handle(const net::HttpServerRequestInfo& request, Reply reply);
  void Tick();
  // Browser viewers are temporary identities, never enrolled device secrets.
  std::string BrowserSessionHash(const net::HttpServerRequestInfo& request) const;
  std::string BrowserAccount(const std::string& session_hash);
  std::string ConsumeRemoteTicket(const std::string& ticket, const std::string& session_hash);
  std::vector<std::string> BrowserDevices(const std::string& account);
  std::optional<DeviceConfig> Device(const std::string& id);
  std::vector<std::string> Peers(const std::string& device);
  void SetOnline(const std::string& device, bool online, std::string platform = {});
  bool TakeDeviceChanges();
  void RecordEvent(const std::string& account, const std::string& category,
                   const std::string& action, const std::string& subject = {},
                   const std::string& detail = {}, const std::string& session = {});
  void SetStatusProvider(base::RepeatingCallback<base::DictValue(const std::string&)> provider);
  bool SetAvatar(const std::string& device, int index, const std::string& hash, const std::string& data);
  std::string AvatarData(const std::string& device, const std::string& hash);
  void SaveDiagnostics(const std::string& device, base::ListValue results);
 private:
  struct Flow {
    std::string id, challenge, name, cookie, csrf, account, email;
    std::string approved_account, confirmation;
    std::string current_device, login_method, session_hash;
    bool management = false;
    base::ListValue login_failures;
    std::string identity_key;
    bool identity_verified = false;
    bool restoring = false;
    bool revoked = false;
    std::string oauth_state, oauth_nonce, oauth_verifier, passkey_challenge, passkey_operation;
    std::string passkey_account, passkey_name;
    bool approved = false;
    bool claimed = false;
    base::TimeTicks expires;
    base::TimeTicks last_poll;
    base::TimeTicks passkey_expires;
    std::unique_ptr<CloudHttp> http;
  };
  void GoogleCallback(const net::HttpServerRequestInfo&, Flow&, Reply);
  void GoogleToken(std::string flow, Reply, int error, int status, std::string body);
  void GoogleKeys(std::string flow, Reply, std::string token, int error, int status, std::string body);
  base::DictValue PasskeyOptions(Flow&, const std::string& operation);
  bool PasskeyFinish(Flow&, const base::DictValue&, const std::string& operation);
  Flow* BrowserFlow(const net::HttpServerRequestInfo& request, const std::string& id = {});
  std::string RememberBrowser(Flow& flow);
  bool AllowStart(bool new_flow = true);
  bool HasGoogleLogin(const std::string& account);
  std::string DeviceId(const Flow& flow) const;
  void RestoreDevice(Flow& flow);
  void LoginFailure(Flow& flow, std::string reason);
  void FlushLoginFailures(Flow& flow);
  struct RemoteTicket {
    std::string session_hash;
    base::TimeTicks expires;
  };
  std::map<std::string, RemoteTicket> remote_tickets_;
  AccountConfig config_;
  AccountStore store_;
  std::map<std::string, Flow> flows_;
  std::map<std::string, base::TimeTicks> online_;
  base::TimeTicks last_start_;
  double start_budget_ = 20;
  bool devices_changed_ = false;
  base::TimeTicks last_prune_;
  base::RepeatingCallback<base::DictValue(const std::string&)> status_provider_;
  base::WeakPtrFactory<AccountAuth> weak_{this};
};
}
#endif
