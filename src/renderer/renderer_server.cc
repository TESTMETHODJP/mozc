// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "renderer/renderer_server.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "base/const.h"
#include "base/logging.h"
#include "base/system_util.h"
#include "client/client_interface.h"
#include "config/config_handler.h"
#include "ipc/ipc.h"
#include "ipc/named_event.h"
#include "ipc/process_watch_dog.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "protocol/renderer_command.pb.h"
#include "renderer/renderer_interface.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

#ifdef _WIN32
#include <windows.h>
#endif  // _WIN32

// By default, mozc_renderer quits when user-input continues to be
// idle for 10min.
ABSL_FLAG(int32_t, timeout, 10 * 60, "timeout of candidate server (sec)");
ABSL_FLAG(bool, restricted, false,
          "launch candidates server with restricted mode");

namespace mozc {
namespace renderer {

namespace {
#ifdef _WIN32
constexpr int kNumConnections = 1;
#else   // _WIN32
constexpr int kNumConnections = 10;
#endif  // _WIN32
constexpr absl::Duration kIPCServerTimeOut = absl::Milliseconds(1000);
constexpr char kServiceName[] = "renderer";

std::string GetServiceName() {
  std::string name = kServiceName;
  const std::string desktop_name = SystemUtil::GetDesktopNameAsString();
  if (!desktop_name.empty()) {
    absl::StrAppend(&name, ".", desktop_name);
  }
  return name;
}
}  // namespace

class ParentApplicationWatchDog : public ProcessWatchDog {
 public:
  ParentApplicationWatchDog(const ParentApplicationWatchDog &) = delete;
  ParentApplicationWatchDog &operator=(const ParentApplicationWatchDog &) =
      delete;
  explicit ParentApplicationWatchDog(RendererServer *renderer_server)
      : renderer_server_(renderer_server) {}
  ~ParentApplicationWatchDog() override = default;

  void Signaled(ProcessWatchDog::SignalType type) override {
    if (renderer_server_ == nullptr) {
      LOG(ERROR) << "renderer_server is nullptr";
      return;
    }
    if (type == ProcessWatchDog::PROCESS_SIGNALED ||
        type == ProcessWatchDog::THREAD_SIGNALED) {
      VLOG(1) << "Parent process is terminated: call Hide event";
      mozc::commands::RendererCommand command;
      command.set_type(mozc::commands::RendererCommand::UPDATE);
      command.set_visible(false);
      std::string proto_message;
      if (command.SerializeToString(&proto_message)) {
        renderer_server_->AsyncExecCommand(proto_message);
      } else {
        LOG(ERROR) << "SerializeToString failed";
      }
    }
  }

 private:
  RendererServer *renderer_server_;
};

class RendererServerSendCommand : public client::SendCommandInterface {
 public:
  RendererServerSendCommand() : receiver_handle_(0) {}
  RendererServerSendCommand(const RendererServerSendCommand &) = delete;
  RendererServerSendCommand &operator=(const RendererServerSendCommand &) =
      delete;
  ~RendererServerSendCommand() override = default;

  bool SendCommand(const mozc::commands::SessionCommand &command,
                   mozc::commands::Output *output) override {
#ifdef _WIN32
    if ((command.type() != commands::SessionCommand::SELECT_CANDIDATE) &&
        (command.type() != commands::SessionCommand::HIGHLIGHT_CANDIDATE) &&
        (command.type() != commands::SessionCommand::USAGE_STATS_EVENT)) {
      // Unsupported command.
      return false;
    }

    HWND target = reinterpret_cast<HWND>(receiver_handle_);
    if (target == nullptr) {
      LOG(ERROR) << "target window is nullptr";
      return false;
    }
    UINT mozc_msg = ::RegisterWindowMessageW(kMessageReceiverMessageName);
    if (mozc_msg == 0) {
      LOG(ERROR) << "RegisterWindowMessage failed: " << ::GetLastError();
      return false;
    }
    if (command.type() == mozc::commands::SessionCommand::USAGE_STATS_EVENT) {
      WPARAM type = static_cast<WPARAM>(command.type());
      LPARAM event = static_cast<LPARAM>(command.usage_stats_event());
      ::PostMessage(target, mozc_msg, type, event);
    } else {  // SELECT_CANDIDATE or HIGHLIGHT_CANDIDATE
      WPARAM type = static_cast<WPARAM>(command.type());
      LPARAM id = static_cast<LPARAM>(command.id());
      ::PostMessage(target, mozc_msg, type, id);
    }
#endif  // _WIN32

    // TODO(all): implementation for Mac/Linux
    return true;
  }

