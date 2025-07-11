/*
  Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include "classic_mock_session.h"

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <system_error>
#include <thread>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "hexify.h"
#include "mysql/harness/logging/logger.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/net_ts/buffer.h"
#include "mysql/harness/net_ts/impl/socket_constants.h"
#include "mysql/harness/net_ts/socket.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/tls_error.h"
#include "mysql/harness/utility/string.h"
#include "mysqld_error.h"
#include "mysqlrouter/classic_protocol.h"
#include "mysqlrouter/classic_protocol_codec_error.h"
#include "mysqlrouter/classic_protocol_codec_session_track.h"
#include "mysqlrouter/classic_protocol_constants.h"
#include "mysqlrouter/classic_protocol_message.h"
#include "mysqlrouter/classic_protocol_session_track.h"
#include "router/src/mock_server/src/authentication.h"
#include "router/src/mock_server/src/statement_reader.h"

using namespace std::string_literals;

namespace server_mock {

template <class Rep, class Period>
static std::string duration_to_us_string(
    const std::chrono::duration<Rep, Period> &dur) {
  return std::to_string(
             std::chrono::duration_cast<std::chrono::microseconds>(dur)
                 .count()) +
         " us";
}

stdx::expected<size_t, std::error_code> MySQLClassicProtocol::read_packet(
    std::vector<uint8_t> &payload) {
  net::const_buffer buf = net::buffer(recv_buffer());

  auto decode_res =
      classic_protocol::decode<classic_protocol::frame::Header>(buf, {});
  if (!decode_res) return stdx::unexpected(decode_res.error());

  const auto hdr_frame = decode_res.value();

  const auto hdr_size = hdr_frame.first;
  const auto hdr = hdr_frame.second;
  seq_no_ = hdr.seq_id() + 1;
  const auto payload_size = hdr.payload_size();

  if (payload_size == 0xffffff) {
    return stdx::unexpected(
        make_error_code(std::errc::operation_not_supported));
  }

  // skip the header.
  buf += hdr_size;

  if (buf.size() < payload_size) {
    // not enough data.
    return stdx::unexpected(
        make_error_code(classic_protocol::codec_errc::not_enough_input));
  }

  payload.resize(payload_size);
  net::buffer_copy(net::buffer(payload), buf, payload_size);

  // remove the bytes from the recv-buffer
  net::dynamic_buffer(recv_buffer()).consume(hdr_size + payload_size);

  return payload_size;
}

void MySQLServerMockSessionClassic::server_greeting() {
  auto started = std::chrono::steady_clock::now();

  const auto handshake_res = json_reader_->handshake(true /* is_greeting */);
  if (!handshake_res) {
    protocol_.encode_error(handshake_res.error());

    send_response_then_disconnect();

    return;
  }

  auto &exec_timer = protocol_.exec_timer();
  exec_timer.expires_after(handshake_res->exec_time);

  auto greeting = handshake_res->greeting;

  if (with_tls_) {
    greeting.capabilities(greeting.capabilities() |
                          classic_protocol::capabilities::ssl);
  }

  exec_timer.async_wait([this, greeting, started](std::error_code ec) {
    if (ec) {
      if (ec != std::errc::operation_canceled) {
        logger_.warning(
            [ec]() { return "wait for exec-time failed: " + ec.message(); });
      }

      disconnect();
      return;
    }
    // greeting contains a trailing \0, but we want it without \0
    auto auth_method_data = greeting.auth_method_data();

    if (auth_method_data.size() == 21) {
      auth_method_data.pop_back();  // strip last char
    }
    protocol_.server_auth_method_data(auth_method_data);
    protocol_.server_auth_method_name(greeting.auth_method_name());
    protocol_.encode_server_greeting(greeting);

    protocol_.async_send([this, started,
                          to_send = protocol_.send_buffer().size()](
                             std::error_code ec, size_t transferred) {
      if (ec) {
        disconnect();
        return;
      }

      if (to_send < transferred) {
        std::terminate();
      } else {
        auto now = std::chrono::steady_clock::now();

        logger_.info(mysql_harness::utility::string_format(
            "(%s)+< greeting", duration_to_us_string(now - started).c_str()));

        client_greeting();
      }
    });
  });
}

