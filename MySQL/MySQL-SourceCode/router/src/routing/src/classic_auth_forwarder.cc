/*
  Copyright (c) 2022, 2025, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is designed to work with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have either included with
  the program or referenced in the documentation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "classic_auth_forwarder.h"

#include <memory>  // unique_ptr
#include <system_error>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "auth_digest.h"
#include "classic_auth_caching_sha2_forwarder.h"
#include "classic_auth_caching_sha2_sender.h"
#include "classic_auth_cleartext.h"
#include "classic_auth_cleartext_forwarder.h"
#include "classic_auth_cleartext_sender.h"
#include "classic_auth_native_forwarder.h"
#include "classic_auth_native_sender.h"
#include "classic_auth_openid_connect_forwarder.h"
#include "classic_auth_openid_connect_sender.h"
#include "classic_auth_sha256_password_forwarder.h"
#include "classic_auth_sha256_password_sender.h"
#include "classic_frame.h"
#include "harness_assert.h"
#include "hexify.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/stdx/expected.h"
#include "mysqld_error.h"  // mysql-server error-codes
#include "mysqlrouter/classic_protocol_wire.h"

IMPORT_LOG_FUNCTIONS()

using mysql_harness::hexify;

class AuthGenericForwarder : public ForwardingProcessor {
 public:
  AuthGenericForwarder(MysqlRoutingClassicConnectionBase *conn,
                       std::string auth_method_name,
                       std::string initial_server_auth_data,
                       bool in_handshake = false)
      : ForwardingProcessor(conn),
        auth_method_name_{std::move(auth_method_name)},
        initial_server_auth_data_{std::move(initial_server_auth_data)},
        stage_{in_handshake ? Stage::Response : Stage::Init} {}

  enum class Stage {
    Init,

    ClientData,
    AuthData,

    Response,

    Error,
    Ok,

    Done,
  };

  stdx::expected<Result, std::error_code> process() override;

  void stage(Stage stage) { stage_ = stage; }
  [[nodiscard]] Stage stage() const { return stage_; }

 private:
  stdx::expected<Result, std::error_code> init();
  stdx::expected<Result, std::error_code> client_data();
  stdx::expected<Result, std::error_code> auth_data();
  stdx::expected<Result, std::error_code> response();
  stdx::expected<Result, std::error_code> error();
  stdx::expected<Result, std::error_code> ok();

  stdx::expected<Result, std::error_code> send_password();

  std::string auth_method_name_;
  std::string initial_server_auth_data_;
  std::string password_;

  Stage stage_;
};

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::process() {
  switch (stage()) {
    case Stage::Init:
      return init();
    case Stage::ClientData:
      return client_data();
    case Stage::Response:
      return response();
    case Stage::AuthData:
      return auth_data();
    case Stage::Error:
      return error();
    case Stage::Ok:
      return ok();
    case Stage::Done:
      return Result::Done;
  }

  harness_assert_this_should_not_execute();
}

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::init() {
  auto &dst_conn = connection()->client_conn();

  if (auto &tr = tracer()) {
    tr.trace(
        Tracer::Event().stage("generic::forward::switch: " + auth_method_name_ +
                              "\n" + hexify(initial_server_auth_data_)));
  }

  auto send_res = ClassicFrame::send_msg<
      classic_protocol::borrowed::message::server::AuthMethodSwitch>(
      dst_conn, {auth_method_name_, initial_server_auth_data_});
  if (!send_res) return send_client_failed(send_res.error());

  stage(Stage::ClientData);
  return Result::SendToClient;
}

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::client_data() {
  auto &src_conn = connection()->client_conn();

  auto msg_res = ClassicFrame::recv_msg<
      classic_protocol::borrowed::message::client::AuthMethodData>(src_conn);
  if (!msg_res) return recv_client_failed(msg_res.error());

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage(
        "generic::forward::client:\n" +
        mysql_harness::hexify(msg_res->auth_method_data())));
  }

  // if it isn't a public-key request, it is a fast-auth.
  stage(Stage::Response);

  return forward_client_to_server();
}

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::response() {
  // ERR|OK|EOF|other
  auto &src_conn = connection()->server_conn();
  auto &src_channel = src_conn.channel();
  auto &src_protocol = src_conn.protocol();

  // ensure the recv_buf has at last frame-header (+ msg-byte)
  auto read_res = ClassicFrame::ensure_has_msg_prefix(src_conn);
  if (!read_res) return recv_server_failed(read_res.error());

  const uint8_t msg_type = src_protocol.current_msg_type().value();

  enum class Msg {
    Ok = ClassicFrame::cmd_byte<classic_protocol::message::server::Ok>(),
    Error = ClassicFrame::cmd_byte<classic_protocol::message::server::Error>(),
    AuthData = ClassicFrame::cmd_byte<
        classic_protocol::message::server::AuthMethodData>(),
  };

  switch (Msg{msg_type}) {
    case Msg::AuthData:
      stage(Stage::AuthData);
      return Result::Again;
    case Msg::Ok:
      stage(Stage::Ok);
      return Result::Again;
    case Msg::Error:
      stage(Stage::Error);
      return Result::Again;
  }

  // if there is another packet, dump its payload for now.
  const auto &recv_buf = src_channel.recv_plain_view();

  // get as much data of the current frame from the recv-buffers to log it.
  (void)ClassicFrame::ensure_has_full_frame(src_conn);

  log_debug("received unexpected message from server in %s:\n%s",
            auth_method_name_.c_str(), hexify(recv_buf).c_str());

  return recv_server_failed(make_error_code(std::errc::bad_message));
}

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::auth_data() {
  auto &src_conn = connection()->server_conn();

  auto msg_res = ClassicFrame::recv_msg<
      classic_protocol::borrowed::message::server::AuthMethodData>(src_conn);
  if (!msg_res) return recv_server_failed(msg_res.error());

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("generic::forward::data\n" +
                                   hexify(msg_res->auth_method_data())));
  }
  stage(Stage::ClientData);

  return forward_server_to_client();
}

stdx::expected<Processor::Result, std::error_code> AuthGenericForwarder::ok() {
  stage(Stage::Done);

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("generic::forward::ok"));
  }

  // leave the message in the queue for the AuthForwarder.
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
AuthGenericForwarder::error() {
  stage(Stage::Done);

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("generic::forward::error"));
  }

  // leave the message in the queue for the AuthForwarder.
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code> AuthForwarder::process() {
  switch (stage()) {
    case Stage::Init:
      return init();
    case Stage::AuthMethodSwitch:
      return auth_method_switch();
    case Stage::Response:
      return response();
    case Stage::Error:
      return error();
    case Stage::Ok:
      return ok();
    case Stage::Done:
      return Result::Done;
  }

  harness_assert_this_should_not_execute();
}

stdx::expected<Processor::Result, std::error_code> AuthForwarder::init() {
  auto &src_conn = connection()->server_conn();
  auto &src_protocol = src_conn.protocol();

  auto &dst_conn = connection()->client_conn();
  auto &dst_protocol = dst_conn.protocol();

  // ensure the recv_buf has at last frame-header (+ msg-byte)
  auto read_res = ClassicFrame::ensure_has_msg_prefix(src_conn);
  if (!read_res) return recv_server_failed(read_res.error());

  const uint8_t msg_type = src_protocol.current_msg_type().value();

  if (msg_type == ClassicFrame::cmd_byte<
                      classic_protocol::message::server::AuthMethodSwitch>()) {
    stage(Stage::AuthMethodSwitch);
    return Result::Again;
  }

  auto auth_method_name = dst_protocol.auth_method_name();
  auto initial_auth_method_data = src_protocol.auth_method_data();

  // handle the pre-auth-plugin capabilities.
  if (auth_method_name.empty()) {
    auth_method_name =
        src_protocol.shared_capabilities().test(
            classic_protocol::capabilities::pos::secure_connection)
            ? AuthNativePassword::kName
            : "old_password";
  }

  if (auto &tr = tracer()) {
    tr.trace(
        Tracer::Event().stage("auth::forwarder::direct: " + auth_method_name));
  }

  if (auth_method_name == AuthSha256Password::kName) {
    connection()->push_processor(std::make_unique<AuthSha256Forwarder>(
        connection(), initial_auth_method_data, true));
  } else if (auth_method_name == AuthCachingSha2Password::kName) {
    connection()->push_processor(std::make_unique<AuthCachingSha2Forwarder>(
        connection(), initial_auth_method_data, true,
        client_requested_full_auth_));
  } else if (auth_method_name == AuthNativePassword::kName) {
    connection()->push_processor(std::make_unique<AuthNativeForwarder>(
        connection(), initial_auth_method_data, true));
  } else if (auth_method_name == AuthCleartextPassword::kName) {
    connection()->push_processor(std::make_unique<AuthCleartextForwarder>(
        connection(), initial_auth_method_data, true));
  } else if (auth_method_name == AuthOpenidConnect::kName) {
    connection()->push_processor(std::make_unique<AuthOpenidConnectForwarder>(
        connection(), initial_auth_method_data, true));
  } else {
    connection()->push_processor(std::make_unique<AuthGenericForwarder>(
        connection(), auth_method_name, initial_auth_method_data, true));
  }

  stage(Stage::Response);
  return Result::Again;
}

// server wants to switch to another auth-method.
stdx::expected<Processor::Result, std::error_code>
AuthForwarder::auth_method_switch() {
  auto &src_conn = connection()->server_conn();
  auto &src_protocol = src_conn.protocol();

  auto &dst_conn = connection()->client_conn();
  auto &dst_protocol = dst_conn.protocol();

  const auto msg_res = ClassicFrame::recv_msg<
      classic_protocol::borrowed::message::server::AuthMethodSwitch>(src_conn);
  if (!msg_res) return recv_server_failed(msg_res.error());

  const auto msg = *msg_res;

  const auto auth_method_name = std::string(msg.auth_method());
  const auto auth_method_data = std::string(msg.auth_method_data());

  src_protocol.auth_method_name(auth_method_name);
  src_protocol.auth_method_data(auth_method_data);

  if (auto &tr = tracer()) {
    tr.trace(
        Tracer::Event().stage("auth::forwarder::switch: " + auth_method_name));
  }

  // invalidates 'msg'
  discard_current_msg(src_conn);

  if (auth_method_name == AuthSha256Password::kName) {
    dst_protocol.auth_method_name(auth_method_name);
    dst_protocol.auth_method_data(auth_method_data);

    if (auto creds = dst_protocol.credentials().get(auth_method_name)) {
      connection()->push_processor(std::make_unique<AuthSha256Sender>(
          connection(), auth_method_data, *creds));
    } else {
      connection()->push_processor(std::make_unique<AuthSha256Forwarder>(
          connection(), auth_method_data, false));
    }
  } else if (auth_method_name == AuthCachingSha2Password::kName) {
    dst_protocol.auth_method_name(auth_method_name);
    dst_protocol.auth_method_data(auth_method_data);

    if (auto creds = dst_protocol.credentials().get(auth_method_name)) {
      connection()->push_processor(std::make_unique<AuthCachingSha2Sender>(
          connection(), auth_method_data, *creds));
    } else {
      // trigger a switch
      connection()->push_processor(std::make_unique<AuthCachingSha2Forwarder>(
          connection(), auth_method_data, false));
    }
  } else if (auth_method_name == AuthNativePassword::kName) {
    if (auto creds = dst_protocol.credentials().get(auth_method_name)) {
      dst_protocol.auth_method_name(auth_method_name);
      dst_protocol.auth_method_data(auth_method_data);

      connection()->push_processor(std::make_unique<AuthNativeSender>(
          connection(), auth_method_data, *creds));
    } else {
      connection()->push_processor(std::make_unique<AuthNativeForwarder>(
          connection(), auth_method_data, false));
    }
  } else if (auth_method_name == AuthCleartextPassword::kName) {
    dst_protocol.auth_method_name(auth_method_name);
    dst_protocol.auth_method_data(auth_method_data);

    if (auto creds = dst_protocol.credentials().get(auth_method_name)) {
      connection()->push_processor(std::make_unique<AuthCleartextSender>(
          connection(), auth_method_data, *creds));
    } else {
      connection()->push_processor(std::make_unique<AuthCleartextForwarder>(
          connection(), auth_method_data, false));
    }
  } else if (auth_method_name == AuthOpenidConnect::kName) {
    dst_protocol.auth_method_name(auth_method_name);
    dst_protocol.auth_method_data(auth_method_data);

    if (auto creds = dst_protocol.credentials().get(auth_method_name)) {
      connection()->push_processor(std::make_unique<AuthOpenidConnectSender>(
          connection(), auth_method_data, *creds));
    } else {
      connection()->push_processor(std::make_unique<AuthOpenidConnectForwarder>(
          connection(), auth_method_data, false));
    }
  } else {
    dst_protocol.auth_method_name(auth_method_name);
    dst_protocol.auth_method_data(auth_method_data);

    connection()->push_processor(std::make_unique<AuthGenericForwarder>(
        connection(), auth_method_name, auth_method_data));
  }

  stage(Stage::Response);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code> AuthForwarder::response() {
  // ERR|OK|EOF|other
  auto &src_conn = connection()->server_conn();
  auto &src_channel = src_conn.channel();
  auto &src_protocol = src_conn.protocol();

  // ensure the recv_buf has at last frame-header (+ msg-byte)
  auto read_res = ClassicFrame::ensure_has_msg_prefix(src_conn);
  if (!read_res) return recv_server_failed(read_res.error());

  const uint8_t msg_type = src_protocol.current_msg_type().value();

  enum class Msg {
    Ok = ClassicFrame::cmd_byte<classic_protocol::message::server::Ok>(),
    Error = ClassicFrame::cmd_byte<classic_protocol::message::server::Error>(),
  };

  switch (Msg{msg_type}) {
    case Msg::Ok:
      stage(Stage::Ok);
      return Result::Again;
    case Msg::Error:
      stage(Stage::Error);
      return Result::Again;
  }

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("auth::forwarder::response"));
  }

  // if there is another packet, dump its payload for now.
  const auto &recv_buf = src_channel.recv_plain_view();

  // get as much data of the current frame from the recv-buffers to log it.
  (void)ClassicFrame::ensure_has_full_frame(src_conn);

  log_debug("received unexpected message from server in auth:\n%s",
            hexify(recv_buf).c_str());

  return recv_server_failed(make_error_code(std::errc::bad_message));
}

stdx::expected<Processor::Result, std::error_code> AuthForwarder::ok() {
  stage(Stage::Done);

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("auth::forwarder::ok"));
  }

  // leave the message in the queue for the caller.
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code> AuthForwarder::error() {
  stage(Stage::Done);

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("auth::forwarder::error"));
  }

  // leave the message in the queue for the caller.
  return Result::Again;
}
