/*
  Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQLD_MOCK_STATEMENT_READER_INCLUDED
#define MYSQLD_MOCK_STATEMENT_READER_INCLUDED

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <openssl/bio.h>

#include "mysql/harness/destination_endpoint.h"
#include "mysql/harness/destination_socket.h"
#include "mysql/harness/net_ts/buffer.h"
#include "mysql/harness/net_ts/executor.h"
#include "mysql/harness/net_ts/internet.h"
#include "mysql/harness/net_ts/io_context.h"
#include "mysql/harness/net_ts/socket.h"
#include "mysql/harness/net_ts/timer.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/stdx/monitor.h"
#include "mysql/harness/tls_error.h"
#include "mysql/harness/tls_server_context.h"
#include "mysqlrouter/classic_protocol_message.h"
#include "mysqlrouter/classic_protocol_session_track.h"

namespace server_mock {

/** @brief Vector for keeping has_value|string representation of the values
 *         of the single row (ordered by column)
 **/
using RowValueType = std::vector<std::optional<std::string>>;

using session_tracker_field =
    std::variant<classic_protocol::session_track::TransactionCharacteristics,
                 classic_protocol::session_track::TransactionState,
                 classic_protocol::session_track::SystemVariable,
                 classic_protocol::session_track::Schema,
                 classic_protocol::session_track::State,
                 classic_protocol::session_track::Gtid>;

std::string encode_session_trackers(
    const std::vector<session_tracker_field> &trackers);

/** @brief Keeps result data for single SQL statement that returns
 *         resultset.
 **/
struct ResultsetResponse {
  std::vector<classic_protocol::message::server::ColumnMeta> columns;
  std::vector<RowValueType> rows;

  classic_protocol::message::server::Eof end_of_rows;
};

using OkResponse = classic_protocol::message::server::Ok;
using ErrorResponse = classic_protocol::message::server::Error;

struct AsyncNotice {
  // how many milliseconds after the client connects this Notice
  // should be sent to the client
  std::chrono::milliseconds send_offset_ms;
  unsigned type;
  bool is_local;  // true = local, false = global
  std::string payload;
};

class Connection {
 public:
  explicit Connection(mysql_harness::DestinationSocket sock,
                      mysql_harness::DestinationEndpoint ep,
                      TlsServerContext &tls_ctx)
      : sock_(std::move(sock)), ep_(std::move(ep)), tls_ctx_(tls_ctx) {
    // if it doesn't work, no problem.

    assert(sock_.is_tcp() == ep.is_tcp());

    if (sock_.is_tcp()) {
      sock_.set_option(net::ip::tcp::no_delay{true});
    }
    sock_.native_non_blocking(true);
  }

  stdx::expected<size_t, std::error_code> write_ssl(
      const net::const_buffer &buf);

  stdx::expected<size_t, std::error_code> read_ssl(
      const net::mutable_buffer &buf);

  stdx::expected<size_t, std::error_code> avail_ssl();

  net::io_context::executor_type get_executor() { return sock_.get_executor(); }

  net::io_context &io_context() { return get_executor().context(); }

  template <class CompletionToken>
  void async_send_tls(CompletionToken &&token) {
    net::async_completion<CompletionToken, void(std::error_code, size_t)> init{
        token};

    const auto write_res = write_ssl(net::buffer(send_buffer_));
    if (!write_res) {
      auto write_ec = write_res.error();

      if (write_ec == TlsErrc::kWantRead || write_ec == TlsErrc::kWantWrite) {
        sock_.async_wait(
            write_ec == TlsErrc::kWantRead ? net::socket_base::wait_read
                                           : net::socket_base::wait_write,
            [this, compl_handler = std::move(init.completion_handler)](
                std::error_code ec) mutable {
              if (ec) {
                compl_handler(ec, {});
                return;
              }

              async_send(std::move(compl_handler));
            });
      } else {
        net::defer(get_executor(),
                   [compl_handler = std::move(init.completion_handler),
                    ec = write_res.error()]() { compl_handler(ec, {}); });
      }
    } else {
      net::dynamic_buffer(send_buffer_).consume(write_res.value());

      net::defer(get_executor(),
                 [compl_handler = std::move(init.completion_handler),
                  transferred = write_res.value()]() {
                   compl_handler({}, transferred);
                 });
    }

    return init.result.get();
  }

  template <class CompletionToken>
  void async_send(CompletionToken &&token) {
    if (is_tls()) {
      async_send_tls(std::forward<CompletionToken>(token));
    } else {
      sock_.async_send(net::dynamic_buffer(send_buffer_),
                       std::forward<CompletionToken>(token));
    }
  }