void MySQLServerMockSessionClassic::client_greeting() {
  // check we have enough of a client-greeting.
  std::vector<uint8_t> payload;
  auto frame_decode_res = protocol_.read_packet(payload);
  if (!frame_decode_res) {
    const auto ec = frame_decode_res.error();

    if (ec == classic_protocol::codec_errc::not_enough_input ||
        ec == std::errc::operation_would_block) {
      protocol_.async_receive(
          [this](std::error_code ec, size_t /* transferred */) {
            if (ec) {
              if (ec != std::errc::operation_canceled &&
                  ec != make_error_condition(std::errc::connection_reset) &&
                  ec != net::stream_errc::eof) {
                // op-cancelled: .cancel() was called
                // connection-reset: client closed the connection after
                // handshake was sent.
                logger_.warning("receiving client-greeting failed: " +
                                ec.message());
              }
              disconnect();
              return;
            }

            client_greeting();
          });

      return;
    }

    logger_.warning("decoding client-greeting frame failed: " + ec.message());
    disconnect();

    return;
  }

  if (auto *ssl = protocol_.connection().ssl()) {
    json_reader_->set_session_ssl_info(ssl);
  }

  auto decode_res =
      classic_protocol::decode<classic_protocol::message::client::Greeting>(
          net::buffer(payload), protocol_.server_capabilities());
  if (!decode_res) {
    auto ec = decode_res.error();

    logger_.warning("decoding client-greeting failed: " + ec.message());

    disconnect();

    return;
  }

  const auto greeting = std::move(decode_res.value().second);

  protocol_.client_capabilities(greeting.capabilities());

  if (protocol_.shared_capabilities().test(
          classic_protocol::capabilities::pos::ssl) &&
      !protocol_.connection().is_tls()) {
    protocol_.connection().init_tls();

    protocol_.connection().async_tls_accept([&](std::error_code ec) {
      if (ec) {
        if (ec != std::errc::operation_canceled) {
          logger_.warning("TLS accept failed: " + ec.message());
        }

        disconnect();
        return;
      }

      // read again other part
      client_greeting();
    });
    return;
  }

  // check what the test expects from the authentication

  auto handshake_data_res =
      json_reader_->handshake(false /* not is_greeting */);
  if (!handshake_data_res) {
    protocol_.encode_error(handshake_data_res.error());

    send_response_then_disconnect();

    return;
  }

  expected_handshake_ = handshake_data_res.value();

  protocol_.username(greeting.username());

  if (greeting.capabilities().test(
          classic_protocol::capabilities::pos::plugin_auth)) {
    protocol_.auth_method_name(greeting.auth_method_name());

    if (auto expected_auth_method_name =
            expected_handshake_->auth_method_name) {
      if (protocol_.auth_method_name() != *expected_auth_method_name) {
        // switch to the expected method.
        protocol_.auth_method_data(std::string(20, 'a'));
        protocol_.auth_method_name(*expected_auth_method_name);

        protocol_.encode_auth_switch_message(
            {protocol_.auth_method_name(),
             protocol_.auth_method_data() + std::string(1, '\0')});

        protocol_.async_send([this, to_send = protocol_.send_buffer().size()](
                                 std::error_code ec, size_t transferred) {
          if (ec) {
            if (ec != std::errc::operation_canceled) {
              logger_.warning("send auth result failed: " + ec.message());
            }

            disconnect();
            return;
          }

          if (to_send < transferred) {
            std::terminate();
          } else {
            auth_switched();
          }
        });
        return;
      }
    }

    /**
     * if the client client wants to switch to a method the server does not
     * understand, force the server's method.
     */
    if (protocol_.auth_method_name() != protocol_.server_auth_method_name()) {
      // auth_response() should be empty
      //
      // ask for the real full authentication
      protocol_.auth_method_data(std::string(20, 'a'));

      if (!(protocol_.auth_method_name() == CachingSha2Password::name ||
            protocol_.auth_method_name() == ClearTextPassword::name)) {
        protocol_.auth_method_name(CachingSha2Password::name);
      }

      protocol_.encode_auth_switch_message(
          {protocol_.auth_method_name(),
           protocol_.auth_method_data() + std::string(1, '\0')});

      protocol_.async_send([this, to_send = protocol_.send_buffer().size()](
                               std::error_code ec, size_t transferred) {
        if (ec) {
          if (ec != std::errc::operation_canceled) {
            logger_.warning("send auth result failed: " + ec.message());
          }

          disconnect();
          return;
        }

        if (to_send < transferred) {
          std::terminate();
        } else {
          auth_switched();
        }
      });
      return;
    }
  } else {
    // 4.1 or so
    protocol_.auth_method_name(MySQLNativePassword::name);
  }

  if (protocol_.auth_method_name() == CachingSha2Password::name) {
    // auth_response() should be empty
    //
    // ask for the real full authentication
    protocol_.auth_method_data(std::string(20, 'a'));

    protocol_.encode_auth_switch_message(
        {protocol_.auth_method_name(),
         protocol_.auth_method_data() + std::string(1, '\0')});

    protocol_.async_send([this, to_send = protocol_.send_buffer().size()](
                             std::error_code ec, size_t transferred) {
      if (ec) {
        if (ec != std::errc::operation_canceled) {
          logger_.warning("send auth result failed: " + ec.message());
        }

        disconnect();
        return;
      }

      if (to_send < transferred) {
        std::terminate();
      } else {
        auth_switched();
      }
    });
    return;
  } else if (protocol_.auth_method_name() == MySQLNativePassword::name ||
             protocol_.auth_method_name() == ClearTextPassword::name) {
    // authenticate wants a vector<uint8_t>
    auto client_auth_method_data = greeting.auth_method_data();
    std::vector<uint8_t> auth_method_data_vec(client_auth_method_data.begin(),
                                              client_auth_method_data.end());

    auto auth_res = authenticate(*expected_handshake_, auth_method_data_vec);

    if (!auth_res) {
      protocol_.encode_error(auth_res.error());

      send_response_then_disconnect();

      return;
    } else {
      OkResponse msg;

      if (protocol_.shared_capabilities().test(
              classic_protocol::capabilities::pos::session_track)) {
        msg.status_flags(
            1 << classic_protocol::status::pos::session_state_changed);
        msg.session_changes(encode_session_trackers({
            {classic_protocol::session_track::TransactionCharacteristics{""}},
        }));
      }

      protocol_.encode_ok(msg);

      send_response_then_idle();
      return;
    }
  } else {
    // switch to something we know.
    protocol_.auth_method_name("caching_sha2_password");

    // ask for the real full authentication
    protocol_.auth_method_data(std::string(20, 'a'));

    protocol_.encode_auth_switch_message(
        {protocol_.auth_method_name(),
         protocol_.auth_method_data() + std::string(1, '\0')});

    protocol_.async_send([this, to_send = protocol_.send_buffer().size()](
                             std::error_code ec, size_t transferred) {
      if (ec) {
        if (ec != std::errc::operation_canceled) {
          logger_.warning("send auto result failed: " + ec.message());
        }

        disconnect();
        return;
      }

      if (to_send < transferred) {
        std::terminate();
      } else {
        auth_switched();
      }
    });
    return;
  }
}

