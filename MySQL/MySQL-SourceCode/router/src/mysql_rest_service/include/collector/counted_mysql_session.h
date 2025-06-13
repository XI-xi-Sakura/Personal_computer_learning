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

#ifndef ROUTER_SRC_MYSQL_REST_SERVICE_SRC_COLLECTOR_MYSQL_SQL_SESSION_H_
#define ROUTER_SRC_MYSQL_REST_SERVICE_SRC_COLLECTOR_MYSQL_SQL_SESSION_H_

#include "mysql/harness/destination.h"
#include "mysqlrouter/mysql_session.h"

#include <vector>

#include "secure_string.h"  // NOLINT(build/include_subdir)

namespace collector {

class CountedMySQLSession : public mysqlrouter::MySQLSession {
 public:
  using Sqls = std::vector<std::string>;
  struct ConnectionParameters {
    struct SslOptions {
      mysql_ssl_mode ssl_mode;
      std::string tls_version;
      std::string ssl_cipher;
      std::string ca;
      std::string capath;
      std::string crl;
      std::string crlpath;
    } ssl_opts;
    struct SslCert {
      std::string cert;
      std::string key;
    } ssl_cert;
    struct ConnOptions {
      mysql_harness::Destination destination{mysql_harness::TcpDestination{}};

      std::string username;
      mysql_harness::SecureString password;
      std::string default_schema;
      int connect_timeout{kDefaultConnectTimeout};
      int read_timeout{kDefaultReadTimeout};
      unsigned long extra_client_flags{0};
    } conn_opts;
  };

  CountedMySQLSession();
  ~CountedMySQLSession() override;

  virtual void allow_failure_at_next_query();
  virtual ConnectionParameters get_connection_parameters() const;
  virtual void execute_initial_sqls();
  virtual Sqls get_initial_sqls() const;
  virtual void connect_and_set_opts(
      const ConnectionParameters &connection_params, const Sqls &initial_sqls);

  void connect(const MySQLSession &other, const std::string &username,
               const mysql_harness::SecureString &password) override;
  void connect(const std::string &host, unsigned int port,
               const std::string &username,
               const mysql_harness::SecureString &password,
               const std::string &unix_socket,
               const std::string &default_schema,
               int connect_timeout = kDefaultConnectTimeout,
               int read_timeout = kDefaultReadTimeout,
               unsigned long extra_client_flags = 0) override;

  void change_user(const std::string &user,
                   const mysql_harness::SecureString &password,
                   const std::string &db) override;
  void reset() override;
  uint64_t prepare(const std::string &query) override;
  void prepare_execute_with_bind_parameters(
      uint64_t ps_id, std::vector<MYSQL_BIND> bind_parameters,
      const ResultRowProcessor &processor, const FieldValidator &validator,
      const OnResultSetEnd &on_resultset_end) override;
  void prepare_remove(uint64_t ps_id) override;

  void execute(
      const std::string &query) override;  // throws Error, std::logic_error

  void query(const std::string &query, const ResultRowProcessor &processor,
             const FieldValidator &validator)
      override;  // throws Error, std::logic_error
  std::unique_ptr<MySQLSession::ResultRow> query_one(
      const std::string &query,
      const FieldValidator &validator) override;  // throws Error
  std::unique_ptr<MySQLSession::ResultRow> query_one(
      const std::string &query) override;  // throws Error

 private:
  ConnectionParameters connection_params_;
  bool reconnect_at_next_query_{false};
  Sqls initial_sqls_;
};

}  // namespace collector

#endif  // ROUTER_SRC_MYSQL_REST_SERVICE_SRC_COLLECTOR_MYSQL_SQL_SESSION_H_