  template <class CompletionToken>
  void async_receive_tls(CompletionToken &&token) {
    net::async_completion<CompletionToken, void(std::error_code, size_t)> init{
        token};

    auto buf = net::dynamic_buffer(recv_buffer_);

    auto orig_size = buf.size();
    auto grow_size = 16 * 1024;

    buf.grow(grow_size);
    size_t transferred{};
    auto read_res = read_ssl(buf.data(orig_size, grow_size));
    if (read_res) {
      transferred = read_res.value();
    }

    buf.shrink(grow_size - transferred);

    if (!read_res) {
      const auto read_ec = read_res.error();
      if (read_ec == TlsErrc::kWantRead || read_ec == TlsErrc::kWantWrite) {
        sock_.async_wait(
            read_ec == TlsErrc::kWantRead ? net::socket_base::wait_read
                                          : net::socket_base::wait_write,
            [this, compl_handler = std::move(init.completion_handler)](
                std::error_code ec) mutable {
              if (ec) {
                compl_handler(ec, {});
                return;
              }

              async_receive_tls(std::move(compl_handler));
            });
      } else {
        // as we can't handle the error, forward the error to the
        // completion handler

        net::defer(
            get_executor(),
            [compl_handler = std::move(init.completion_handler),
             ec = map_tls_error_code(read_ec)]() { compl_handler(ec, {}); });
      }
    } else {
      // success, forward it to the completion handler
      net::defer(get_executor(),
                 [compl_handler = std::move(init.completion_handler),
                  transferred]() { compl_handler({}, transferred); });
    }

    return init.result.get();
  }

  template <class CompletionToken>
  void async_receive(CompletionToken &&token) {
    is_terminated_([&](const bool killed) {
      if (killed) {
        net::async_completion<CompletionToken, void(std::error_code, size_t)>
            init{token};

        net::defer(get_executor(), [compl_handler =
                                        std::move(init.completion_handler)]() {
          compl_handler(make_error_code(std::errc::operation_canceled), 0);
        });
      } else if (is_tls()) {
        return async_receive_tls(std::forward<CompletionToken>(token));
      } else {
        return sock_.async_recv(net::dynamic_buffer(recv_buffer_),
                                std::forward<CompletionToken>(token));
      }
    });
  }

  template <class CompletionToken>
  void async_tls_accept(CompletionToken &&token) {
    net::async_completion<CompletionToken, void(std::error_code)> init{token};

    // data may already be pending
    auto res = tls_accept();
    if (!res) {
      auto ec = res.error();
      if (ec == TlsErrc::kWantRead || ec == TlsErrc::kWantWrite) {
        auto wt = ec == TlsErrc::kWantRead ? net::socket_base::wait_read
                                           : net::socket_base::wait_write;

        sock_.async_wait(
            wt, [&, compl_handler = std::move(init.completion_handler)](
                    std::error_code ec) mutable {
              if (ec) {
                compl_handler(ec);
                return;
              }

              // call async accept again.
              async_tls_accept(std::move(compl_handler));
            });
      } else {
        net::defer(get_executor(),
                   [ec, compl_handler = std::move(init.completion_handler)]() {
                     compl_handler(ec);
                   });
      }
    } else {
      net::defer(get_executor(),
                 [compl_handler = std::move(init.completion_handler)]() {
                   compl_handler({});
                 });
    }

    return init.result.get();
  }

  bool is_tls() { return static_cast<bool>(ssl_); }

  const SSL *ssl() const { return ssl_.get(); }

  const std::vector<uint8_t> &send_buffer() const { return send_buffer_; }
  std::vector<uint8_t> &send_buffer() { return send_buffer_; }

  const std::vector<uint8_t> &recv_buffer() const { return recv_buffer_; }
  std::vector<uint8_t> &recv_buffer() { return recv_buffer_; }

  stdx::expected<void, std::error_code> cancel() { return sock_.cancel(); }

  net::impl::socket::native_handle_type native_handle() const {
    return sock_.native_handle();
  }

  void init_tls();

  stdx::expected<void, std::error_code> tls_accept();

  void terminate();

 private:
  // TlsErrc to net::stream_errc if needed.
  static std::error_code map_tls_error_code(std::error_code ec) {
    return (ec == TlsErrc::kZeroReturn) ? net::stream_errc::eof : ec;
  }

  Monitor<bool> is_terminated_{false};

  mysql_harness::DestinationSocket sock_;
  mysql_harness::DestinationEndpoint ep_;

  std::vector<uint8_t> send_buffer_;
  std::vector<uint8_t> recv_buffer_;

  class SSL_Deleter {
   public:
    void operator()(SSL *ssl) { SSL_free(ssl); }
  };

  std::unique_ptr<SSL, SSL_Deleter> ssl_;

  TlsServerContext &tls_ctx_;
};