void MySQLServerMockSessionClassic::auth_switched() {
  // check we have enough of a client-greeting.
  std::vector<uint8_t> payload;
  auto frame_decode_res = protocol_.read_packet(payload);
  if (!frame_decode_res) {
    auto ec = frame_decode_res.error();

    if (ec == classic_protocol::codec_errc::not_enough_input) {
      protocol_.async_receive(
          [this](std::error_code ec, size_t /* transferred */) {
            if (ec) {
              disconnect();
              return;
            }

            auth_switched();
          });

      return;
    }

    disconnect();
    return;
  }

  // empty password is signaled by {0},
  // -> authenticate expects {}
  // -> client expects OK, instead of AUTH_FAST in this case
  bool empty_password = payload == std::vector<uint8_t>{0};
  auto auth_res = authenticate(
      *expected_handshake_, empty_password ? std::vector<uint8_t>{} : payload);

  if (!auth_res) {
    protocol_.encode_error(auth_res.error());

    send_response_then_disconnect();
    return;
  }

  if (protocol_.auth_method_name() == CachingSha2Password::name &&
      !empty_password) {
    // caching-sha2-password is special and needs the auth-fast state

    protocol_.encode_auth_fast_message();
  }

  OkResponse msg;
  if (protocol_.shared_capabilities().test(
          classic_protocol::capabilities::pos::session_track)) {
    msg.status_flags(1 << classic_protocol::status::pos::session_state_changed);
    msg.session_changes(encode_session_trackers({
        {classic_protocol::session_track::TransactionCharacteristics{""}},
    }));
  }

  protocol_.encode_ok(msg);

  send_response_then_idle();
}

