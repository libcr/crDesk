// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include <cstdio>
#include <utility>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/rand_util.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "crDesk/server/server.h"
#include "crDesk/server/server_config.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::FeatureList::InitInstance("", "");
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_STDERR;
  logging::InitLogging(settings);
  const auto* command = base::CommandLine::ForCurrentProcess();
  if (command->HasSwitch("help")) {
    std::puts("crDeskServer --config=/absolute/path/server.json [--check-config]\n"
              "crDeskServer --generate-secret\n"
              "Listens on 127.0.0.1 only. Use an HTTPS/WebSocket reverse proxy.");
    return 0;
  }
  if (command->HasSwitch("generate-secret")) {
    std::puts(base::ToLowerASCII(base::HexEncode(base::RandBytesAsString(32))).c_str());
    return 0;
  }
  crdesk::server::ServerConfig config;
  std::string error;
  if (!base::i18n::InitializeICU()) {
    LOG(ERROR) << "crDeskServer: failed to initialize ICU data.";
    return 2;
  }
  if (!crdesk::server::LoadConfig(command->GetSwitchValuePath("config"), config, error)) {
    LOG(ERROR) << "crDeskServer: " << error;
    return 2;
  }
  if (command->HasSwitch("check-config")) {
    std::puts("Configuration is valid.");
    return 0;
  }
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crDeskServer");
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  auto network_change_notifier = net::NetworkChangeNotifier::CreateIfNeeded();
  crdesk::server::Server server(std::move(config));
  const int result = server.Start();
  if (result != net::OK) {
    LOG(ERROR) << "crDeskServer: listen failed; error=" << net::ErrorToString(result);
    return 1;
  }
  base::RunLoop().Run();
  return 0;
}
