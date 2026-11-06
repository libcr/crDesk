// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crDesk/server/account_auth.h"
#include "crDesk/avatar_format.h"
#include <algorithm>
#include <string_view>
#include <utility>
#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "components/cbor/reader.h"
#include "crDesk/cloud_http.h"
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "crypto/sign.h"
#include "device/fido/attestation_object.h"
#include "device/fido/public_key.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
namespace crdesk::server {
namespace {
constexpr std::string_view kLoginCookiePrefix = "__Secure-crdesk-login=";
constexpr std::string_view kAccountCookiePrefix = "__Secure-crdesk-account=";
std::string Cookie(const net::HttpServerRequestInfo& request, std::string_view prefix) {
  std::string cookie;
  bool found = false;
  for (const auto& part : base::SplitString(request.GetHeaderValue("cookie"), ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (!part.starts_with(prefix)) continue;
    if (found) return {};
    found = true; cookie = part.substr(prefix.size());
  }
  return cookie.size() == 43 && base::ContainsOnlyChars(cookie, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ? cookie : std::string();
}
void AccountCookie(net::HttpServerResponseInfo& response, const std::string& token, bool clear = false) {
  if (clear || !token.empty()) response.AddHeader("Set-Cookie", std::string(kAccountCookiePrefix) + token +
      "; Path=/auth/; Secure; HttpOnly; SameSite=Lax; Max-Age=" + (clear ? "0" : "2592000"));
}
std::string Field(const base::DictValue& value, std::string_view name) {
  const auto* text = value.FindString(name); return text ? *text : std::string();
}
std::string Encode(std::string_view bytes) {
  std::string result; base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING, &result); return result;
}
std::string Decode(std::string_view bytes) {
  std::string result;
  if (bytes.size() > 32768 || !base::Base64UrlDecode(bytes, base::Base64UrlDecodePolicy::DISALLOW_PADDING, &result)) return {};
  return result;
}
std::string Random() { return Encode(base::RandBytesAsString(32)); }
double Timestamp() { return base::Time::Now().InMillisecondsFSinceUnixEpochIgnoringNull(); }
std::string Digest(std::string_view text) {
  auto hash = crypto::hash::Sha256(text); return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
}
std::string Param(const GURL& url, const char* key) { std::string result; net::GetValueForKeyInQuery(url, key, &result); return result; }
std::string Escape(std::string_view text) { return base::EscapeUrlEncodedData(text, true); }
// Only log known protocol errors, never response bodies or caller-supplied text.
std::string_view SafeGoogleError(std::string_view error) {
  for (std::string_view known : {"access_denied", "invalid_request", "invalid_client", "invalid_grant", "unauthorized_client", "unsupported_grant_type", "invalid_scope", "server_error", "temporarily_unavailable"}) {
    if (error == known) return known;
  }
  return error.empty() ? "none" : "other";
}
void Json(AccountAuth::Reply reply, base::DictValue body, net::HttpStatusCode code = net::HTTP_OK, std::string cookie = {}, bool clear_cookie = false, std::string account_cookie = {}) {
  net::HttpServerResponseInfo response(code);
  response.AddHeader("Cache-Control", "no-store"); response.AddHeader("X-Content-Type-Options", "nosniff");
  response.AddHeader("Referrer-Policy", "no-referrer");
  if (clear_cookie) response.AddHeader("Set-Cookie", std::string(kLoginCookiePrefix) + "; Path=/auth/; Secure; HttpOnly; SameSite=Lax; Max-Age=0");
  else if (!cookie.empty()) response.AddHeader("Set-Cookie", std::string(kLoginCookiePrefix) + cookie + "; Path=/auth/; Secure; HttpOnly; SameSite=Lax; Max-Age=600");
  AccountCookie(response, account_cookie, clear_cookie);
  response.SetBody(base::WriteJson(body).value_or("{}"), "application/json"); std::move(reply).Run(std::move(response));
}
void Error(AccountAuth::Reply reply, const char* reason, net::HttpStatusCode code = net::HTTP_BAD_REQUEST) {
  Json(std::move(reply), base::DictValue().Set("error", reason), code);
}
void Redirect(AccountAuth::Reply reply, const std::string& location, const std::string& account_cookie = {}, const std::string& flow_cookie = {}) {
  net::HttpServerResponseInfo response(net::HTTP_SEE_OTHER);
  AccountCookie(response, account_cookie);
  if (!flow_cookie.empty()) response.AddHeader("Set-Cookie", std::string(kLoginCookiePrefix) + flow_cookie + "; Path=/auth/; Secure; HttpOnly; SameSite=Lax; Max-Age=600");
  response.AddHeader("Location", location); response.AddHeader("Cache-Control", "no-store");
  response.AddHeader("Referrer-Policy", "no-referrer"); response.SetBody("", "text/plain"); std::move(reply).Run(std::move(response));
}
uint32_t Counter(const device::AuthenticatorData& auth) {
  uint32_t count = 0; for (uint8_t byte : auth.counter()) count = (count << 8) | byte; return count;
}
bool ValidAuth(const device::AuthenticatorData& auth, const std::string& rp) {
  return auth.obtained_user_presence() && auth.obtained_user_verification() &&
      auth.application_parameter() == crypto::hash::Sha256(rp) && (!auth.backup_state() || auth.backup_eligible());
}
bool ValidName(const std::string& name) {
  return !name.empty() && name.size() <= 64 && base::IsStringUTF8(name) &&
      std::ranges::none_of(name, [](unsigned char c) { return c < 32 || c == 127; });
}
}
AccountAuth::AccountAuth(AccountConfig config) : config_(std::move(config)) {}
AccountAuth::~AccountAuth() = default;
bool AccountAuth::Start() { return store_.Open(config_.database); }
void AccountAuth::Tick() {
  const auto now = base::TimeTicks::Now();
  std::erase_if(remote_tickets_, [&](const auto& item) { return item.second.expires <= now; });
  if (last_prune_.is_null() || now - last_prune_ >= base::Hours(1)) {
    store_.PruneEvents(); store_.PruneBrowserSessions(); last_prune_ = now;
  }
  std::erase_if(flows_, [now](const auto& entry) { return entry.second.expires <= now; });
  // Persist presence at most once a minute, rather than on every heartbeat.
  // After a server restart all devices are offline until authenticated again.
  for (auto& [id, saved] : online_) {
    if (now - saved < base::Minutes(1)) continue;
    auto row = store_.Get("device", id);
    if (row) {
      row->Set("last_seen", Timestamp());
      store_.Put("device", id, Field(*row, "account"), *row);
    }
    saved = now;
  }
}
void AccountAuth::SetOnline(const std::string& id, bool online, std::string platform) {
  if (online) online_[id] = base::TimeTicks::Now();
  else online_.erase(id);
  auto row = store_.Get("device", id);
  if (!row) return;
  row->Set("last_seen", Timestamp());
  if (platform == "Windows" || platform == "macOS" || platform == "Linux") row->Set("platform", platform);
  store_.Put("device", id, Field(*row, "account"), *row);
}
void AccountAuth::LoginFailure(Flow& flow, std::string reason) {
  // Never attribute an unverified email, device hint or credential ID to an
  // account. Retain failures only in this browser flow until it authenticates.
  if (!flow.account.empty()) RecordEvent(flow.account, "account", "failed", {}, reason);
  else if (flow.login_failures.size() < 8) flow.login_failures.Append(base::DictValue()
      .Set("category", "account").Set("action", "failed").Set("detail", std::move(reason)).Set("time", Timestamp()));
}
void AccountAuth::FlushLoginFailures(Flow& flow) {
  for (const auto& event : flow.login_failures) store_.AppendEvent(flow.account, event.GetDict().Clone());
  flow.login_failures.clear();
}
void AccountAuth::RecordEvent(const std::string& account, const std::string& category,
                              const std::string& action, const std::string& subject,
                              const std::string& detail, const std::string& session) {
  if (account.empty()) return;
  store_.AppendEvent(account, base::DictValue().Set("category", category).Set("action", action)
      .Set("subject", subject).Set("detail", detail).Set("session", session));
}
void AccountAuth::SetStatusProvider(base::RepeatingCallback<base::DictValue(const std::string&)> provider) {
  status_provider_ = std::move(provider);
}
bool AccountAuth::SetAvatar(const std::string& device, int index, const std::string& hash, const std::string& data) {
  auto row = store_.Get("device", device); std::string jpeg;
  if (!row || index < 0 || index > 8 || (!hash.empty() && !DecodeAvatar(hash, data, &jpeg)) ||
      (hash.empty() && !data.empty())) return false;
  const auto account = Field(*row, "account");
  base::DictValue avatar;
  if (!hash.empty()) { avatar.Set("hash", hash); avatar.Set("data", data); }
  row->Set("avatar", index); row->Set("avatar_hash", hash);
  if (!store_.PutDeviceAvatar(device, account, *row, avatar)) return false;
  devices_changed_ = true; return true;
}
std::string AccountAuth::AvatarData(const std::string& device, const std::string& hash) {
  if (!IsAvatarHash(hash)) return {};
  auto row = store_.Get("avatar", device);
  return row && Field(*row, "hash") == hash ? Field(*row, "data") : std::string();
}
void AccountAuth::SaveDiagnostics(const std::string& device, base::ListValue results) {
  auto row = store_.Get("device", device);
  if (!row) return;
  store_.Put("diagnostic", device, Field(*row, "account"),
      base::DictValue().Set("device", device).Set("time", Timestamp()).Set("results", std::move(results)));
}
bool AccountAuth::AllowStart(bool new_flow) {
  const auto now = base::TimeTicks::Now();
  if (!last_start_.is_null()) start_budget_ = std::min(20.0, start_budget_ + (now - last_start_).InSecondsF() / 3);
  last_start_ = now;
  if (start_budget_ < 1 || (new_flow && flows_.size() >= 128)) return false;
  start_budget_ -= 1;
  return true;
}
bool AccountAuth::HasGoogleLogin(const std::string& account) {
  return !config_.google_client_id.empty() && !store_.List("google", account).empty();
}
bool AccountAuth::TakeDeviceChanges() { return std::exchange(devices_changed_, false); }
std::string AccountAuth::DeviceId(const Flow& flow) const {
  // Account-scoped identity: one record per installation and account, even
  // when two authorized login flows finish at the same time.
  return base::ToLowerASCII(base::HexEncode(Digest("crdesk-device-v1\n" + flow.account + "\n" + flow.identity_key))).substr(0, 32);
}
void AccountAuth::RestoreDevice(Flow& flow) {
  if (flow.management || flow.revoked || !flow.identity_verified || flow.account.empty()) return;
  auto row = store_.Get("device", DeviceId(flow));
  if (!row || Field(*row, "account") != flow.account || Field(*row, "identity_key") != flow.identity_key) return;
  flow.restoring = true; flow.approved = true;
  flow.approved_account = flow.account;
  flow.current_device = Field(*row, "id");
  flow.name = Field(*row, "name");
}
std::optional<DeviceConfig> AccountAuth::Device(const std::string& id) {
  auto row = store_.Get("device", id); if (!row) return std::nullopt;
  DeviceConfig device;
  device.name = Field(*row, "name"); device.account = Field(*row, "account");
  device.avatar = row->FindInt("avatar").value_or(0); device.avatar_hash = Field(*row, "avatar_hash");
  device.group = Field(*row, "group"); device.note = Field(*row, "note");
  device.order = row->FindInt("order").value_or(0);
  if (const auto* tags = row->FindList("tags")) for (const auto& tag : *tags)
    if (tag.is_string()) device.tags.push_back(tag.GetString());
  if (device.account.empty() || !base::HexStringToBytes(Field(*row, "secret"), &device.secret) || device.secret.size() != 32) return std::nullopt;
  return device;
}
std::vector<std::string> AccountAuth::Peers(const std::string& id) {
  std::vector<std::string> peers; auto device = Device(id); if (!device) return peers;
  for (const auto& item : store_.List("device", device->account)) {
    const auto target = Field(item.GetDict(), "id"); if (target != id) peers.push_back(target);
  }
  return peers;
}
std::string AccountAuth::BrowserSessionHash(const net::HttpServerRequestInfo& request) const {
  const auto token = Cookie(request, kAccountCookiePrefix);
  return token.empty() ? std::string() : Encode(Digest(config_.origin + "\n" + token));
}
std::string AccountAuth::RememberBrowser(Flow& flow) {
  const auto token = Random();
  const auto hash = Encode(Digest(config_.origin + "\n" + token));
  if (!store_.CreateBrowserSession(hash, flow.account, flow.login_method)) {
    flow.account.clear(); flow.email.clear(); flow.login_method.clear();
    return {};
  }
  flow.session_hash = hash;
  // Authentication rotates the short-lived browser binding and CSRF token.
  flow.cookie = Random(); flow.csrf = Random();
  return token;
}
AccountAuth::Flow* AccountAuth::BrowserFlow(const net::HttpServerRequestInfo& request, const std::string& requested) {
  const auto cookie = Cookie(request, kLoginCookiePrefix);
  const auto hash = BrowserSessionHash(request);
  for (auto& [id, flow] : flows_) {
    if ((!requested.empty() && requested != id) || flow.expires <= base::TimeTicks::Now()) continue;
    const bool remembered = !hash.empty() && hash == flow.session_hash;
    if ((cookie.empty() || flow.cookie != cookie) && (requested.empty() || !remembered)) continue;
    if (!flow.account.empty()) {
      const auto session = remembered ? store_.BrowserSession(hash) : std::nullopt;
      if (!session || Field(*session, "account") != flow.account) return nullptr;
    }
    return &flow;
  }
  return nullptr;
}
std::string AccountAuth::BrowserAccount(const std::string& session_hash) {
  const auto session = store_.BrowserSession(session_hash);
  return session ? Field(*session, "account") : std::string();
}
std::string AccountAuth::ConsumeRemoteTicket(const std::string& ticket, const std::string& session_hash) {
  if (ticket.size() != 43 || session_hash.empty()) return {};
  auto it = remote_tickets_.find(Encode(Digest(ticket)));
  if (it == remote_tickets_.end() || it->second.expires <= base::TimeTicks::Now() ||
      it->second.session_hash != session_hash) return {};
  remote_tickets_.erase(it);
  return BrowserAccount(session_hash);
}
std::vector<std::string> AccountAuth::BrowserDevices(const std::string& account) {
  std::vector<std::string> result;
  if (!account.empty()) for (const auto& item : store_.List("device", account))
    result.push_back(Field(item.GetDict(), "id"));
  return result;
}
void AccountAuth::Handle(const net::HttpServerRequestInfo& request, Reply reply) {
  Tick();
  const GURL url(config_.origin + request.path);
  const std::string path(url.path());
  if (!request.peer.address().IsLoopback() || request.data.size() > 48 * 1024) { Error(std::move(reply), "invalid_request"); return; }
  if (request.method == "GET" && path == "/auth/google/callback") {
    auto* flow = BrowserFlow(request);
    if (!flow) {
      LOG(WARNING) << "crDeskServer: Google login failed; stage=callback reason=missing-or-expired-browser-session";
      Redirect(std::move(reply), "/auth/?error=expired"); return;
    }
    GoogleCallback(request, *flow, std::move(reply)); return;
  }
  if (request.method != "POST" || !request.GetHeaderValue("content-type").starts_with("application/json")) {
    Error(std::move(reply), "invalid_request"); return;
  }
  auto body = base::JSONReader::ReadDict(request.data, base::JSON_PARSE_RFC);
  if (!body) { Error(std::move(reply), "invalid_request"); return; }
  const bool native = path == "/auth/device/start" || path == "/auth/device/prove" || path == "/auth/device/poll" || path == "/auth/device/confirm";
  const auto origin = request.GetHeaderValue("origin");
  if (native ? !origin.empty() : origin != config_.origin) { Error(std::move(reply), "invalid_origin", net::HTTP_FORBIDDEN); return; }
  if (path == "/auth/device/start") {
    auto now = base::TimeTicks::Now();
    if (!AllowStart()) { Error(std::move(reply), "rate_limited", net::HTTP_TOO_MANY_REQUESTS); return; }
    if (Decode(Field(*body, "challenge")).size() != 32 || !ValidName(Field(*body, "name"))) { Error(std::move(reply), "invalid_request"); return; }
    const auto identity = Decode(Field(*body, "identity_key"));
    auto public_key = crypto::keypair::PublicKey::FromSubjectPublicKeyInfo(base::as_byte_span(identity));
    if (!public_key || !public_key->IsEd25519()) { Error(std::move(reply), "invalid_request"); return; }
    const auto canonical = public_key->ToSubjectPublicKeyInfo();
    if (identity != std::string(canonical.begin(), canonical.end()) || Encode(identity) != Field(*body, "identity_key")) { Error(std::move(reply), "invalid_request"); return; }
    Flow flow; flow.id = Random(); flow.challenge = Field(*body, "challenge"); flow.name = Field(*body, "name");
    flow.identity_key = Field(*body, "identity_key");
    flow.expires = now + base::Minutes(10);
    auto response = base::DictValue().Set("flow", flow.id).Set("native_confirmation", true)
        .Set("login_url", config_.origin + "/auth/?flow=" + flow.id).Set("expires_in", 600);
    flows_.emplace(flow.id, std::move(flow)); Json(std::move(reply), std::move(response)); return;
  }
  if (path == "/auth/device/prove") {
    auto it = flows_.find(Field(*body, "flow"));
    if (it == flows_.end() || it->second.management || it->second.identity_verified || it->second.revoked ||
        Field(*body, "verifier").size() != 43 || Encode(Digest(Field(*body, "verifier"))) != it->second.challenge) {
      Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return;
    }
    auto& flow = it->second;
    auto key = crypto::keypair::PublicKey::FromSubjectPublicKeyInfo(base::as_byte_span(Decode(flow.identity_key)));
    const auto transcript = "crdesk-device-identity-v1\n" + config_.origin + "\n" + flow.id + "\n" + flow.challenge;
    const auto signature = Decode(Field(*body, "signature"));
    if (!key || !key->IsEd25519() || signature.size() != 64 ||
        !crypto::sign::Verify(crypto::sign::ED25519, *key, base::as_byte_span(transcript), base::as_byte_span(signature))) {
      Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return;
    }
    flow.identity_verified = true;
    Json(std::move(reply), base::DictValue().Set("identity_verified", true)); return;
  }
  if (path == "/auth/device/poll" || path == "/auth/device/confirm") {
    const bool confirm = path == "/auth/device/confirm";
    auto it = flows_.find(Field(*body, "flow"));
    if (it == flows_.end() || it->second.management || Field(*body, "verifier").size() != 43 || Encode(Digest(Field(*body, "verifier"))) != it->second.challenge) {
      Error(std::move(reply), "expired", net::HTTP_UNAUTHORIZED); return;
    }
    auto& flow = it->second;
    if (!flow.identity_verified || flow.revoked) { Error(std::move(reply), "device_revoked", net::HTTP_FORBIDDEN); return; }
    if (!confirm) {
      if (!flow.last_poll.is_null() && base::TimeTicks::Now() - flow.last_poll < base::Seconds(2)) { Error(std::move(reply), "rate_limited", net::HTTP_TOO_MANY_REQUESTS); return; }
      flow.last_poll = base::TimeTicks::Now();
    }
    if (!flow.approved) {
      if (confirm) { Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return; }
      Json(std::move(reply), base::DictValue().Set("status", "pending")); return;
    }
    // Redemption is one-shot. A lost response requires another explicit login;
    // no reusable bearer device secret is exposed to the browser.
    if (flow.claimed) { Error(std::move(reply), "expired"); return; }
    if (flow.account.empty()) { Error(std::move(reply), "sign_in_required", net::HTTP_UNAUTHORIZED); return; }
    const auto session = store_.BrowserSession(flow.session_hash);
    if (!session || Field(*session, "account") != flow.account) { Error(std::move(reply), "expired", net::HTTP_UNAUTHORIZED); return; }
    if (flow.approved_account != flow.account) { Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return; }
    if (!flow.restoring) {
      if (flow.confirmation.empty()) { Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return; }
      if (!confirm) {
        // Only the initiating native client can read this challenge. Browser
        // approval alone must never create a device or issue its credential.
        Json(std::move(reply), base::DictValue().Set("status", "confirmation")
            .Set("account", flow.account).Set("email", flow.email).Set("confirmation", flow.confirmation)); return;
      }
      if (Field(*body, "account") != flow.approved_account || Field(*body, "confirmation") != flow.confirmation) {
        Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return;
      }
    } else if (confirm) { Error(std::move(reply), "invalid_request", net::HTTP_FORBIDDEN); return; }
    const auto id = DeviceId(flow);
    auto existing = store_.Get("device", id);
    if ((flow.restoring && !existing) || (existing &&
        (Field(*existing, "account") != flow.account || Field(*existing, "identity_key") != flow.identity_key))) {
      Error(std::move(reply), "device_revoked", net::HTTP_FORBIDDEN); return;
    }
    if (!existing && store_.List("device", flow.account).size() >= 128) { Error(std::move(reply), "device_limit"); return; }
    auto secret = base::HexEncode(base::RandBytesAsString(32));
    auto record = existing ? existing->Clone() : base::DictValue().Set("id", id).Set("account", flow.account)
        .Set("name", flow.name).Set("identity_key", flow.identity_key).Set("created", Timestamp());
    record.Set("secret", secret); record.Set("last_login", Timestamp());
    if (!store_.Put("device", id, flow.account, record, !existing)) { Error(std::move(reply), "storage_failed"); return; }
    devices_changed_ = true;
    RecordEvent(flow.account, "device", existing ? "restored" : "registered", Field(record, "name"));
    flow.claimed = true;
    flow.current_device = id;
    const auto wss = "wss" + config_.origin.substr(5) + "/v1/ws";
    Json(std::move(reply), base::DictValue().Set("status", "complete").Set("restoring", flow.restoring).Set("url", wss)
        .Set("device_id", id).Set("secret", secret).Set("account", flow.account).Set("email", flow.email)); return;
  }
  if (path == "/auth/browser/start") {
    auto requested = Field(*body, "flow");
    auto* previous = BrowserFlow(request, requested);
    // This hint can only create a management context, never revive enrollment.
    if (!requested.empty() && body->FindBool("management").value_or(false)) {
      auto existing = flows_.find(requested);
      if (existing == flows_.end() || (existing->second.management && !previous)) requested.clear();
    }
    if (requested.empty()) {
      if (previous && previous->management) requested = previous->id;
      else {
        if (!AllowStart()) { Error(std::move(reply), "rate_limited", net::HTTP_TOO_MANY_REQUESTS); return; }
        Flow fresh; fresh.id = Random(); fresh.management = true;
        fresh.expires = base::TimeTicks::Now() + base::Minutes(10);
        requested = fresh.id;
        flows_.emplace(requested, std::move(fresh));
      }
    }
    auto it = flows_.find(requested);
    if (it == flows_.end()) { Error(std::move(reply), "expired"); return; }
    auto& flow = it->second;
    if (!flow.management && (!flow.identity_verified || flow.revoked)) { Error(std::move(reply), "device_revoked"); return; }
    if (!flow.cookie.empty() && previous != &flow) { Error(std::move(reply), "already_open"); return; }
    if (flow.cookie.empty()) { flow.cookie = Random(); flow.csrf = Random(); }
    if (flow.account.empty() && !flow.http && flow.passkey_challenge.empty() && flow.oauth_state.empty()) {
      const auto hash = BrowserSessionHash(request);
      const auto session = store_.BrowserSession(hash);
      if (session) {
        flow.account = Field(*session, "account"); flow.login_method = Field(*session, "method"); flow.session_hash = hash;
        const auto profile = store_.Get("account", flow.account);
        flow.email = profile ? Field(*profile, "email") : std::string();
        RestoreDevice(flow);
      }
    }
    // Renew only the disposable management context, not the account session
    // or the native enrollment deadline.
    if (flow.management) flow.expires = base::TimeTicks::Now() + base::Minutes(10);
    // This is only a UI marker, checked against the authenticated account's
    // device list below. It never grants access or approves a registration.
    const auto current = Field(*body, "device");
    if (flow.management && current.size() == 32 && base::ContainsOnlyChars(current, "0123456789abcdef")) flow.current_device = current;
    Json(std::move(reply), base::DictValue().Set("csrf", flow.csrf).Set("name", flow.name)
        .Set("flow", flow.id).Set("management", flow.management).Set("login_method", flow.login_method).Set("restoring", flow.restoring)
        .Set("account", flow.account).Set("email", flow.email).Set("signed_in", !flow.account.empty()).Set("approved", flow.approved).Set("claimed", flow.claimed)
        .Set("google", !config_.google_client_id.empty()), net::HTTP_OK, flow.cookie); return;
  }
  auto* flow = BrowserFlow(request, Field(*body, "flow"));
  if (!flow || Field(*body, "csrf") != flow->csrf) { Error(std::move(reply), "expired", net::HTTP_FORBIDDEN); return; }
  if (path == "/auth/browser/logout") {
    const auto id = flow->id, hash = BrowserSessionHash(request);
    if (!hash.empty() && !store_.RemoveBrowserSession(hash)) { Error(std::move(reply), "storage_failed"); return; }
    RecordEvent(flow->account, "account", "signedOut");
    // Revoke every pending approval and browser tab bound to this login.
    std::erase_if(flows_, [&](const auto& entry) { return entry.first == id || (!hash.empty() && entry.second.session_hash == hash); });
    // Invalidate the server-side session; replace the browser cookie as well.
    Json(std::move(reply), base::DictValue().Set("signed_out", true), net::HTTP_OK, {}, true); return;
  }
  if (path == "/auth/google/start") {
    if (config_.google_client_id.empty() || flow->http || !flow->account.empty()) { Error(std::move(reply), "unavailable"); return; }
    flow->passkey_challenge.clear(); flow->passkey_operation.clear();
    flow->passkey_account.clear(); flow->passkey_name.clear();
    flow->oauth_state = Random(); flow->oauth_nonce = Random(); flow->oauth_verifier = Random();
    auto location = "https://accounts.google.com/o/oauth2/v2/auth?response_type=code&scope=openid%20email&prompt=select_account&client_id=" + Escape(config_.google_client_id) +
        "&redirect_uri=" + Escape(config_.origin + "/auth/google/callback") + "&state=" + flow->oauth_state + "&nonce=" + flow->oauth_nonce +
        "&code_challenge_method=S256&code_challenge=" + Encode(Digest(flow->oauth_verifier));
    Json(std::move(reply), base::DictValue().Set("url", location)); return;
  }
  if (path == "/auth/passkey/options" || path == "/auth/passkey/finish") {
    const auto operation = Field(*body, "operation");
    const bool registering = operation == "register", create = operation != "get";
    if (flow->http || (operation != "create" && operation != "get" && !registering)) { Error(std::move(reply), "invalid_request"); return; }
    if ((operation == "create") != !flow->account.empty()) { Error(std::move(reply), "invalid_request"); return; }
    if (operation == "create" && store_.List("passkey", flow->account).size() >= 32) { Error(std::move(reply), "passkey_limit"); return; }
    if (path.ends_with("options")) {
      if (registering && !ValidName(Field(*body, "name"))) { Error(std::move(reply), "invalid_name"); return; }
      if (!AllowStart(false)) { Error(std::move(reply), "rate_limited", net::HTTP_TOO_MANY_REQUESTS); return; }
      flow->oauth_state.clear(); flow->oauth_nonce.clear(); flow->oauth_verifier.clear();
      flow->passkey_account = registering ? base::HexEncode(base::RandBytesAsString(32)) : std::string();
      flow->passkey_name = registering ? Field(*body, "name") : std::string();
      Json(std::move(reply), PasskeyOptions(*flow, operation)); return;
    }
    if (!PasskeyFinish(*flow, *body, operation)) { LoginFailure(*flow, "passkey_verification"); Error(std::move(reply), "passkey_failed"); return; }
    std::string account_cookie;
    if (!create || registering) {
      account_cookie = RememberBrowser(*flow);
      if (account_cookie.empty()) { Error(std::move(reply), "storage_failed"); return; }
    }
    FlushLoginFailures(*flow);
    RecordEvent(flow->account, create ? "passkeyName" : "account", create ? "registered" : "signedIn", "Passkey");
    if (!create || registering) RestoreDevice(*flow);
    Json(std::move(reply), base::DictValue().Set("email", flow->email).Set("signed_in", true), net::HTTP_OK, account_cookie.empty() ? std::string() : flow->cookie, false, account_cookie); return;
  }
  if (flow->account.empty()) { Error(std::move(reply), "sign_in_required", net::HTTP_UNAUTHORIZED); return; }
  if (path == "/auth/remote/ticket") {
    if (!AllowStart(false) || remote_tickets_.size() >= 128) {
      Error(std::move(reply), "rate_limited", net::HTTP_TOO_MANY_REQUESTS); return;
    }
    const auto ticket = Random();
    remote_tickets_.emplace(Encode(Digest(ticket)), RemoteTicket{
        flow->session_hash, base::TimeTicks::Now() + base::Seconds(60)});
    Json(std::move(reply), base::DictValue().Set("ticket", ticket).Set("expires_in", 60)); return;
  }
  if (path == "/auth/device/approve") {
    if (flow->management || !flow->identity_verified || flow->revoked) { Error(std::move(reply), "invalid_request"); return; }
    if (!flow->approved) {
      flow->approved_account = flow->account;
      flow->confirmation = Random();
    }
    flow->approved = true; Json(std::move(reply), base::DictValue().Set("approved", true)); return;
  }
  if (path == "/auth/account/avatar") {
    const auto id = Field(*body, "id"), hash = Field(*body, "hash");
    auto device = store_.Get("device", id);
    // Browser sessions may read images only for devices owned by this account.
    // Missing, revoked and foreign devices all use the same response.
    if (!device || Field(*device, "account") != flow->account || !IsAvatarHash(hash) ||
        Field(*device, "avatar_hash") != hash) {
      Error(std::move(reply), "unavailable", net::HTTP_NOT_FOUND); return;
    }
    const auto data = AvatarData(id, hash); std::string jpeg;
    if (!DecodeAvatar(hash, data, &jpeg)) {
      Error(std::move(reply), "unavailable", net::HTTP_NOT_FOUND); return;
    }
    Json(std::move(reply), base::DictValue().Set("hash", hash).Set("data", data)); return;
  }
  if (path == "/auth/account/history") {
    Json(std::move(reply), base::DictValue().Set("events", store_.Events(flow->account))); return;
  }
  if (path == "/auth/account/status") {
    auto status = status_provider_ ? status_provider_.Run(flow->account) : base::DictValue();
    base::ListValue diagnostics;
    for (const auto& item : store_.List("diagnostic", flow->account)) {
      auto device = store_.Get("device", Field(item.GetDict(), "device"));
      if (!device || Field(*device, "account") != flow->account) continue;
      auto row = item.GetDict().Clone(); row.Set("name", Field(*device, "name"));
      diagnostics.Append(std::move(row));
    }
    std::ranges::sort(diagnostics, [](const base::Value& a, const base::Value& b) {
      return a.GetDict().FindDouble("time").value_or(0) > b.GetDict().FindDouble("time").value_or(0);
    });
    while (diagnostics.size() > 20) diagnostics.erase(diagnostics.end() - 1);
    status.Set("diagnostics", std::move(diagnostics));
    Json(std::move(reply), std::move(status)); return;
  }
  if (path == "/auth/account/metadata") {
    auto row = store_.Get("device", Field(*body, "id"));
    const auto name = Field(*body, "name");
    if (!ValidName(name)) { Error(std::move(reply), "invalid_name"); return; }
    const auto group = Field(*body, "group"), note = Field(*body, "note");
    const auto* tags = body->FindList("tags");
    const auto order = body->FindInt("order");
    if (!row || Field(*row, "account") != flow->account || !body->FindString("group") || !body->FindString("note") ||
        (!group.empty() && !ValidName(group)) ||
        note.size() > 256 || !base::IsStringUTF8(note) ||
        std::ranges::any_of(note, [](unsigned char c) { return c < 32 || c == 127; }) ||
        !tags || tags->size() > 8 || !order || *order < 0 || *order > 9999) {
      Error(std::move(reply), "invalid_request"); return;
    }
    base::ListValue clean;
    for (const auto& tag : *tags) {
      if (!tag.is_string() || !ValidName(tag.GetString()) || tag.GetString().size() > 32) {
        Error(std::move(reply), "invalid_request"); return;
      }
      if (!clean.contains(tag.GetString())) clean.Append(tag.GetString());
    }
    row->Set("name", name); row->Set("group", group); row->Set("note", note); row->Set("tags", std::move(clean)); row->Set("order", *order);
    if (!store_.Put("device", Field(*row, "id"), flow->account, *row)) { Error(std::move(reply), "storage_failed"); return; }
    devices_changed_ = true;
    RecordEvent(flow->account, "device", "updated", Field(*row, "name"));
    Json(std::move(reply), base::DictValue().Set("saved", true)); return;
  }
  if (path == "/auth/account/list") {
    base::ListValue devices, passkeys;
    // Explicit public fields: never serialize device secrets or key material.
    for (const auto& item : store_.List("device", flow->account)) {
      const auto& row = item.GetDict(); const auto id = Field(row, "id");
      auto entry = base::DictValue().Set("id", id).Set("name", Field(row, "name"))
          .Set("avatar", std::clamp(row.FindInt("avatar").value_or(0), 0, 8))
          .Set("avatar_hash", IsAvatarHash(Field(row, "avatar_hash")) ? Field(row, "avatar_hash") : std::string())
          .Set("platform", Field(row, "platform")).Set("online", online_.contains(id))
          .Set("current", id == flow->current_device).Set("group", Field(row, "group"))
          .Set("note", Field(row, "note")).Set("order", row.FindInt("order").value_or(0));
      const auto* tags = row.FindList("tags"); entry.Set("tags", tags ? tags->Clone() : base::ListValue());
      for (const char* key : {"created", "last_seen"}) if (auto time = row.FindDouble(key)) entry.Set(key, *time);
      devices.Append(std::move(entry));
    }
    for (const auto& item : store_.List("passkey", flow->account)) {
      const auto& row = item.GetDict();
      auto entry = base::DictValue().Set("id", Field(row, "id")).Set("name", Field(row, "name"));
      for (const char* key : {"created", "last_used"}) if (auto time = row.FindDouble(key)) entry.Set(key, *time);
      passkeys.Append(std::move(entry));
    }
    Json(std::move(reply), base::DictValue().Set("devices", std::move(devices)).Set("passkeys", std::move(passkeys))
        .Set("google_linked", HasGoogleLogin(flow->account))); return;
  }
  if (path == "/auth/account/rename") {
    const auto kind = Field(*body, "kind"), id = Field(*body, "id"), name = Field(*body, "name");
    auto row = (kind == "device" || kind == "passkey") ? store_.Get(kind, id) : std::nullopt;
    if (!row || Field(*row, "account") != flow->account || !ValidName(name)) { Error(std::move(reply), "invalid_name"); return; }
    row->Set("name", name);
    if (!store_.Put(kind, id, flow->account, *row)) { Error(std::move(reply), "storage_failed"); return; }
    if (kind == "device") devices_changed_ = true;
    RecordEvent(flow->account, kind == "device" ? "device" : "passkeyName", "rename", name);
    Json(std::move(reply), base::DictValue().Set("renamed", true)); return;
  }
  if (path == "/auth/account/remove") {
    const auto kind = Field(*body, "kind");
    const auto removed = (kind == "device" || kind == "passkey") ? store_.Get(kind, Field(*body, "id")) : std::nullopt;
    if (kind == "passkey" && store_.List("passkey", flow->account).size() <= 1 && !HasGoogleLogin(flow->account)) {
      Error(std::move(reply), "last_login_method"); return;
    }
    if ((kind != "device" && kind != "passkey") || !store_.Remove(kind, Field(*body, "id"), flow->account)) { Error(std::move(reply), "invalid_request"); return; }
    RecordEvent(flow->account, kind == "device" ? "device" : "passkeyName", "removed", removed ? Field(*removed, "name") : std::string());
    if (kind == "device") {
      store_.Remove("diagnostic", Field(*body, "id"), flow->account);
      store_.Remove("avatar", Field(*body, "id"), flow->account);
      devices_changed_ = true;
      // Invalidate in-flight reauthentication as well as existing credentials.
      // A fresh flow after revocation must explicitly enroll the device again.
      if (removed) for (auto& [id, pending] : flows_) {
        if (!pending.management && pending.identity_key == Field(*removed, "identity_key") &&
            (pending.account.empty() || pending.account == flow->account)) {
          pending.revoked = true; pending.approved = false;
        }
      }
    }
    Json(std::move(reply), base::DictValue().Set("removed", true)); return;
  }
  Error(std::move(reply), "not_found", net::HTTP_NOT_FOUND);
}
void AccountAuth::GoogleCallback(const net::HttpServerRequestInfo& request, Flow& flow, Reply reply) {
  const GURL url(config_.origin + request.path);
  const auto state = Param(url, "state"), code = Param(url, "code");
  if (flow.http || !flow.account.empty() || flow.oauth_state.empty() || state != flow.oauth_state || code.empty() || code.size() > 4096) {
    LOG(WARNING) << "crDeskServer: Google login failed; stage=callback"
                 << " request_pending=" << !!flow.http << " already_signed_in=" << !flow.account.empty()
                 << " state_present=" << !flow.oauth_state.empty() << " state_matches=" << (state == flow.oauth_state)
                 << " code_present=" << !code.empty() << " code_too_long=" << (code.size() > 4096)
                 << " provider_error=" << SafeGoogleError(Param(url, "error"));
    LoginFailure(flow, "google_callback");
    Redirect(std::move(reply), "/auth/?flow=" + flow.id + "&error=google_failed"); return;
  }
  LOG(INFO) << "crDeskServer: Google login; stage=token-exchange started";
  flow.oauth_state.clear();
  std::string body = "grant_type=authorization_code&code=" + Escape(code) + "&client_id=" + Escape(config_.google_client_id) +
      "&client_secret=" + Escape(config_.google_client_secret) + "&redirect_uri=" + Escape(config_.origin + "/auth/google/callback") +
      "&code_verifier=" + flow.oauth_verifier;
  flow.http = std::make_unique<CloudHttp>();
  flow.http->Start("https://oauth2.googleapis.com/token", body, "application/x-www-form-urlencoded",
      base::BindOnce(&AccountAuth::GoogleToken, weak_.GetWeakPtr(), flow.id, std::move(reply)));
}
void AccountAuth::GoogleToken(std::string id, Reply reply, int error, int status, std::string body) {
  auto it = flows_.find(id); if (it == flows_.end() || it->second.expires <= base::TimeTicks::Now()) {
    LOG(WARNING) << "crDeskServer: Google login failed; stage=token-exchange reason=expired-flow";
    Redirect(std::move(reply), "/auth/?error=expired"); return;
  }
  auto& flow = it->second; flow.http.reset();
  auto value = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  if (error || status != 200 || !value || Field(*value, "id_token").empty()) {
    LOG(WARNING) << "crDeskServer: Google login failed; stage=token-exchange net_error=" << error
                 << " http_status=" << status << " json_valid=" << value.has_value()
                 << " id_token_present=" << (value && !Field(*value, "id_token").empty())
                 << " provider_error=" << SafeGoogleError(value ? Field(*value, "error") : std::string());
    LoginFailure(flow, "google_token_exchange");
    Redirect(std::move(reply), "/auth/?flow=" + id + "&error=google_failed"); return;
  }
  LOG(INFO) << "crDeskServer: Google login; stage=token-exchange succeeded; stage=public-keys started";
  flow.http = std::make_unique<CloudHttp>();
  flow.http->Start("https://www.googleapis.com/oauth2/v3/certs", "", "",
      base::BindOnce(&AccountAuth::GoogleKeys, weak_.GetWeakPtr(), id, std::move(reply), Field(*value, "id_token")));
}
void AccountAuth::GoogleKeys(std::string id, Reply reply, std::string token, int error, int status, std::string body) {
  auto it = flows_.find(id); if (it == flows_.end() || it->second.expires <= base::TimeTicks::Now()) {
    LOG(WARNING) << "crDeskServer: Google login failed; stage=public-keys reason=expired-flow";
    Redirect(std::move(reply), "/auth/?error=expired"); return;
  }
  auto& flow = it->second; flow.http.reset();
  const auto parts = base::SplitString(token, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  std::optional<base::DictValue> header, claims, jwks;
  if (parts.size() == 3) {
    header = base::JSONReader::ReadDict(Decode(parts[0]), base::JSON_PARSE_RFC);
    claims = base::JSONReader::ReadDict(Decode(parts[1]), base::JSON_PARSE_RFC);
    jwks = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  }
  bool valid = false;
  const char* failure = error ? "public-keys-network" : status != 200 ? "public-keys-http" :
      parts.size() != 3 ? "token-format" : !header ? "token-header-json" : !claims ? "token-claims-json" :
      !jwks ? "public-keys-json" : Field(*header, "alg") != "RS256" ? "token-algorithm" : "public-key-not-found";
  if (!error && status == 200 && header && claims && jwks && Field(*header, "alg") == "RS256") {
    bool key_imported = false;
    const auto* keys = jwks->FindList("keys");
    if (keys) for (const auto& item : *keys) {
      if (!item.is_dict()) continue;
      const auto& key = item.GetDict();
      if (Field(key, "kid") != Field(*header, "kid") || Field(key, "kty") != "RSA" || Field(key, "alg") != "RS256" || Field(key, "use") != "sig") continue;
      auto pub = crypto::keypair::PublicKey::FromRsaPublicKeyComponents(base::as_byte_span(Decode(Field(key, "n"))), base::as_byte_span(Decode(Field(key, "e"))));
      key_imported |= pub.has_value();
      failure = key_imported ? "token-signature" : "public-key-import";
      const auto data = parts[0] + "." + parts[1]; const auto signature = Decode(parts[2]);
      valid = pub && crypto::sign::Verify(crypto::sign::RSA_PKCS1_SHA256, *pub, base::as_byte_span(data), base::as_byte_span(signature));
      if (valid) break;
    }
    const double now = base::Time::Now().InSecondsFSinceUnixEpoch();
    if (valid) {
      // Preserve the verification policy and report only a fixed failure code.
      failure = [&]() -> const char* {
        if (Field(*claims, "iss") != "https://accounts.google.com" && Field(*claims, "iss") != "accounts.google.com") return "token-issuer";
        if (Field(*claims, "aud") != config_.google_client_id) return "token-audience";
        if (claims->contains("azp") && Field(*claims, "azp") != config_.google_client_id) return "token-authorized-party";
        if (flow.oauth_nonce.empty() || Field(*claims, "nonce") != flow.oauth_nonce) return "token-nonce";
        if (!(claims->FindDouble("exp").value_or(0) > now)) return "token-expired";
        if (!(claims->FindDouble("iat").value_or(0) <= now + 60)) return "token-issued-in-future";
        if (!(claims->FindDouble("iat").value_or(0) >= now - 600)) return "token-issued-too-long-ago";
        if (!claims->FindBool("email_verified").value_or(false)) return "email-unverified";
        if (Field(*claims, "sub").empty() || Field(*claims, "sub").size() > 255) return "token-subject";
        if (Field(*claims, "email").empty() || Field(*claims, "email").size() > 320) return "token-email";
        return nullptr;
      }();
      valid = !failure;
    }
  }
  flow.oauth_nonce.clear(); flow.oauth_verifier.clear();
  if (valid) {
    flow.account = store_.GoogleAccount(Field(*claims, "sub"), Field(*claims, "email"));
    if (!flow.account.empty()) { flow.email = Field(*claims, "email"); flow.login_method = "google"; }
    else failure = "account-store";
  }
  std::string account_cookie;
  if (!flow.account.empty()) {
    account_cookie = RememberBrowser(flow);
    if (account_cookie.empty()) failure = "session-store";
  }
  if (flow.account.empty()) {
    LoginFailure(flow, failure ? failure : "google_verification");
    LOG(WARNING) << "crDeskServer: Google login failed; stage=verification reason=" << failure
                 << " net_error=" << error << " http_status=" << status;
  } else {
    FlushLoginFailures(flow);
    RecordEvent(flow.account, "account", "signedIn", "Google");
    RestoreDevice(flow);
    LOG(INFO) << "crDeskServer: Google login succeeded";
  }
  Redirect(std::move(reply), "/auth/?flow=" + id + (flow.account.empty() ? "&error=google_failed" : ""), account_cookie, account_cookie.empty() ? std::string() : flow.cookie);
}
base::DictValue AccountAuth::PasskeyOptions(Flow& flow, const std::string& operation) {
  const bool create = operation != "get";
  flow.passkey_expires = base::TimeTicks::Now() + base::Minutes(2);
  flow.passkey_challenge = Random(); flow.passkey_operation = operation;
  auto options = base::DictValue().Set("challenge", flow.passkey_challenge).Set("timeout", 120000);
  if (!create) { options.Set("rpId", config_.rp_id); options.Set("userVerification", "required"); return options; }
  base::ListValue exclude;
  for (const auto& key : store_.List("passkey", flow.account)) exclude.Append(base::DictValue().Set("type", "public-key").Set("id", Field(key.GetDict(), "id")));
  options.Set("rp", base::DictValue().Set("id", config_.rp_id).Set("name", "crDesk"));
  const auto account = operation == "register" ? flow.passkey_account : flow.account;
  const auto profile = store_.Get("account", account);
  const auto name = operation == "register" ? flow.passkey_name :
      (!flow.email.empty() ? flow.email : profile ? Field(*profile, "name") : account);
  options.Set("user", base::DictValue().Set("id", Encode(account)).Set("name", flow.email.empty() ? account : flow.email).Set("displayName", name));
  options.Set("pubKeyCredParams", base::ListValue().Append(base::DictValue().Set("type", "public-key").Set("alg", -7)).Append(base::DictValue().Set("type", "public-key").Set("alg", -257)));
  options.Set("authenticatorSelection", base::DictValue().Set("residentKey", "required").Set("userVerification", "required"));
  options.Set("attestation", "none"); options.Set("excludeCredentials", std::move(exclude)); return options;
}
bool AccountAuth::PasskeyFinish(Flow& flow, const base::DictValue& body, const std::string& requested_operation) {
  const bool create = requested_operation != "get", registering = requested_operation == "register";
  const auto pending_account = std::exchange(flow.passkey_account, {});
  const auto pending_name = std::exchange(flow.passkey_name, {});
  const auto challenge = std::exchange(flow.passkey_challenge, {});
  const auto operation = std::exchange(flow.passkey_operation, {});
  const auto client_data = Decode(Field(body, "clientDataJSON"));
  auto client = base::JSONReader::ReadDict(client_data, base::JSON_PARSE_RFC);
  if (base::TimeTicks::Now() > flow.passkey_expires || challenge.empty() || operation != requested_operation || !client ||
      Field(*client, "type") != (create ? "webauthn.create" : "webauthn.get") ||
      Field(*client, "challenge") != challenge || Field(*client, "origin") != config_.origin ||
      (client->contains("crossOrigin") && client->FindBool("crossOrigin") != false) || client->contains("topOrigin")) return false;
  const auto id = Field(body, "id");
  if (Decode(id).empty() || Decode(id).size() > 1024) return false;
  if (create) {
    if (registering && (pending_account.empty() || !ValidName(pending_name))) return false;
    const auto& account = registering ? pending_account : flow.account;
    const auto bytes = Decode(Field(body, "attestationObject"));
    auto cbor = cbor::Reader::Read(base::as_byte_span(bytes));
    if (!cbor || !cbor->is_map()) return false;
    const auto& map = cbor->GetMap();
    auto fmt = map.find(cbor::Value("fmt")), stmt = map.find(cbor::Value("attStmt"));
    if (fmt == map.end() || !fmt->second.is_string() || fmt->second.GetString() != "none" ||
        stmt == map.end() || !stmt->second.is_map() || !stmt->second.GetMap().empty()) return false;
    auto object = device::AttestationObject::Parse(*cbor);
    if (!object || !ValidAuth(object->authenticator_data(), config_.rp_id) || !object->authenticator_data().attested_data()) return false;
    const auto& auth = object->authenticator_data(); const auto* key = auth.attested_data()->public_key();
    const auto credential = object->GetCredentialId();
    if (!key || !key->der_bytes || (key->algorithm != -7 && key->algorithm != -257) ||
        id != Encode(std::string_view(reinterpret_cast<const char*>(credential.data()), credential.size()))) return false;
    auto record = base::DictValue().Set("id", id).Set("account", account).Set("alg", key->algorithm)
        .Set("key", Encode(std::string_view(reinterpret_cast<const char*>(key->der_bytes->data()), key->der_bytes->size())))
        .Set("counter", static_cast<double>(Counter(auth))).Set("backup_eligible", auth.backup_eligible());
    record.Set("created", Timestamp());
    if (!registering) return store_.Put("passkey", id, account, record, true);
    if (!store_.CreatePasskeyAccount(account, pending_name, id, record)) return false;
    flow.account = account; flow.email.clear(); flow.login_method = "passkey";
    return true;
  }
  auto saved = store_.Get("passkey", id); if (!saved) return false;
  const auto account = Field(*saved, "account");
  if (Decode(Field(body, "userHandle")) != account) return false;
  const auto bytes = Decode(Field(body, "authenticatorData"));
  auto auth = device::AuthenticatorData::DecodeAuthenticatorData(base::as_byte_span(bytes));
  if (!auth || !ValidAuth(*auth, config_.rp_id) || auth->attestation_credential_included() ||
      auth->backup_eligible() != saved->FindBool("backup_eligible").value_or(false)) return false;
  auto key = crypto::keypair::PublicKey::FromSubjectPublicKeyInfo(base::as_byte_span(Decode(Field(*saved, "key"))));
  const int alg = saved->FindInt("alg").value_or(0);
  if (!key || (alg == -7 ? !key->IsEcP256() : alg == -257 ? !key->IsRsa() : true)) return false;
  const auto signed_data = bytes + Digest(client_data), signature = Decode(Field(body, "signature"));
  if (!crypto::sign::Verify(alg == -7 ? crypto::sign::ECDSA_SHA256 : crypto::sign::RSA_PKCS1_SHA256, *key, base::as_byte_span(signed_data), base::as_byte_span(signature))) return false;
  const double previous = saved->FindDouble("counter").value_or(0); const auto count = Counter(*auth);
  if ((previous || count) && count <= previous) return false;
  saved->Set("counter", static_cast<double>(count));
  saved->Set("last_used", Timestamp());
  if (!store_.Put("passkey", id, account, *saved)) return false;
  auto profile = store_.Get("account", account); if (!profile) return false;
  flow.account = account; flow.email = Field(*profile, "email"); flow.login_method = "passkey"; return true;
}
}