void MySQLServerMockSessionClassic::send_response_then_disconnect() {
  protocol_.async_send([&, to_send = protocol_.send_buffer().size()](
                           std::error_code ec, size_t transferred) {
    if (ec) {
      if (ec != std::errc::operation_canceled) {
        logger_.warning("sending response failed: " + ec.message());
      }

      disconnect();
      return;
    }

    if (transferred < to_send) {
      // still some data to send.

      send_response_then_disconnect();
    } else {
      disconnect();
    }
  });
}

void MySQLServerMockSessionClassic::send_response_then_idle() {
  protocol_.async_send([&, to_send = protocol_.send_buffer().size()](
                           std::error_code ec, size_t transferred) {
    if (ec) {
      if (ec != std::errc::operation_canceled) {
        logger_.warning("sending response failed: " + ec.message());
      }

      disconnect();
      return;
    }

    if (transferred < to_send) {
      // still some data to send.

      send_response_then_idle();
    } else {
      // fetch the next statement.
      idle();
    }
  });
}

void MySQLServerMockSessionClassic::idle() {
  std::vector<uint8_t> payload;
  auto frame_decode_res = protocol_.read_packet(payload);
  if (!frame_decode_res) {
    auto ec = frame_decode_res.error();

    if (ec == classic_protocol::codec_errc::not_enough_input) {
      protocol_.async_receive([this](std::error_code ec,
                                     size_t /* transferred */) {
        if (ec) {
          if (ec != std::errc::operation_canceled &&
              ec != net::stream_errc::eof) {
            logger_.warning("receiving command-frame failed: " + ec.message());
          }
          disconnect();
          return;
        }

        idle();
      });

      return;
    }

    disconnect();

    return;
  }

  if (payload.empty()) {
    logger_.debug("message was empty, closing connection.");

    disconnect();

    return;
  }

  const auto cmd = payload[0];
  switch (cmd) {
    case classic_protocol::Codec<
        classic_protocol::message::client::Query>::cmd_byte(): {
      // skip the first (command) byte, rest is statement text
      std::string statement_received(std::next(payload.begin()), payload.end());

      try {
        // writes into send-buffer.
        //
        const auto started = std::chrono::steady_clock::now();

        json_reader_->handle_statement(statement_received, &protocol_);

        // handle_statement will set the exec-timer.
        protocol_.exec_timer().async_wait(
            [this, started,
             statement = statement_received](std::error_code ec) {
              // wait until exec-time passed.
              if (ec) {
                if (ec != std::errc::operation_canceled) {
                  logger_.warning("wait exec-time failed: " + ec.message());
                }
                disconnect();
                return;
              }

              auto now = std::chrono::steady_clock::now();
              logger_.info(mysql_harness::utility::string_format(
                  "(%s)> %s", duration_to_us_string(now - started).c_str(),
                  statement.c_str()));

              send_response_then_idle();
            });

      } catch (const std::exception &e) {
        // handling statement failed. Return the error to the client
        logger_.error("executing statement failed: "s + e.what());

        protocol_.encode_error(
            {ER_PARSE_ERROR, "executing statement failed: "s + e.what()});

        send_response_then_idle();

        return;
      }

      break;
    }
    case classic_protocol::Codec<
        classic_protocol::message::client::Quit>::cmd_byte():

      // wait until the client closed its side of the connection to prevent
      // TIME_WAIT on the mock-server.
      protocol_.async_receive([&](std::error_code ec, size_t transferred) {
        if (ec) {
          // EOF is expected, don't log it.
          if (ec != net::stream_errc::eof &&
              ec != std::errc::operation_canceled) {
            logger_.warning("receive connection-close failed: " + ec.message());
          }
        } else {
          // something _was_ sent? log it.
          logger_.debug(mysql_harness::utility::string_format(
              "data after QUIT: %zu", transferred));
        }

        disconnect();
      });

      return;
    case classic_protocol::Codec<
        classic_protocol::message::client::ResetConnection>::cmd_byte():

      protocol_.encode_ok({});

      send_response_then_idle();
      break;
    case classic_protocol::Codec<
        classic_protocol::message::client::ChangeUser>::cmd_byte(): {
      auto msg_res = classic_protocol::decode<
          classic_protocol::message::client::ChangeUser>(net::buffer(payload),
                                                         {});
      if (!msg_res) {
        protocol_.encode_error({ER_PARSE_ERROR, std::string("change-user: ") +
                                                    msg_res.error().message()});
        send_response_then_idle();
        break;
      }

      if (msg_res->second.auth_method_name() == CachingSha2Password::name) {
        protocol_.encode_auth_fast_message();
      }

      OkResponse msg;
      if (protocol_.shared_capabilities().test(
              classic_protocol::capabilities::pos::session_track)) {
        msg.status_flags(
            1 << classic_protocol::status::pos::session_state_changed);
        msg.session_changes(encode_session_trackers({
            {classic_protocol::session_track::TransactionCharacteristics{""}},
        }));
      }

      protocol_.encode_ok(msg);

      send_response_then_idle();
      break;
    }
    case classic_protocol::Codec<
        classic_protocol::message::client::Ping>::cmd_byte():

      protocol_.encode_ok({});

      send_response_then_idle();
      break;
    default:
      logger_.info("received unsupported command from the client: " +
                   std::to_string(cmd));

      protocol_.encode_error({ER_PARSE_ERROR,
                              "Unsupported command: " + std::to_string(cmd),
                              "HY000"});

      send_response_then_disconnect();

      return;
  }
}

