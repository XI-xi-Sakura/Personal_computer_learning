/*
  Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#include "classic_stmt_fetch_forwarder.h"

#include "classic_connection_base.h"
#include "classic_frame.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/tls_error.h"
#include "mysqld_error.h"  // mysql-server error-codes

stdx::expected<Processor::Result, std::error_code>
StmtFetchForwarder::process() {
  switch (stage()) {
    case Stage::Command:
      return command();
    case Stage::Response:
      return response();
    case Stage::EndOfRows:
      return end_of_rows();
    case Stage::Row:
      return row();
    case Stage::Error:
      return error();
    case Stage::Done:
      return Result::Done;
  }

  harness_assert_this_should_not_execute();
}

stdx::expected<Processor::Result, std::error_code>
StmtFetchForwarder::command() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("stmt_fetch::command"));
  }

  if (!connection()->server_conn().is_open()) {
    auto &src_conn = connection()->client_conn();

    auto frame_res = ClassicFrame::ensure_has_full_frame(src_conn);
    if (!frame_res) return recv_client_failed(frame_res.error());

    // discard the recv'ed message as there is ...
    //
    // - no server connection to send it to
    // - and therefore no prepared statement that could be executed on the
    //   server.
    discard_current_msg(src_conn);

    auto send_res = ClassicFrame::send_msg<
        classic_protocol::borrowed::message::server::Error>(
        src_conn,
        {ER_UNKNOWN_STMT_HANDLER, "Unknown prepared statement id", "HY000"});
    if (!send_res) return send_client_failed(send_res.error());

    stage(Stage::Done);
    return Result::SendToClient;
  } else {
    stage(Stage::Response);

    return forward_client_to_server();
  }
}

stdx::expected<Processor::Result, std::error_code>
StmtFetchForwarder::response() {
  auto &src_conn = connection()->server_conn();
  auto &src_protocol = src_conn.protocol();

  auto read_res = ClassicFrame::ensure_has_msg_prefix(src_conn);
  if (!read_res) return recv_server_failed(read_res.error());

  const uint8_t msg_type = src_protocol.current_msg_type().value();

  enum class Msg {
    Row = 0x00,
    EndOfRows =
        ClassicFrame::cmd_byte<classic_protocol::message::server::Eof>(),
    Error = ClassicFrame::cmd_byte<classic_protocol::message::server::Error>(),
  };

  switch (Msg{msg_type}) {
    case Msg::EndOfRows:
      stage(Stage::EndOfRows);
      return Result::Again;
    case Msg::Row:
      stage(Stage::Row);
      return Result::Again;
    case Msg::Error:
      stage(Stage::Error);
      return Result::Again;
  }

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("stmt_fetch::response"));
  }

  return stdx::unexpected(make_error_code(std::errc::bad_message));
}

stdx::expected<Processor::Result, std::error_code> StmtFetchForwarder::row() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("stmt_fetch::row"));
  }

  stage(Stage::Response);

  return forward_server_to_client(true);
}

stdx::expected<Processor::Result, std::error_code>
StmtFetchForwarder::end_of_rows() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("stmt_fetch::end_of_rows"));
  }

  stage(Stage::Done);

  return forward_server_to_client();
}

stdx::expected<Processor::Result, std::error_code> StmtFetchForwarder::error() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("stmt_fetch::error"));
  }

  stage(Stage::Done);

  return forward_server_to_client();
}