  void set_receiver_handle(uint32_t receiver_handle) {
    receiver_handle_ = receiver_handle;
  }

 private:
  uint32_t receiver_handle_;
};

RendererServer::RendererServer()
    : IPCServer(GetServiceName(), kNumConnections, kIPCServerTimeOut),
      renderer_interface_(nullptr),
      timeout_(0),
      send_command_(new RendererServerSendCommand) {
  watch_dog_ = std::make_unique<ParentApplicationWatchDog>(this);
  watch_dog_->StartWatchDog();
  if (absl::GetFlag(FLAGS_restricted)) {
    absl::SetFlag(&FLAGS_timeout,
                  // set 60sec with restricted mode
                  std::min(absl::GetFlag(FLAGS_timeout), 60));
  }

  timeout_ =
      1000 * std::max(3, std::min(24 * 60 * 60, absl::GetFlag(FLAGS_timeout)));
  VLOG(2) << "timeout is set to be : " << timeout_;

#ifndef MOZC_NO_LOGGING
  config::Config config;
  config::ConfigHandler::GetConfig(&config);
  Logging::SetConfigVerboseLevel(config.verbose_level());
#endif  // MOZC_NO_LOGGING
}

RendererServer::~RendererServer() { watch_dog_->StopWatchDog(); }

void RendererServer::SetRendererInterface(
    RendererInterface *renderer_interface) {
  renderer_interface_ = renderer_interface;
  if (renderer_interface_ != nullptr) {
    renderer_interface_->SetSendCommandInterface(send_command_.get());
  }
}

int RendererServer::StartServer() {
  if (!Connected()) {
    LOG(ERROR) << "cannot start server";
    return -1;
  }

  LoopAndReturn();

  // send "ready" event to the client
  const std::string name = GetServiceName();
  NamedEventNotifier notifier(name.c_str());
  notifier.Notify();

  // start main event loop
  return StartMessageLoop();
}

bool RendererServer::Process(absl::string_view request, std::string *response) {
  // No need to set the result code.
  response->clear();

  // Cannot call the method directly like renderer_interface_->ExecCommand()
  // as it's not thread-safe.
  return AsyncExecCommand(request);
}

bool RendererServer::ExecCommandInternal(
    const commands::RendererCommand &command) {
  if (renderer_interface_ == nullptr) {
    LOG(ERROR) << "renderer_interface is nullptr";
    return false;
  }

  VLOG(2) << MOZC_LOG_PROTOBUF(command);

  // Check process info if update mode
  if (command.type() == commands::RendererCommand::UPDATE) {
    // set HWND of message-only window
    if (command.has_application_info() &&
        command.application_info().has_receiver_handle()) {
      send_command_->set_receiver_handle(
          command.application_info().receiver_handle());
    } else {
      LOG(WARNING) << "receiver_handle is not set";
    }

    // watch the parent application.
    if (command.has_application_info() &&
        command.application_info().has_process_id() &&
        command.application_info().has_thread_id()) {
      if (!watch_dog_->SetID(static_cast<ProcessWatchDog::ProcessID>(
                                 command.application_info().process_id()),
                             static_cast<ProcessWatchDog::ThreadID>(
                                 command.application_info().thread_id()),
                             -1)) {
        LOG(ERROR) << "Cannot set new ids for watch dog";
      }
    } else {
      LOG(WARNING) << "process id and thread id are not set";
    }
  }

  if (renderer_interface_->ExecCommand(command)) {
    return true;
  }

  return false;
}

uint32_t RendererServer::timeout() const { return timeout_; }
}  // namespace renderer
}  // namespace mozc