void MySQLServerMockSessionClassic::finish() { disconnect(); }

void MySQLServerMockSessionClassic::run() { server_greeting(); }

void MySQLClassicProtocol::encode_auth_fast_message() {
  auto encode_res = classic_protocol::encode<classic_protocol::frame::Frame<
      classic_protocol::message::server::AuthMethodData>>(
      {seq_no_++, {"\x03"}}, shared_capabilities(),
      net::dynamic_buffer(send_buffer()));

  if (!encode_res) {
    // ignore
  }
}

void MySQLClassicProtocol::encode_auth_switch_message(
    const classic_protocol::message::server::AuthMethodSwitch &msg) {
  auto encode_res = classic_protocol::encode<classic_protocol::frame::Frame<
      classic_protocol::message::server::AuthMethodSwitch>>(
      {seq_no_++, msg}, shared_capabilities(),
      net::dynamic_buffer(send_buffer()));
  if (!encode_res) {
    // ignore
  }
}

void MySQLClassicProtocol::encode_server_greeting(
    const classic_protocol::message::server::Greeting &greeting) {
  server_capabilities_ = greeting.capabilities();

  auto encode_res = classic_protocol::encode<classic_protocol::frame::Frame<
      classic_protocol::message::server::Greeting>>(
      {seq_no_++, greeting}, server_capabilities(),
      net::dynamic_buffer(send_buffer()));
  if (!encode_res) {
    // ignore
  }
}