class ProtocolBase {
 public:
  explicit ProtocolBase(mysql_harness::DestinationSocket sock,
                        mysql_harness::DestinationEndpoint ep,
                        TlsServerContext &tls_ctx)
      : conn_(std::move(sock), std::move(ep), tls_ctx) {}

  ProtocolBase(const ProtocolBase &) = delete;
  ProtocolBase(ProtocolBase &&) = delete;

  ProtocolBase &operator=(const ProtocolBase &) = delete;
  ProtocolBase &operator=(ProtocolBase &&rhs) = delete;

  virtual ~ProtocolBase() = default;

  // throws std::system_error
  virtual void encode_error(const ErrorResponse &resp) = 0;

  // throws std::system_error
  virtual void encode_ok(const OkResponse &resp) = 0;

  // throws std::system_error
  virtual void encode_resultset(const ResultsetResponse &response) = 0;

  template <class CompletionToken>
  void async_send_tls(CompletionToken &&token) {
    return conn_.async_send_tls(std::forward<CompletionToken>(token));
  }

  template <class CompletionToken>
  void async_send(CompletionToken &&token) {
    return conn_.async_send(std::forward<CompletionToken>(token));
  }

  template <class CompletionToken>
  void async_receive(CompletionToken &&token) {
    return conn_.async_receive(std::forward<CompletionToken>(token));
  }

  const std::vector<uint8_t> &send_buffer() const {
    return conn_.send_buffer();
  }

  std::vector<uint8_t> &send_buffer() { return conn_.send_buffer(); }

  const std::vector<uint8_t> &recv_buffer() const {
    return conn_.recv_buffer();
  }

  std::vector<uint8_t> &recv_buffer() { return conn_.recv_buffer(); }

  void username(const std::string &username) { username_ = username; }

  std::string username() const { return username_; }

  void server_auth_method_name(const std::string &auth_method_name) {
    server_auth_method_name_ = auth_method_name;
  }

  std::string server_auth_method_name() const {
    return server_auth_method_name_;
  }

  void server_auth_method_data(const std::string &auth_method_data) {
    server_auth_method_data_ = auth_method_data;
  }

  std::string server_auth_method_data() const {
    return server_auth_method_data_;
  }

  void auth_method_name(const std::string &auth_method_name) {
    auth_method_name_ = auth_method_name;
  }

  std::string auth_method_name() const { return auth_method_name_; }

  void auth_method_data(const std::string &auth_method_data) {
    auth_method_data_ = auth_method_data;
  }

  std::string auth_method_data() const { return auth_method_data_; }

  static bool authenticate(const std::string &auth_method_name,
                           const std::string &auth_method_data,
                           const std::string &password,
                           const std::vector<uint8_t> &auth_response);

  net::steady_timer &exec_timer() { return exec_timer_; }

  void cancel();

  /**
   * terminate the current connection.
   *
   * sets is_terminated(true) and cancels the current operation.
   *
   * may be called from another thread.
   */
  void terminate() { conn_.terminate(); }

  net::io_context &io_context() { return conn_.io_context(); }

  Connection &connection() { return conn_; }

 protected:
  Connection conn_;
  net::steady_timer exec_timer_{io_context()};

  std::string username_{};

  std::string server_auth_method_name_{};
  std::string server_auth_method_data_{};

  std::string auth_method_name_{};
  std::string auth_method_data_{};
};

class StatementReaderBase {
 public:
  struct handshake_data {
    classic_protocol::message::server::Greeting greeting;

    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<std::string> auth_method_name;
    bool cert_required{false};
    std::optional<std::string> cert_subject;
    std::optional<std::string> cert_issuer;

    std::chrono::microseconds exec_time;
  };

  StatementReaderBase() = default;

  StatementReaderBase(const StatementReaderBase &) = default;
  StatementReaderBase(StatementReaderBase &&) = default;

  StatementReaderBase &operator=(const StatementReaderBase &) = default;
  StatementReaderBase &operator=(StatementReaderBase &&) = default;

  virtual ~StatementReaderBase() = default;

  /** @brief Returns the data about the next statement from the
   *         json file. If there is no more statements it returns
   *         empty statement.
   **/
  virtual void handle_statement(const std::string &statement,
                                ProtocolBase *protocol) = 0;

  /** @brief Returns the default execution time in microseconds. If
   *         no default execution time is provided in json file, then
   *         0 microseconds is returned.
   **/
  virtual std::chrono::microseconds get_default_exec_time() = 0;

  virtual std::vector<AsyncNotice> get_async_notices() = 0;

  virtual stdx::expected<handshake_data, ErrorResponse> handshake(
      bool is_greeting) = 0;

  virtual void set_session_ssl_info(const SSL *ssl) = 0;
};

}  // namespace server_mock

#endif
