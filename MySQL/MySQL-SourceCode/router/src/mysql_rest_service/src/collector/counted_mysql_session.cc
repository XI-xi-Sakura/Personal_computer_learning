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

#include "collector/counted_mysql_session.h"

#include "mrs/router_observation_entities.h"
#include "mysql/harness/destination.h"

namespace collector {

static bool did_server_disconnect(unsigned int code) {
  switch (code) {
    case CR_SERVER_GONE_ERROR:
    case CR_SERVER_LOST:
    case CR_SERVER_LOST_EXTENDED:
      return true;
    default:
      return false;
  }
}

class GuardBoolSetFalse {
 public:
  GuardBoolSetFalse(bool &value) : value_{value} {}
  ~GuardBoolSetFalse() { value_ = false; }

  bool &value_;
};

#define CHECK_DISCONNECTION_RETURN(X)                                       \
  {                                                                         \
    GuardBoolSetFalse guard{reconnect_at_next_query_};                      \
    try {                                                                   \
      return X;                                                             \
    } catch (const CountedMySQLSession::Error &e) {                         \
      if (reconnect_at_next_query_ && did_server_disconnect(e.code())) {    \
        try {                                                               \
          connect_and_set_opts(get_connection_parameters(), initial_sqls_); \
        } catch (...) {                                                     \
          throw e;                                                          \
        }                                                                   \
        return X;                                                           \
      }                                                                     \
      throw;                                                                \
    }                                                                       \
  }

#define CHECK_DISCONNECTION_VOID(X)                                         \
  {                                                                         \
    GuardBoolSetFalse guard{reconnect_at_next_query_};                      \
    try {                                                                   \
      X;                                                                    \
    } catch (const CountedMySQLSession::Error &e) {                         \
      if (reconnect_at_next_query_ && did_server_disconnect(e.code())) {    \
        try {                                                               \
          connect_and_set_opts(get_connection_parameters(), initial_sqls_); \
        } catch (...) {                                                     \
          throw e;                                                          \
        }                                                                   \
        X;                                                                  \
      }                                                                     \
      throw;                                                                \
    }                                                                       \
  }

using MySQLSession = mysqlrouter::MySQLSession;

CountedMySQLSession::CountedMySQLSession() {
  mrs::Counter<kEntityCounterMySQLConnectionsActive>::increment();
}

CountedMySQLSession::~CountedMySQLSession() {
  mrs::Counter<kEntityCounterMySQLConnectionsActive>::increment(-1);
}

void CountedMySQLSession::allow_failure_at_next_query() {
  reconnect_at_next_query_ = true;
}

CountedMySQLSession::ConnectionParameters
CountedMySQLSession::get_connection_parameters() const {
  return connection_params_;
}

void CountedMySQLSession::execute_initial_sqls() {
  for (const auto &sql : initial_sqls_) {
    execute(sql);
  }
}

CountedMySQLSession::Sqls CountedMySQLSession::get_initial_sqls() const {
  return initial_sqls_;
}

void CountedMySQLSession::connect_and_set_opts(
    const ConnectionParameters &connection_params, const Sqls &initial_sqls) {
  connection_params_ = connection_params;
  initial_sqls_ = initial_sqls;

  std::string host;
  uint16_t port{};
  std::string unix_socket;

  if (connection_params.conn_opts.destination.is_tcp()) {
    const auto dest = connection_params.conn_opts.destination.as_tcp();
    host = dest.hostname();
    port = dest.port();
  } else {
    const auto dest = connection_params.conn_opts.destination.as_local();
    unix_socket = dest.path();
  }

  const auto &opt = connection_params.ssl_opts;
  set_ssl_options(opt.ssl_mode, opt.tls_version, opt.ssl_cipher, opt.ca,
                  opt.capath, opt.crl, opt.crlpath);

  connect(host, port, connection_params_.conn_opts.username,
          connection_params_.conn_opts.password, unix_socket,
          connection_params_.conn_opts.default_schema,
          connection_params_.conn_opts.connect_timeout,
          connection_params_.conn_opts.read_timeout,
          connection_params_.conn_opts.extra_client_flags);
  reconnect_at_next_query_ = false;
  execute_initial_sqls();
}

void CountedMySQLSession::connect(const std::string &host, unsigned int port,
                                  const std::string &username,
                                  const mysql_harness::SecureString &password,
                                  const std::string &unix_socket,
                                  const std::string &default_schema,
                                  int connect_timeout, int read_timeout,
                                  unsigned long extra_client_flags) {
  MySQLSession::connect(host, port, username, password, unix_socket,
                        default_schema, connect_timeout, read_timeout,
                        extra_client_flags);
  reconnect_at_next_query_ = false;

  auto dest = !unix_socket.empty()
                  ? mysql_harness::Destination(
                        mysql_harness::LocalDestination(unix_socket))
                  : mysql_harness::Destination(
                        mysql_harness::TcpDestination(host, port));

  connection_params_.conn_opts.destination = dest;
  connection_params_.conn_opts.username = username;
  connection_params_.conn_opts.password = password;
  connection_params_.conn_opts.default_schema = default_schema;
  connection_params_.conn_opts.connect_timeout = connect_timeout;
  connection_params_.conn_opts.read_timeout = read_timeout;
  connection_params_.conn_opts.extra_client_flags = extra_client_flags;
}

void CountedMySQLSession::connect(const MySQLSession &other,
                                  const std::string &username,
                                  const mysql_harness::SecureString &password) {
  auto other_counted = dynamic_cast<const CountedMySQLSession *>(&other);
  assert(other_counted && "Must be instance of CountedMySQLSession.");
  auto params = other_counted->get_connection_parameters();
  auto sqls = other_counted->get_initial_sqls();
  params.conn_opts.username = username;
  params.conn_opts.password = password;

  connect_and_set_opts(params, sqls);
}

void CountedMySQLSession::change_user(
    const std::string &user, const mysql_harness::SecureString &password,
    const std::string &db) {
  mrs::Counter<kEntityCounterMySQLChangeUser>::increment();
  CHECK_DISCONNECTION_VOID(MySQLSession::change_user(user, password, db));
  connection_params_.conn_opts.username = user;
  connection_params_.conn_opts.password = password;
  connection_params_.conn_opts.default_schema = db;
}

void CountedMySQLSession::reset() { MySQLSession::reset(); }

uint64_t CountedMySQLSession::prepare(const std::string &query) {
  mrs::Counter<kEntityCounterMySQLPrepare>::increment();
  CHECK_DISCONNECTION_RETURN(MySQLSession::prepare(query));
}

void CountedMySQLSession::prepare_execute_with_bind_parameters(
    uint64_t ps_id, std::vector<MYSQL_BIND> bind_parameters,
    const ResultRowProcessor &processor, const FieldValidator &validator,
    const OnResultSetEnd &on_resultset_end) {
  mrs::Counter<kEntityCounterMySQLPrepareExecute>::increment();
  CHECK_DISCONNECTION_VOID(MySQLSession::prepare_execute_with_bind_parameters(
      ps_id, bind_parameters, processor, validator, on_resultset_end));
}

void CountedMySQLSession::prepare_remove(uint64_t ps_id) {
  mrs::Counter<kEntityCounterMySQLPrepareRemove>::increment();
  CHECK_DISCONNECTION_VOID(MySQLSession::prepare_remove(ps_id));
}

void CountedMySQLSession::execute(const std::string &query) {
  mrs::Counter<kEntityCounterMySQLQueries>::increment();
  CHECK_DISCONNECTION_VOID(MySQLSession::execute(query));
}

void CountedMySQLSession::query(const std::string &query,
                                const ResultRowProcessor &processor,
                                const FieldValidator &validator) {
  mrs::Counter<kEntityCounterMySQLQueries>::increment();
  CHECK_DISCONNECTION_VOID(MySQLSession::query(query, processor, validator));
}

std::unique_ptr<MySQLSession::ResultRow> CountedMySQLSession::query_one(
    const std::string &query, const FieldValidator &validator) {
  mrs::Counter<kEntityCounterMySQLQueries>::increment();
  CHECK_DISCONNECTION_RETURN(MySQLSession::query_one(query, validator));
}

std::unique_ptr<MySQLSession::ResultRow> CountedMySQLSession::query_one(
    const std::string &query) {
  // It calls query_one with two arguments. There is no need to count this
  // call.
  CHECK_DISCONNECTION_RETURN(MySQLSession::query_one(query));
}

}  // namespace collector