stdx::expected<std::string, std::error_code> cert_get_name(X509_NAME *name) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()),
                                                &BIO_free};
  // X509_NAME_oneline() is a legacy function and supposed to be not used for
  // new apps, but the server uses it, so we do to get the same serialization.
#if 0
  int res = X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_ONELINE);
  if (res <= 0) {
    return stdx::unexpected(make_tls_error());
  }

  BUF_MEM *buf;

  BIO_get_mem_ptr(bio.get(), &buf);

  return {std::in_place, buf->data, buf->data + buf->length};
#else
  std::array<char, 256> buf;

  return stdx::expected<std::string, std::error_code>{
      std::in_place, X509_NAME_oneline(name, buf.data(), buf.size())};
#endif
}

stdx::expected<std::string, std::error_code> cert_get_subject_name(X509 *cert) {
  return cert_get_name(X509_get_subject_name(cert));
}

stdx::expected<std::string, std::error_code> cert_get_issuer_name(X509 *cert) {
  return cert_get_name(X509_get_issuer_name(cert));
}

stdx::expected<void, ErrorResponse> MySQLServerMockSessionClassic::authenticate(
    const StatementReaderBase::handshake_data &handshake,
    const std::vector<uint8_t> &client_auth_method_data) {
  if (handshake.username.has_value()) {
    if (handshake.username.value() != protocol_.username()) {
      return stdx::unexpected(ErrorResponse{
          ER_ACCESS_DENIED_ERROR,  // 1045
          "Access Denied for user '" + protocol_.username() + "'@'localhost'",
          "28000"});
    }
  }

  if (handshake.password.has_value()) {
    if (!MySQLClassicProtocol::authenticate(
            protocol_.auth_method_name(), protocol_.auth_method_data(),
            handshake.password.value(), client_auth_method_data)) {
      return stdx::unexpected(ErrorResponse{
          ER_ACCESS_DENIED_ERROR,  // 1045
          "Access Denied for user '" + protocol_.username() + "'@'localhost'",
          "28000"});
    }
  }

  if (handshake.cert_required) {
    auto *ssl = protocol_.connection().ssl();

    std::unique_ptr<X509, decltype(&X509_free)> client_cert{
        SSL_get_peer_certificate(ssl), &X509_free};
    if (!client_cert) {
      logger_.info("cert required, no cert received.");
      return stdx::unexpected(ErrorResponse{
          ER_ACCESS_DENIED_ERROR,  // 1045
          "Access Denied for user '" + protocol_.username() + "'@'localhost'",
          "28000"});
    }

    if (handshake.cert_subject.has_value()) {
      auto subject_res = cert_get_subject_name(client_cert.get());
      if (!subject_res) {
        throw std::system_error(subject_res.error(), "cert_get_subject_name");
      }
      logger_.debug("client-cert::subject: " + subject_res.value());

      if (handshake.cert_subject.value() != subject_res.value()) {
        return stdx::unexpected(ErrorResponse{
            ER_ACCESS_DENIED_ERROR,  // 1045
            "Access Denied for user '" + protocol_.username() + "'@'localhost'",
            "28000"});
      }
    }

    if (handshake.cert_issuer.has_value()) {
      auto issuer_res = cert_get_issuer_name(client_cert.get());
      if (!issuer_res) {
        throw std::system_error(issuer_res.error(), "cert_get_issuer_name");
      }
      logger_.debug("client-cert::issuer: " + issuer_res.value());

      if (handshake.cert_issuer.value() != issuer_res.value()) {
        return stdx::unexpected(ErrorResponse{
            ER_ACCESS_DENIED_ERROR,  // 1045
            "Access Denied for user '" + protocol_.username() + "'@'localhost'",
            "28000"});
      }
    }

    const auto verify_res = SSL_get_verify_result(protocol_.connection().ssl());

    if (verify_res != X509_V_OK) {
      logger_.info("ssl-verify failed: " + std::to_string(verify_res));

      return stdx::unexpected(ErrorResponse{
          ER_ACCESS_DENIED_ERROR,  // 1045
          "Access Denied for user '" + protocol_.username() + "'@'localhost'",
          "28000"});
    }
  }

  return {};
}

void MySQLClassicProtocol::encode_error(const ErrorResponse &msg) {
  auto encode_res = classic_protocol::encode<
      classic_protocol::frame::Frame<classic_protocol::message::server::Error>>(
      {seq_no_++, msg}, shared_capabilities(),
      net::dynamic_buffer(send_buffer()));

  if (!encode_res) {
    //
    return;
  }
}

void MySQLClassicProtocol::encode_ok(const OkResponse &msg) {
  auto tmp_msg = msg;

  if (shared_capabilities().test(
          classic_protocol::capabilities::pos::session_track) &&
      !msg.session_changes().empty()) {
    tmp_msg.status_flags(msg.status_flags().set(
        classic_protocol::status::pos::session_state_changed));
  }

  auto encode_res = classic_protocol::encode<
      classic_protocol::frame::Frame<classic_protocol::message::server::Ok>>(
      {seq_no_++, tmp_msg}, shared_capabilities(),
      net::dynamic_buffer(send_buffer()));

  if (!encode_res) {
    //
    return;
  }
}

void MySQLClassicProtocol::encode_resultset(const ResultsetResponse &response) {
  const auto shared_caps = shared_capabilities();

  auto encode_res = classic_protocol::encode<
      classic_protocol::frame::Frame<classic_protocol::wire::VarInt>>(
      {seq_no_++, {static_cast<long>(response.columns.size())}}, shared_caps,
      net::dynamic_buffer(send_buffer()));
  if (!encode_res) {
    //
    return;
  }

  for (const auto &column : response.columns) {
    encode_res = classic_protocol::encode<classic_protocol::frame::Frame<
        classic_protocol::message::server::ColumnMeta>>(
        {seq_no_++, column}, shared_caps, net::dynamic_buffer(send_buffer()));
    if (!encode_res) {
      //
      return;
    }
  }

  if (!shared_caps.test(classic_protocol::capabilities::pos::
                            text_result_with_session_tracking)) {
    encode_res = classic_protocol::encode<
        classic_protocol::frame::Frame<classic_protocol::message::server::Eof>>(
        {seq_no_++, {}}, shared_caps, net::dynamic_buffer(send_buffer()));
    if (!encode_res) {
      //
      return;
    }
  }

  for (auto const &row : response.rows) {
    encode_res = classic_protocol::encode<
        classic_protocol::frame::Frame<classic_protocol::message::server::Row>>(
        {seq_no_++, {row}}, shared_caps, net::dynamic_buffer(send_buffer()));
    if (!encode_res) {
      //
      return;
    }
  }

  encode_res = classic_protocol::encode<
      classic_protocol::frame::Frame<classic_protocol::message::server::Eof>>(
      {seq_no_++, response.end_of_rows}, shared_caps,
      net::dynamic_buffer(send_buffer()));
  if (!encode_res) {
    //
    return;
  }
}

}  // namespace server_mock
