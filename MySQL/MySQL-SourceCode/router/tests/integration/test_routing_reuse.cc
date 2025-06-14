/*
  Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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

#include <algorithm>  // min
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

#include <gmock/gmock-matchers.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/message.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/text_format.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <mysqlx_crud.pb.h>
#include <mysqlx_datatypes.pb.h>
#include <mysqlx_expr.pb.h>

#include "mysql/harness/destination.h"
#include "mysql/harness/filesystem.h"
#include "mysql/harness/net_ts/impl/socket.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/stdx/expected_ostream.h"
#include "mysql/harness/tls_context.h"
#include "mysql/harness/utility/string.h"  // join
#include "mysqlxclient.h"
#include "mysqlxclient/xerror.h"
#include "mysqlxclient/xquery_result.h"
#include "openssl_version.h"  // ROUTER_OPENSSL_VERSION
#include "process_manager.h"
#include "procs.h"
#include "router/src/routing/tests/mysql_client.h"
#include "router_component_test.h"
#include "scope_guard.h"
#include "stdx_expected_no_error.h"
#include "tcp_port_pool.h"

using namespace std::string_literals;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

using ::testing::ElementsAre;

static constexpr const std::string_view kDisabled{"DISABLED"};
static constexpr const std::string_view kRequired{"REQUIRED"};
static constexpr const std::string_view kPreferred{"PREFERRED"};
static constexpr const std::string_view kPassthrough{"PASSTHROUGH"};
static constexpr const std::string_view kAsClient{"AS_CLIENT"};

#ifdef _WIN32
#define EXE_EXTENSION ".exe"
#define SO_EXTENSION ".dll"
#else
#define EXE_EXTENSION ""
#define SO_EXTENSION ".so"
#endif

std::ostream &operator<<(std::ostream &os, MysqlError e) {
  os << e.sql_state() << " (" << e.value() << ") " << e.message();
  return os;
}

/**
 * convert a multi-resultset into a simple container which can be EXPECTed
 * against.
 */
static std::vector<std::vector<std::vector<std::string>>> result_as_vector(
    const MysqlClient::Statement::Result &results) {
  std::vector<std::vector<std::vector<std::string>>> resultsets;

  for (const auto &result : results) {
    std::vector<std::vector<std::string>> res_;

    const auto field_count = result.field_count();

    for (const auto &row : result.rows()) {
      std::vector<std::string> row_;
      row_.reserve(field_count);

      for (unsigned int ndx = 0; ndx < field_count; ++ndx) {
        auto fld = row[ndx];

        row_.emplace_back(fld == nullptr ? "<NULL>" : fld);
      }

      res_.push_back(std::move(row_));
    }
    resultsets.push_back(std::move(res_));
  }

  return resultsets;
}

static stdx::expected<std::vector<std::vector<std::string>>, MysqlError>
query_one_result(MysqlClient &cli, std::string_view stmt) {
  auto cmd_res = cli.query(stmt);
  if (!cmd_res) return stdx::unexpected(cmd_res.error());

  auto results = result_as_vector(*cmd_res);
  if (results.size() != 1) {
    return stdx::unexpected(MysqlError{1, "Too many results", "HY000"});
  }

  return results.front();
}

/*
 * collect parse errors into a string.
 *
 * one line per error/warning.
 */
class StringErrorCollector : public google::protobuf::io::ErrorCollector {
 public:
#if (GOOGLE_PROTOBUF_VERSION >= 4024000)
  void RecordError(int line, google::protobuf::io::ColumnNumber column,
                   absl::string_view msg) override
#else
  void AddError(int line, google::protobuf::io::ColumnNumber column,
                const std::string &msg) override
#endif  // (GOOGLE_PROTOBUF_VERSION >= 4024000)
  {
    std::ostringstream ss;

    ss << "ERROR: " << line << ":" << column << ": " << msg;
    lines_.push_back(ss.str());
  }
#if (GOOGLE_PROTOBUF_VERSION >= 4024000)
  void RecordWarning(int line, google::protobuf::io::ColumnNumber column,
                     absl::string_view msg) override
#else
  void AddWarning(int line, google::protobuf::io::ColumnNumber column,
                  const std::string &msg) override
#endif  // (GOOGLE_PROTOBUF_VERSION >= 4024000)
  {
    std::ostringstream ss;

    ss << "WARN: " << line << ":" << column << ": " << msg;
    lines_.push_back(ss.str());
  }

  std::string to_string() const { return mysql_harness::join(lines_, "\n"); }

  void clear() { lines_.clear(); }

 private:
  std::list<std::string> lines_;
};

/*
 * parse a protobuf message from text-proto format.
 *
 * stores errors in a string.
 */
class TextFormatParser {
 public:
  TextFormatParser() { parser_.RecordErrorsTo(&errors_); }

  bool ParseFromString(const std::string &input,
                       google::protobuf::Message *msg) {
    return parser_.ParseFromString(input, msg);
  }

  StringErrorCollector &errors() & { return errors_; }

 private:
  google::protobuf::TextFormat::Parser parser_;

  StringErrorCollector errors_;
};

struct ReuseConnectionParam {
  std::string testname;

  std::string_view client_ssl_mode;
  std::string_view server_ssl_mode;
};

const ReuseConnectionParam reuse_connection_params[] = {
    // DISABLED
    {
        "DISABLED__DISABLED",
        kDisabled,  // client_ssl_mode
        kDisabled,  // server_ssl_mode
    },
    {
        "DISABLED__AS_CLIENT",
        kDisabled,
        kAsClient,
    },
    {
        "DISABLED__REQUIRED",
        kDisabled,
        kRequired,
    },
    {
        "DISABLED__PREFERRED",
        kDisabled,
        kPreferred,
    },

    // PASSTHROUGH
    {
        "PASSTHROUGH__AS_CLIENT",
        kPassthrough,
        kAsClient,
    },

    // PREFERRED
    {
        "PREFERRED__DISABLED",
        kPreferred,
        kDisabled,
    },
    {
        "PREFERRED__AS_CLIENT",
        kPreferred,
        kAsClient,
    },
    {
        "PREFERRED__PREFERRED",
        kPreferred,
        kPreferred,
    },
    {
        "PREFERRED__REQUIRED",
        kPreferred,
        kRequired,
    },

    // REQUIRED ...
    {
        "REQUIRED__DISABLED",
        kRequired,
        kDisabled,
    },
    {
        "REQUIRED__AS_CLIENT",
        kRequired,
        kAsClient,
    },
    {
        "REQUIRED__PREFERRED",
        kRequired,
        kPreferred,
    },
    {
        "REQUIRED__REQUIRED",
        kRequired,
        kRequired,
    },
};

const std::array is_tcp_values = {
    true,
#ifndef _WIN32
    // no unix-socket support on windows.
    false,
#endif
};

/**
 * start once, use often.
 */
class SharedServer {
 public:
  SharedServer(TcpPortPool &port_pool) : port_pool_(port_pool) {}

  [[nodiscard]] std::string mysqld_dir_name() const {
    return mysqld_dir_.name();
  }

  integration_tests::Procs &process_manager() { return procs_; }

  void initialize_server() {
    auto bindir = process_manager().get_origin();
    auto mysqld = bindir.join("mysqld");

    if (!mysqld.exists()) {
      mysqld_failed_to_start_ = true;
      return;
    }

    auto &proc =
        process_manager()
            .spawner(mysqld.str())
            .wait_for_sync_point(ProcessManager::Spawner::SyncPoint::NONE)
            .spawn({
                "--no-defaults",
                "--initialize-insecure",
                "--datadir=" + mysqld_dir_name(),
                "--log-error=" + mysqld_dir_name() +
                    mysql_harness::Path::directory_separator + "mysqld.err",
            });
    proc.set_logging_path(mysqld_dir_name(), "mysqld.err");
    try {
      proc.wait_for_exit(90s);

      if (proc.exit_code() != 0) mysqld_failed_to_start_ = true;
    } catch (const std::exception &) {
      process_manager().dump_logs();

      mysqld_failed_to_start_ = true;
    }
  }

  void spawn_server() {
    SCOPED_TRACE("// start server");

    auto bindir = process_manager().get_origin();
    auto builddir = bindir.join("..");
    auto sharedir = builddir.join("share");
    auto lc_messages_dir = sharedir;

    auto lc_messages80_dir = sharedir.join("mysql-8.0");

    if (lc_messages80_dir.join("english").join("errmsg.sys").exists()) {
      lc_messages_dir = lc_messages80_dir;
    }

    classic_socket_dest_ = Path(mysqld_dir_name()).join("mysql.sock").str();
    x_socket_dest_ = Path(mysqld_dir_name()).join("mysqlx.sock").str();

    auto &proc =
        process_manager()
            .spawner(bindir.join("mysqld").str())
#ifdef _WIN32
            // on windows, mysqld has no notify-socket
            .wait_for_sync_point(ProcessManager::Spawner::SyncPoint::NONE)
            // on windows, ctrl+c leads to return value 0xc000013a
            // (STATUS_CONTROL_C_EXIT)
            .expected_exit_code(ExitStatus{ExitStatus::terminated_t{},
                                           static_cast<int>(0xc000013a)})
#endif
            .spawn({
                "--no-defaults",
                "--lc-messages-dir=" + lc_messages_dir.str(),
                "--datadir=" + mysqld_dir_name(),
                "--log-error=" + mysqld_dir_name() +
                    mysql_harness::Path::directory_separator + "mysqld.err",
                "--port=" + std::to_string(classic_tcp_destination().port()),
                // defaults to {datadir}/mysql.socket
                "--socket=" + classic_socket_dest_.path(),
                "--mysqlx-port=" + std::to_string(x_tcp_destination().port()),
                // defaults to {datadir}/mysqlx.socket
                "--mysqlx-socket=" + x_socket_dest_.path(),
                // disable LOAD DATA/SELECT INTO on the server
                "--secure-file-priv=NULL",
                "--require-secure-transport=OFF",
            });
    proc.set_logging_path(mysqld_dir_name(), "mysqld.err");
    if (!proc.wait_for_sync_point_result()) mysqld_failed_to_start_ = true;

#ifdef _WIN32
    // on windows, wait until port is ready as there is no notify-socket.
    if (!(wait_for_port_ready(classic_tcp_destination().port(), 10s) &&
          wait_for_port_ready(x_tcp_destination().port(), 10s))) {
      mysqld_failed_to_start_ = true;
    }
#endif
  }

  struct Account {
    Account(std::string usr, std::string pwd, std::string with)
        : username(std::move(usr)),
          password(std::move(pwd)),
          auth_method(std::move(with)) {}

    Account(std::string usr, std::string pwd, std::string with,
            std::optional<std::string> as)
        : username(std::move(usr)),
          password(std::move(pwd)),
          auth_method(std::move(with)),
          identified_as(std::move(as)) {}

    std::string username;
    std::string password;
    std::string auth_method;

    std::optional<std::string> identified_as;
  };

  stdx::expected<MysqlClient, MysqlError> admin_cli() {
    MysqlClient cli;

    auto account = SharedServer::admin_account();
    cli.username(account.username);
    cli.password(account.password);

    auto dest = classic_tcp_destination();

    auto connect_res = cli.connect(dest.hostname(), dest.port());
    if (!connect_res) return stdx::unexpected(connect_res.error());

    return cli;
  }

  stdx::expected<std::unique_ptr<xcl::XSession>, xcl::XError> admin_xcli() {
    auto dest = x_tcp_destination();

    auto sess = xcl::create_session();

    auto account = SharedServer::admin_account();
    auto xerr =
        sess->connect(dest.hostname().c_str(), dest.port(),
                      account.username.c_str(), account.password.c_str(), "");

    if (xerr.error() != 0) return stdx::unexpected(xerr);

    return sess;
  }

  void create_account(MysqlClient &cli, Account account) {
    {
      std::string q = "CREATE USER " + account.username + " " +  //
                      "IDENTIFIED WITH " + account.auth_method;

      if (!account.password.empty()) {
        q += " BY '" + account.password + "'";
      }

      if (account.identified_as) {
        q += " AS '" + *account.identified_as + "'";
      }

      SCOPED_TRACE("// " + q);
      auto res = cli.query(q);

      ASSERT_NO_ERROR(res) << "\n" << q;
    }

    {
      const std::string q = "GRANT ALL ON xproto.* TO " + account.username;

      SCOPED_TRACE("// " + q);
      auto res = cli.query(q);

      ASSERT_NO_ERROR(res) << "\n" << q;
    }
  }

  void drop_account(MysqlClient &cli, Account account) {
    const std::string q = "DROP USER " + account.username;

    SCOPED_TRACE("// " + q);
    auto res = cli.query(q);

    ASSERT_NO_ERROR(res) << "\n" << q;
  }

  void setup_mysqld_accounts() {
    auto cli_res = admin_cli();
    ASSERT_NO_ERROR(cli_res);

    auto cli = std::move(cli_res.value());

    // openid_connect
    //
    auto install_res = cli.query(
        "INSTALL PLUGIN authentication_openid_connect"
        "        SONAME 'authentication_openid_connect" SO_EXTENSION "'");

    if (install_res) {
      has_openid_connect_ = true;
    } else {
      // can't open shared object file.
      ASSERT_EQ(install_res.error().value(), 1126);
    }

    if (has_openid_connect()) {
      std::string set_openid_connect_config(R"(
{ "myissuer" : "{\"kid\":\"6f7254101f56e41cf35c9926de84a2d552b4c6f1\",\"e\":\"AQAB\",\"name\":\"https://myissuer.com\",\"alg\":\"RS256\",\"use\":\"sig\",\"n\":\"oEpcwfsGjBWzWanhb-WNGy4NgPFXOztLiZOZUWFZh25Vgny0YIlVPwtNRqqXgiyvVYzp-uMD7noQl8FUkqNM22NgjpzOWZAcIwc103qxgNr_kIV8__5uDu-ppl5qnHIEYP_IW9_uBpzJ_L2oZjv-AoSCvHiIFpcg9lq5gxKVe9A8FuCGfQ2rodlYqUC2qha0CTwgbUIT9H3469gpoU88AXiHDC90Dsi8Wpa5D1aNGJ8VbPl9CzyMWp-evHmtfDzNzz9yKF7JKExU6pBjG9HsQ0CEW9_8LtQ6NZrt6o3pQoMm8gjUScrUJnrfN16k0q8hfFuewQi5syV0GBlPg6en1w\",\"kty\":\"RSA\"}", "authService.oracle.com": "{\"alg\":\"RS256\",\"use\":\"sig\",\"kty\":\"RSA\",\"e\":\"AQAB\",\"kid\":\"967ea044-88bc-47d7-b286-52b87d0f08a5\",\"n\":\"nSfpzwAHkXy7NPxAh_SyLklu_l1d1hYhWjWl35HIeKMtvlr5oYWAGpbB19EMrkdCcxrXH8kIMhQ9rbmnn9BtaiQ6qbhQgPhBjJfq7k9-csn-qHWpNbALpLY5EuF7ZJQr-Ith13iEAG_qXoapDesWYwBNHDG6muKKeVYdiLc_AsP4CXYtt1emHKIt1zEqFFBJo2tiooXf_oRvC9d_U5lWU0NiSz6yT8z9-4g7XrdDtETmkL--EJLzhywIItuRTykkxPOWOCesSz1BQWcS6y0oTVKE5FNpUCWydvvzataERq5jHd61HbTKw0casV9Lod5MwGFG1dIDk7x8qt0ptOBleQ\"}" }
)");

      std::string set_openid_connect_config_stmt(
          "SET GLOBAL authentication_openid_connect_configuration = \"JSON://" +
          cli.escape(set_openid_connect_config) + "\"");
      SCOPED_TRACE("// " + set_openid_connect_config_stmt);
      ASSERT_NO_ERROR(cli.query(set_openid_connect_config_stmt));
    }

    ASSERT_NO_FATAL_FAILURE(
        create_account(cli, caching_sha2_password_account()));
    ASSERT_NO_FATAL_FAILURE(
        create_account(cli, caching_sha2_empty_password_account()));
    ASSERT_NO_FATAL_FAILURE(create_account(cli, sha256_password_account()));

    ASSERT_NO_FATAL_FAILURE(
        create_account(cli, sha256_empty_password_account()));

    if (has_openid_connect()) {
      ASSERT_NO_FATAL_FAILURE(create_account(cli, openid_connect_account()));
    }
  }

  void setup_mysqld_xproto_test_env() {
    auto sess_res = admin_xcli();
    ASSERT_NO_ERROR(sess_res);

    auto sess = std::move(sess_res.value());

    SCOPED_TRACE("// create schema");
    {
      xcl::XError xerr;
      std::vector<xcl::Argument_value> args;

      sess->execute_sql("CREATE SCHEMA xproto", &xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// create collection");

    {
      xcl::XError xerr;

      sess->execute_stmt(
          "mysqlx", "create_collection",
          {
              xcl::Argument_value(xcl::Argument_object{
                  {"name", xcl::Argument_value("somecollection")},
                  {"schema", xcl::Argument_value("xproto")},
              }),
          },
          &xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    {
      xcl::XError xerr;

      Mysqlx::Crud::Insert msg;

      {
        auto *coll = msg.mutable_collection();
        {
          auto *name = coll->mutable_name();
          name->assign("somecollection");
        }
        {
          auto *schema = coll->mutable_schema();
          schema->assign("xproto");
        }
      }

      // row
      {
        auto *row = msg.mutable_row();
        {
          auto *field = row->Add();
          auto *expr = field->add_field();
          expr->set_type(Mysqlx::Expr::Expr_Type::Expr_Type_LITERAL);

          auto scalar = expr->mutable_literal();
          scalar->set_type(
              Mysqlx::Datatypes::Scalar::Type::Scalar_Type_V_STRING);
          {
            auto *json_v = new Mysqlx::Datatypes::Scalar_String;
            json_v->set_value("{}");
            scalar->set_allocated_v_string(std::move(json_v));
          }
        }
      }

      sess->get_protocol().execute_insert(msg, &xerr);
      ASSERT_THAT(xerr.error(), 0) << xerr;
    }
  }

  void flush_prileges() {
    auto sess_res = admin_xcli();
    ASSERT_NO_ERROR(sess_res);

    auto sess = std::move(sess_res.value());

    xcl::XError xerr;
    sess->execute_sql("FLUSH PRIVILEGES", &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;
  }

  [[nodiscard]] bool mysqld_failed_to_start() const {
    return mysqld_failed_to_start_;
  }

  [[nodiscard]] mysql_harness::TcpDestination classic_tcp_destination() const {
    return classic_tcp_dest_;
  }

  [[nodiscard]] mysql_harness::LocalDestination classic_socket_destination()
      const {
    return classic_socket_dest_;
  }

  [[nodiscard]] mysql_harness::TcpDestination x_tcp_destination() const {
    return x_tcp_dest_;
  }

  [[nodiscard]] mysql_harness::LocalDestination x_socket_destination() const {
    return x_socket_dest_;
  }

  [[nodiscard]] static Account caching_sha2_password_account() {
    return {"caching_sha2", "somepass", "caching_sha2_password"};
  }

  [[nodiscard]] static Account caching_sha2_empty_password_account() {
    return {"caching_sha2_empty", "", "caching_sha2_password"};
  }

  [[nodiscard]] static Account caching_sha2_single_use_password_account() {
    return {"caching_sha2_single_use", "notusedyet", "caching_sha2_password"};
  }

  [[nodiscard]] static Account sha256_password_account() {
    return {"sha256_pass", "sha256pass", "sha256_password"};
  }

  [[nodiscard]] static Account sha256_empty_password_account() {
    return {"sha256_empty", "", "sha256_password"};
  }

  [[nodiscard]] static Account admin_account() {
    return {"root", "", "caching_sha2_password"};
  }

  static Account openid_connect_account() {
    // - identity_provider must match the key of the
    //   'authentication_openid_connect_configuration'
    // - user must match the 'sub' of the id-token from the client.
    return {"openid_connect", "", "authentication_openid_connect", R"({
  "identity_provider": "myissuer",
  "user": "openid_user1"
})"};
  }

  bool has_openid_connect() { return has_openid_connect_; }

 private:
  TempDirectory mysqld_dir_{"mysqld"};

  integration_tests::Procs procs_;
  TcpPortPool &port_pool_;

  mysql_harness::TcpDestination classic_tcp_dest_{
      "127.0.0.1", port_pool_.get_next_available()};

  mysql_harness::TcpDestination x_tcp_dest_{"127.0.0.1",
                                            port_pool_.get_next_available()};

  mysql_harness::LocalDestination classic_socket_dest_;
  mysql_harness::LocalDestination x_socket_dest_;

  bool mysqld_failed_to_start_{false};
  bool has_openid_connect_{false};
};

class SharedRouter {
 public:
  SharedRouter(TcpPortPool &port_pool) : port_pool_(port_pool) {}
  integration_tests::Procs &process_manager() { return procs_; }

  void spawn_router(mysql_harness::Destination classic_tcp_dest,
                    mysql_harness::Destination classic_local_dest,
                    mysql_harness::Destination x_tcp_dest,
                    mysql_harness::Destination x_local_dest) {
    auto writer = process_manager().config_writer(conf_dir_.name());

    writer.section("connection_pool", {
                                          {"max_idle_server_connections", "1"},
                                      });

    auto make_destination = [](const mysql_harness::Destination &dest) {
      std::string out;

      if (dest.is_local()) {
        out = "local:";
      }

      out += dest.str();

      return out;
    };

    for (const auto &param : reuse_connection_params) {
      for (bool is_tcp : is_tcp_values) {
        const auto port = port_pool_.get_next_available();
        const auto xport = port_pool_.get_next_available();
        ports_[std::make_tuple(param.client_ssl_mode, param.server_ssl_mode,
                               is_tcp)] = port;
        xports_[std::make_tuple(param.client_ssl_mode, param.server_ssl_mode,
                                is_tcp)] = xport;

        writer
            .section(
                "routing:classic_" + param.testname +
                    (is_tcp ? "_tcp" : "_unix"),
                {
                    {"bind_port", std::to_string(port)},
                    {"destinations",
                     make_destination(is_tcp ? classic_tcp_dest
                                             : classic_local_dest)},
                    {"protocol", "classic"},
                    {"routing_strategy", "round-robin"},

                    {"client_ssl_mode", std::string(param.client_ssl_mode)},
                    {"server_ssl_mode", std::string(param.server_ssl_mode)},

                    {"client_ssl_key",
                     SSL_TEST_DATA_DIR "/server-key-sha512.pem"},
                    {"client_ssl_cert",
                     SSL_TEST_DATA_DIR "/server-cert-sha512.pem"},
                    {"connect_retry_timeout", "0"},
                })
            .section(
                "routing:x_" + param.testname + (is_tcp ? "_tcp" : "_unix"),
                {
                    {"bind_port", std::to_string(xport)},
                    {"destinations",
                     make_destination(is_tcp ? x_tcp_dest : x_local_dest)},
                    {"protocol", "x"},
                    {"routing_strategy", "round-robin"},

                    {"client_ssl_mode", std::string(param.client_ssl_mode)},
                    {"server_ssl_mode", std::string(param.server_ssl_mode)},

                    {"client_ssl_key",
                     SSL_TEST_DATA_DIR "/server-key-sha512.pem"},
                    {"client_ssl_cert",
                     SSL_TEST_DATA_DIR "/server-cert-sha512.pem"},
                });
      }
    }

    auto bindir = process_manager().get_origin();
    auto builddir = bindir.join("..");

    auto &proc =
        process_manager()
            .spawner(bindir.join("mysqlrouter").str())
            .with_core_dump(true)
            .wait_for_sync_point(ProcessManager::Spawner::SyncPoint::READY)
            .spawn({"-c", writer.write()});

    proc.set_logging_path(process_manager().get_logging_dir().str(),
                          "mysqlrouter.log");

    if (!proc.wait_for_sync_point_result()) {
      GTEST_SKIP() << "router failed to start";
    }
  }

  auto host() const { return router_host_; }

  uint16_t port(const ReuseConnectionParam &param, bool is_tcp) const {
    return ports_.at(
        std::make_tuple(param.client_ssl_mode, param.server_ssl_mode, is_tcp));
  }

  uint16_t xport(const ReuseConnectionParam &param, bool is_tcp) const {
    return xports_.at(
        std::make_tuple(param.client_ssl_mode, param.server_ssl_mode, is_tcp));
  }

 private:
  integration_tests::Procs procs_;
  TcpPortPool &port_pool_;

  TempDirectory conf_dir_;

  static const constexpr char router_host_[] = "127.0.0.1";
  std::map<std::tuple<std::string_view, std::string_view, bool>, uint16_t>
      ports_;
  std::map<std::tuple<std::string_view, std::string_view, bool>, uint16_t>
      xports_;
};

class TestWithSharedServer : public RouterComponentTest {
 public:
  static void SetUpTestSuite(TcpPortPool &port_pool) {
    if (shared_server_ == nullptr) {
      shared_server_ = new SharedServer(port_pool);
      shared_server_->initialize_server();

      if (!shared_server_->mysqld_failed_to_start()) {
        SCOPED_TRACE("// spawn server");
        ASSERT_NO_FATAL_FAILURE(shared_server_->spawn_server());

        ASSERT_NO_FATAL_FAILURE(shared_server_->setup_mysqld_xproto_test_env());
        ASSERT_NO_FATAL_FAILURE(shared_server_->setup_mysqld_accounts());
      }
    }
  }

  static void TearDownTestSuite() {
    delete shared_server_;
    shared_server_ = nullptr;
  }

 protected:
  static SharedServer *shared_server_;
};

SharedServer *TestWithSharedServer::shared_server_ = nullptr;

class ReuseConnectionTest : public TestWithSharedServer,
                            public ::testing::WithParamInterface<
                                std::tuple<ReuseConnectionParam, bool>> {
 public:
  static void SetUpTestSuite() {
    TestWithSharedServer::SetUpTestSuite(port_pool_);

    if (shared_router_ == nullptr) {
      shared_router_ = new SharedRouter(port_pool_);

      SCOPED_TRACE("// spawn router");
      shared_router_->spawn_router(shared_server_->classic_tcp_destination(),
                                   shared_server_->classic_socket_destination(),
                                   shared_server_->x_tcp_destination(),
                                   shared_server_->x_socket_destination());
    }
  }

  static void TearDownTestSuite() {
    delete shared_router_;
    shared_router_ = nullptr;

    TestWithSharedServer::TearDownTestSuite();
  }

  static SharedRouter *shared_router() { return shared_router_; }

  static TcpPortPool port_pool_;

  void SetUp() override {
    // shared_server_ may be null if TestWithSharedServer::SetUpTestSuite threw?
    if (shared_server_ == nullptr || shared_server_->mysqld_failed_to_start()) {
      GTEST_SKIP() << "failed to start mysqld";
    }
  }

  ~ReuseConnectionTest() override {
    if (::testing::Test::HasFailure()) {
      shared_router_->process_manager().dump_logs();
      shared_server_->process_manager().dump_logs();
    }
  }

  stdx::expected<std::unique_ptr<xcl::XSession>, xcl::XError> xsess(
      const ReuseConnectionParam &param, bool is_tcp) {
    auto sess = xcl::create_session();

    auto account = SharedServer::caching_sha2_password_account();

    auto xerr = sess->connect(
        shared_router_->host(), shared_router_->xport(param, is_tcp),
        account.username.c_str(), account.password.c_str(), "");
    if (xerr.error() != 0) return stdx::unexpected(xerr);

    return sess;
  }

 protected:
  const std::string valid_ssl_key_{SSL_TEST_DATA_DIR "/server-key-sha512.pem"};
  const std::string valid_ssl_cert_{SSL_TEST_DATA_DIR
                                    "/server-cert-sha512.pem"};

  const std::string some_password_{"some_password"};
  const std::string wrong_password_{"wrong_password"};
  const std::string empty_password_;

  static SharedRouter *shared_router_;
};

SharedRouter *ReuseConnectionTest::shared_router_ = nullptr;
TcpPortPool ReuseConnectionTest::port_pool_;

static stdx::expected<unsigned long, MysqlError> fetch_connection_id(
    MysqlClient &cli) {
  auto query_res = cli.query("SELECT connection_id()");
  if (!query_res) return stdx::unexpected(query_res.error());

  // get the first field, of the first row of the first resultset.
  for (const auto &result : *query_res) {
    if (result.field_count() == 0) {
      return stdx::unexpected(MysqlError(1, "not a resultset", "HY000"));
    }

    for (auto row : result.rows()) {
      auto connection_id = strtoull(row[0], nullptr, 10);

      return connection_id;
    }
  }

  return stdx::unexpected(MysqlError(1, "no rows", "HY000"));
}

TEST_P(ReuseConnectionTest, classic_protocol_ping) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  EXPECT_NO_ERROR(cli.ping());
}

// COM_DEBUG -> mysql_dump_debug_info.
TEST_P(ReuseConnectionTest, classic_protocol_debug_succeeds) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  auto account = SharedServer::admin_account();
  cli.username(account.username);
  cli.password(account.password);

  ASSERT_NO_ERROR(
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp)));

  EXPECT_NO_ERROR(cli.dump_debug_info());

  EXPECT_NO_ERROR(cli.dump_debug_info());
}

// COM_DEBUG -> mysql_dump_debug_info.
TEST_P(ReuseConnectionTest, classic_protocol_debug_fails) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  auto account = SharedServer::caching_sha2_empty_password_account();
  cli.username(account.username);
  cli.password(account.password);

  ASSERT_NO_ERROR(
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp)));
  {
    auto res = cli.dump_debug_info();
    ASSERT_ERROR(res);
    EXPECT_EQ(res.error().value(), 1227);  // access denied, you need SUPER
  }

  {
    auto res = cli.dump_debug_info();
    ASSERT_ERROR(res);
    EXPECT_EQ(res.error().value(), 1227);  // access denied, you need SUPER
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_kill_via_select) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto connection_id_res = fetch_connection_id(cli);
  ASSERT_NO_ERROR(connection_id_res);

  auto connection_id = connection_id_res.value();

  SCOPED_TRACE("// killing connection " + std::to_string(connection_id));
  {
    auto kill_res =
        cli.query("KILL CONNECTION " + std::to_string(connection_id));
    ASSERT_ERROR(kill_res);
    EXPECT_EQ(kill_res.error().value(), 1317) << kill_res.error();
    // Query execution was interrupted
  }

  SCOPED_TRACE("// ping after kill");
  {
    auto ping_res = cli.ping();
    ASSERT_ERROR(ping_res);
    EXPECT_EQ(ping_res.error().value(), 2013) << ping_res.error();
    // Lost connection to MySQL server during query
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_change_user_caching_sha2_empty) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  {
    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }

  {
    auto account = SharedServer::caching_sha2_empty_password_account();
    auto change_user_res =
        cli.change_user(account.username, account.password, "");
    ASSERT_NO_ERROR(change_user_res);
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_change_user_caching_sha2) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  {
    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }

  {
    auto account = SharedServer::caching_sha2_password_account();

    bool expected_fail = param.client_ssl_mode == kDisabled;
    if (!expected_fail) {
      MysqlClient cli;

      cli.username(account.username);
      cli.password(account.password);

      auto server_dest = shared_server_->classic_tcp_destination();

      {
        auto connect_res =
            cli.connect(server_dest.hostname(), server_dest.port());
        ASSERT_NO_ERROR(connect_res);
      }
    }

    auto change_user_res =
        cli.change_user(account.username, account.password, "");
    if (expected_fail) {
      // the client side is not encrypted, but caching-sha2 wants SSL.
      ASSERT_ERROR(change_user_res);
      EXPECT_EQ(change_user_res.error().value(), 2061)
          << change_user_res.error();
      // Authentication plugin 'caching_sha2_password' reported error:
      // Authentication requires secure connection.
    } else {
      ASSERT_NO_ERROR(change_user_res);
    }
  }
}

TEST_P(ReuseConnectionTest,
       classic_protocol_change_user_sha256_password_empty) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  {
    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }

  {
    auto account = SharedServer::sha256_empty_password_account();
    auto change_user_res =
        cli.change_user(account.username, account.password, "");
    ASSERT_NO_ERROR(change_user_res);
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_change_user_sha256_password) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  {
    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));

    ASSERT_NO_ERROR(connect_res);
  }

  // DISABLED/DISABLED   client gets the server's public-key
  // any-other/DISABLED  router gets the server's public-key
  //
  // if !is_tcp:
  // DISABLED/PREFERRED  client-side is insecure, plaintext,
  //                     server-side is secure,   plaintext,
  //                     client should fail, but doesn't.

  auto expect_success = is_tcp ? (param.client_ssl_mode != kDisabled ||
                                  (param.server_ssl_mode == kDisabled ||
                                   param.server_ssl_mode == kAsClient))
                               : (param.client_ssl_mode != kDisabled ||
                                  (param.server_ssl_mode == kDisabled ||
                                   param.server_ssl_mode == kAsClient ||
                                   param.server_ssl_mode == kPreferred));

  {
    auto account = SharedServer::sha256_password_account();
    auto change_user_res =
        cli.change_user(account.username, account.password, "");
    if (expect_success) {
      ASSERT_NO_ERROR(change_user_res);
    } else {
      ASSERT_ERROR(change_user_res);
      EXPECT_EQ(change_user_res.error().value(), 1045)
          << change_user_res.error();
    }
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_statistics) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  EXPECT_NO_ERROR(cli.stat());
}

TEST_P(ReuseConnectionTest, classic_protocol_reset_connection) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  EXPECT_NO_ERROR(cli.reset_connection());
}

TEST_P(ReuseConnectionTest, classic_protocol_query_no_result) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto query_res = cli.query("DO 1");
  ASSERT_NO_ERROR(query_res);
}

TEST_P(ReuseConnectionTest, classic_protocol_query_with_result) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto query_res = cli.query("SELECT * FROM sys.version");
  ASSERT_NO_ERROR(query_res);
}

TEST_P(ReuseConnectionTest, classic_protocol_query_multiple_packets) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  std::string stmt(16L * 1024 * 1024 + 16, 'a');
  stmt.insert(0, "SELECT '");
  stmt.back() = '\'';

  size_t field_len = stmt.size() - ("SELECT '"sv).size() - ("'"sv).size();

  SCOPED_TRACE("// SELECT ... <large-string>");
  auto query_res = cli.query(stmt);
  ASSERT_NO_ERROR(query_res);

  auto results = std::move(*query_res);

  uint64_t num_res{};
  for (const auto &res : results) {
    ++num_res;
    ASSERT_EQ(res.field_count(), 1);

    uint64_t num_rows{};
    for (const auto &row : res.rows()) {
      ++num_rows;
      EXPECT_EQ(strlen(row[0]), field_len);
    }

    EXPECT_EQ(num_rows, 1);
  }
  EXPECT_EQ(num_res, 1);
}

TEST_P(ReuseConnectionTest, classic_protocol_query_call) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  {
    auto query_res =
        cli.query("CALL sys.table_exists('mysql', 'user', @exists)");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("SELECT @exists");
    ASSERT_NO_ERROR(query_res);
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_query_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.query("DO");
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error().value(), 1064)
      << res.error();  // You have an error in your SQL syntax
}

TEST_P(ReuseConnectionTest, classic_protocol_query_load_data_local_infile) {
  auto [param, is_tcp] = GetParam();

  // enable local_infile
  {
    MysqlClient cli;

    cli.username("root");
    cli.password("");

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);

    {
      auto query_res = cli.query("SET GLOBAL local_infile=1");
      ASSERT_NO_ERROR(query_res);
    }
  }

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  ASSERT_NO_ERROR(cli.set_option(MysqlClient::LocalInfile(1)));

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  {
    auto query_res = cli.query("DROP SCHEMA IF EXISTS testing");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("CREATE SCHEMA testing");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("CREATE TABLE testing.t1 (word varchar(20))");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("SET GLOBAL local_infile=1");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("LOAD DATA LOCAL INFILE '" SSL_TEST_DATA_DIR
                               "/words.dat' "
                               "INTO TABLE testing.t1");
    ASSERT_NO_ERROR(query_res);
  }
}

TEST_P(ReuseConnectionTest,
       classic_protocol_query_load_data_local_infile_no_server_support) {
  auto [param, is_tcp] = GetParam();

  // enable local_infile
  {
    MysqlClient cli;

    cli.username("root");
    cli.password("");

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);

    {
      auto query_res = cli.query("SET GLOBAL local_infile=0");
      ASSERT_NO_ERROR(query_res);
    }
  }

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  ASSERT_NO_ERROR(cli.set_option(MysqlClient::LocalInfile(1)));

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  {
    auto query_res = cli.query("DROP SCHEMA IF EXISTS testing");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("CREATE SCHEMA testing");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("CREATE TABLE testing.t1 (word varchar(20))");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("SET GLOBAL local_infile=1");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("LOAD DATA LOCAL INFILE '" SSL_TEST_DATA_DIR
                               "/words.dat' "
                               "INTO TABLE testing.t1");
    ASSERT_NO_ERROR(query_res);
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_use_schema_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.use_schema("does_not_exist");
  ASSERT_ERROR(res);
  EXPECT_EQ(res.error().value(), 1049) << res.error();  // Unknown Database
}

TEST_P(ReuseConnectionTest, classic_protocol_use_schema) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.use_schema("sys");
  ASSERT_NO_ERROR(res);
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SEL ?");
  ASSERT_ERROR(res);
  EXPECT_EQ(res.error().value(), 1064) << res.error();  // Syntax Error
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_execute) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  std::array<MYSQL_BIND, 1> params{
      NullParam{},
  };
  auto bind_res = stmt.bind_params(params);
  EXPECT_NO_ERROR(bind_res) << bind_res.error();

  auto exec_res = stmt.execute();
  EXPECT_NO_ERROR(exec_res) << exec_res.error();

  // don't fetch.
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_execute_fetch) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  int one{1};
  std::array<MYSQL_BIND, 1> params{
      IntegerParam{&one},
  };
  auto bind_res = stmt.bind_params(params);
  EXPECT_NO_ERROR(bind_res) << bind_res.error();

  auto exec_res = stmt.execute();
  EXPECT_NO_ERROR(exec_res) << exec_res.error();

  // may contain multi-resultset
  size_t results{0};
  size_t rows{0};
  for (auto result : exec_res.value()) {
    ++results;
    if (result.field_count() > 0) {
      int count;
      std::array<MYSQL_BIND, 1> fields{IntegerParam{&count}};

      result.bind_result(fields);
      for (const auto fetch_status [[maybe_unused]] : result.rows()) {
        ++rows;
      }
    }
  }
  EXPECT_EQ(results, 1);
  EXPECT_EQ(rows, 1);
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_append_data_execute) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  std::string one{"1"};
  std::array<MYSQL_BIND, 1> params{
      StringParam{one},
  };
  {
    auto bind_res = stmt.bind_params(params);
    EXPECT_NO_ERROR(bind_res) << bind_res.error();
  }

  // a..b..c..d

  // longdata: c_string with len
  {
    auto append_res = stmt.append_param_data(0, "a", 1);
    ASSERT_NO_ERROR(append_res);
  }

  // longdata: string_view
  {
    auto append_res = stmt.append_param_data(0, "b"sv);
    ASSERT_NO_ERROR(append_res);
  }

  // longdata: string_view from std::string
  {
    auto append_res = stmt.append_param_data(0, std::string("c"));
    ASSERT_NO_ERROR(append_res);
  }

  // longdata: string_view from c-string
  {
    auto append_res = stmt.append_param_data(0, "d");
    ASSERT_NO_ERROR(append_res);
  }

  {
    auto exec_res = stmt.execute();
    ASSERT_NO_ERROR(exec_res) << exec_res.error();

    // may contain multi-resultset
    size_t results{0};
    size_t rows{0};
    for (auto result : exec_res.value()) {
      ++results;
      if (result.field_count() > 0) {
        std::string data;
        data.resize(16);                // resize to alloca space
        unsigned long data_actual_len;  // actual length
        std::array<MYSQL_BIND, 1> fields{StringParam{data, &data_actual_len}};

        result.bind_result(fields);
        for (const auto fetch_status [[maybe_unused]] : result.rows()) {
          EXPECT_EQ(data_actual_len, 4);
          EXPECT_EQ(data.size(), 16);

          data.resize(std::min(static_cast<size_t>(data_actual_len),
                               data.size()));  // only shrink

          EXPECT_EQ(data, "abcd");
          ++rows;
        }
      }
    }
    EXPECT_EQ(results, 1);
    EXPECT_EQ(rows, 1);
  }

  // execute again
  {
    auto exec_res = stmt.execute();
    ASSERT_NO_ERROR(exec_res);
  }
}

TEST_P(ReuseConnectionTest,
       classic_protocol_prepare_append_data_reset_execute) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  std::string one{"1"};
  std::array<MYSQL_BIND, 1> params{
      StringParam{one},
  };
  {
    auto bind_res = stmt.bind_params(params);
    EXPECT_NO_ERROR(bind_res) << bind_res.error();
  }

  // a..b..c..d

  // longdata: c_string with len
  {
    auto append_res = stmt.append_param_data(0, "a", 1);
    EXPECT_NO_ERROR(append_res) << append_res.error();
  }

  // longdata: string_view
  {
    auto append_res = stmt.append_param_data(0, "b"sv);
    EXPECT_NO_ERROR(append_res) << append_res.error();
  }

  // longdata: string_view from std::string
  {
    auto append_res = stmt.append_param_data(0, std::string("c"));
    EXPECT_NO_ERROR(append_res) << append_res.error();
  }

  // longdata: string_view from c-string
  {
    auto append_res = stmt.append_param_data(0, "d");
    EXPECT_NO_ERROR(append_res) << append_res.error();
  }

  // reset the append data and use the 'one' instead.
  {
    auto reset_res = stmt.reset();
    EXPECT_NO_ERROR(reset_res) << reset_res.error();
  }

  {
    auto exec_res = stmt.execute();
    EXPECT_NO_ERROR(exec_res) << exec_res.error();

    // may contain multi-resultset
    size_t results{0};
    size_t rows{0};
    for (auto result : exec_res.value()) {
      ++results;
      if (result.field_count() > 0) {
        std::string data;
        data.resize(16);                // resize to alloca space
        unsigned long data_actual_len;  // actual length
        std::array<MYSQL_BIND, 1> fields{StringParam{data, &data_actual_len}};

        result.bind_result(fields);
        for (const auto fetch_status [[maybe_unused]] : result.rows()) {
          EXPECT_EQ(data_actual_len, 1);
          EXPECT_EQ(data.size(), 16);

          data.resize(std::min(static_cast<size_t>(data_actual_len),
                               data.size()));  // only shrink

          // the 'one' is used.
          EXPECT_EQ(data, "1");
          ++rows;
        }
      }
    }
    EXPECT_EQ(results, 1);
    EXPECT_EQ(rows, 1);
  }

  // execute again
  {
    auto exec_res = stmt.execute();
    EXPECT_NO_ERROR(exec_res) << exec_res.error();
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_set_attr) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  auto set_res = stmt.set_attr(MysqlClient::PreparedStatement::PrefetchRows{1});
  EXPECT_NO_ERROR(set_res) << set_res.error();
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_param_count) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  auto set_res = stmt.set_attr(MysqlClient::PreparedStatement::PrefetchRows{1});
  EXPECT_NO_ERROR(set_res) << set_res.error();
}

TEST_P(ReuseConnectionTest,
       classic_protocol_prepare_execute_missing_bind_param) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  // no bind.

  auto exec_res = stmt.execute();
  ASSERT_ERROR(exec_res);
  EXPECT_EQ(exec_res.error().value(), 2031) << exec_res.error();
  // No data supplied for parameters in prepared statement
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_reset) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  auto res = cli.prepare("SELECT ?");
  ASSERT_NO_ERROR(res);

  auto stmt = std::move(res.value());

  auto reset_res = stmt.reset();
  ASSERT_NO_ERROR(reset_res);
}

TEST_P(ReuseConnectionTest, classic_protocol_prepare_call) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connecting to server");
  MysqlClient cli;

  cli.username("root");
  cli.password("");

  auto connect_res =
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp));
  ASSERT_NO_ERROR(connect_res);

  {
    auto query_res = cli.query("DROP SCHEMA IF EXISTS testing");
    ASSERT_NO_ERROR(query_res);
  }

  {
    auto query_res = cli.query("CREATE SCHEMA testing");
    ASSERT_NO_ERROR(query_res);
  }

  SCOPED_TRACE("// create a stored proc with multiple results and outparams");
  {
    auto query_res = cli.query(
        R"(
CREATE PROCEDURE testing.p1 (OUT param1 INT, OUT param2 INT) BEGIN
SELECT 1 INTO param1;
SELECT 2 INTO param2;
SELECT 3;
END)");
    ASSERT_NO_ERROR(query_res);
  }

  SCOPED_TRACE("// prepare 'call testing.p1()'");
  auto stmt_res = cli.prepare("CALL testing.p1(?, ?)");
  ASSERT_NO_ERROR(stmt_res);

  auto stmt = std::move(stmt_res.value());

  std::array<int64_t, 2> values{1, 2};
  std::array<MYSQL_BIND, 2> params{
      IntegerParam{&values[0]},
      IntegerParam{&values[1]},
  };

  {
    auto bind_res = stmt.bind_params(params);
    EXPECT_NO_ERROR(bind_res) << bind_res.error();
  }

  SCOPED_TRACE("// ... and execute() it");
  auto exec_res = stmt.execute();
  EXPECT_NO_ERROR(exec_res) << exec_res.error();

  {
    auto results = std::move(*exec_res);

    auto m_field_count =
        &MysqlClient::PreparedStatement::ResultSet::field_count;
    auto m_is_out_param =
        &MysqlClient::PreparedStatement::ResultSet::is_out_param;

    using ::testing::AllOf;
    using ::testing::ElementsAre;
    using ::testing::Eq;
    using ::testing::Property;

    EXPECT_THAT(
        results,
        ElementsAre(AllOf(Property("field_count", m_field_count, Eq(1)),
                          Property("is_out_param", m_is_out_param, Eq(false))),
                    AllOf(Property("field_count", m_field_count, Eq(2)),
                          Property("is_out_param", m_is_out_param, Eq(true))),
                    Property("field_count", m_field_count, Eq(0))));
  }

  SCOPED_TRACE(
      "// check a new query can be sent to verify all packets have received.");
  {
    auto results_res = cli.query("SELECT 1");
    ASSERT_NO_ERROR(results_res);

    for (const auto &res : *results_res) {
      EXPECT_EQ(res.field_count(), 1);
    }
  }
}

//
// caching_sha2_password
//

TEST_P(ReuseConnectionTest, classic_protocol_caching_sha2_password_with_pass) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  auto account = SharedServer::caching_sha2_password_account();

  std::string username(account.username);
  std::string password(account.password);

  {
    SCOPED_TRACE("// user exists, with pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    if (param.client_ssl_mode == kDisabled) {
      // the client side is not encrypted, but caching-sha2 wants SSL.
      ASSERT_ERROR(connect_res);
      EXPECT_EQ(connect_res.error().value(), 2061) << connect_res.error();
      // Authentication plugin 'caching_sha2_password' reported error:
      // Authentication requires secure connection.
    } else {
      ASSERT_NO_ERROR(connect_res);
    }
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(wrong_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);

    if (param.client_ssl_mode == kDisabled) {
      EXPECT_EQ(connect_res.error().value(), 2061) << connect_res.error();
      // Authentication plugin 'caching_sha2_password' reported error:
      // Authentication requires secure connection.
    } else {
      EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
      // "Access denied for user ..."
    }
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-empty-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(empty_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);
    EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
    // "Access denied for user ..."
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_caching_sha2_password_no_pass) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  auto account = SharedServer::caching_sha2_empty_password_account();

  std::string username(account.username);
  std::string password(account.password);

  {
    SCOPED_TRACE("// user exists, with pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(wrong_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);
    if (param.client_ssl_mode == kDisabled) {
      EXPECT_EQ(connect_res.error().value(), 2061) << connect_res.error();
      // Authentication plugin 'caching_sha2_password' reported error:
      // Authentication requires secure connection.
    } else {
      EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
      // "Access denied for user ..."
    }
  }

  // should reuse connection.
  {
    SCOPED_TRACE("// user exists, with pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }
}

/**
 * Check, caching-sha2-password over plaintext works.
 *
 * when the client connects with ssl_mode=DISABLED and uses
 * caching-sha2-password the first time, it will fail "Auth requires secure
 * connections".
 *
 * After successful login of another client that uses SSL, a plaintext client
 * should be able to login too.
 */
TEST_P(ReuseConnectionTest,
       classic_protocol_caching_sha2_over_plaintext_with_pass) {
  auto [param, is_tcp] = GetParam();

  if (param.client_ssl_mode == kRequired) {
    GTEST_SKIP() << "test requires plaintext connection.";
  }

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  auto account = SharedServer::caching_sha2_single_use_password_account();

  std::string username(account.username);
  std::string password(account.password);

  auto cli_res = shared_server_->admin_cli();
  ASSERT_NO_ERROR(cli_res);

  auto admin_cli = std::move(cli_res.value());

  shared_server_->create_account(admin_cli, account);

  // remove the account at the end of the test again.
  Scope_guard drop_at_end([&admin_cli, account]() {
    shared_server_->drop_account(admin_cli, account);
  });

  SCOPED_TRACE("// caching sha2 password requires secure connection");
  {
    MysqlClient cli;
    cli.set_option(MysqlClient::SslMode(SSL_MODE_DISABLED));

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_ERROR(connect_res);
    EXPECT_EQ(connect_res.error().value(), 2061) << connect_res.error();
    // Authentication plugin 'caching_sha2_password' reported error:
    // Authentication requires secure connection.
  }

  SCOPED_TRACE(
      "// caching sha2 password over secure connection should succeed");
  {
    MysqlClient cli;
    cli.set_option(MysqlClient::SslMode(SSL_MODE_PREFERRED));

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    if (param.client_ssl_mode == kDisabled) {
      // the client side is not encrypted, but caching-sha2 wants SSL.
      ASSERT_ERROR(connect_res);
      EXPECT_EQ(connect_res.error().value(), 2061) << connect_res.error();
      // Authentication plugin 'caching_sha2_password' reported error:
      // Authentication requires secure connection.
    } else {
      ASSERT_NO_ERROR(connect_res);
    }
  }

  SCOPED_TRACE(
      "// caching sha2 password over plain connection should succeed after one "
      "successful auth");
  if (param.server_ssl_mode != kDisabled &&
      param.client_ssl_mode != kDisabled) {
    MysqlClient cli;
    cli.set_option(MysqlClient::SslMode(SSL_MODE_PREFERRED));

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);  // should succeed
  }
}

//
// sha256_password
//

TEST_P(ReuseConnectionTest, classic_protocol_sha256_password_no_pass) {
  auto [param, is_tcp] = GetParam();

  auto account = SharedServer::sha256_empty_password_account();

  std::string username(account.username);
  std::string password(account.password);

  {
    SCOPED_TRACE("// user exists, with pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(wrong_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);
    EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
    // "Access denied for user ..."
  }

  // should reuse connection.
  {
    SCOPED_TRACE("// user exists, with pass, reuse");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_sha256_password_with_pass) {
  auto [param, is_tcp] = GetParam();

  auto expect_success = is_tcp ? (param.client_ssl_mode != kDisabled ||
                                  (param.server_ssl_mode == kDisabled ||
                                   param.server_ssl_mode == kAsClient))
                               : (param.client_ssl_mode != kDisabled ||
                                  (param.server_ssl_mode == kDisabled ||
                                   param.server_ssl_mode == kAsClient ||
                                   param.server_ssl_mode == kPreferred));

  auto account = SharedServer::sha256_password_account();

  std::string username(account.username);
  std::string password(account.password);

  {
    SCOPED_TRACE("// user exists, with pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    if (!expect_success) {
      ASSERT_ERROR(connect_res);
      EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
      // Access denied for user '...'@'localhost' (using password: YES)
    } else {
      ASSERT_NO_ERROR(connect_res);
    }
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(wrong_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);

    EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
    // "Access denied for user ..."
  }

  {
    SCOPED_TRACE("// user exists, with pass, but wrong-empty-pass");
    MysqlClient cli;

    cli.username(username);
    cli.password(empty_password_);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    ASSERT_FALSE(connect_res);
    EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
    // "Access denied for user ..."
  }

  // should reuse connection.
  {
    SCOPED_TRACE("// user exists, with pass, reuse");
    MysqlClient cli;

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    if (!expect_success) {
      ASSERT_ERROR(connect_res);
      EXPECT_EQ(connect_res.error().value(), 1045) << connect_res.error();
      // Access denied for user '...'@'localhost' (using password: YES)
    } else {
      ASSERT_NO_ERROR(connect_res);
    }
  }
}

/**
 * Check, caching-sha2-password over plaintext works with get-server-key.
 */
TEST_P(ReuseConnectionTest,
       classic_protocol_sha256_password_over_plaintext_with_get_server_key) {
  auto [param, is_tcp] = GetParam();

  if (param.client_ssl_mode == kRequired) {
    GTEST_SKIP() << "test requires plaintext connection.";
  }

  bool expect_success =
#if OPENSSL_VERSION_NUMBER < ROUTER_OPENSSL_VERSION(1, 0, 2)
      // DISABLED/DISABLED will get the public-key from the server.
      //
      // other modes that should fail, will fail as the router can't get the
      // public-key from the ssl-certs in openssl 1.0.1
      (param.client_ssl_mode == kDisabled &&
       (param.server_ssl_mode == kDisabled ||
        param.server_ssl_mode == kAsClient)) ||
      (param.client_ssl_mode == kPassthrough) ||
      (param.client_ssl_mode == kPreferred &&
       (param.server_ssl_mode == kDisabled ||
        param.server_ssl_mode == kAsClient));
#else
      is_tcp ? (param.client_ssl_mode != kDisabled ||
                (param.server_ssl_mode == kDisabled ||
                 param.server_ssl_mode == kAsClient))
             : (param.client_ssl_mode != kDisabled ||
                (param.server_ssl_mode == kDisabled ||
                 param.server_ssl_mode == kAsClient ||
                 param.server_ssl_mode == kPreferred));
#endif

  auto account = SharedServer::sha256_password_account();

  std::string username(account.username);
  std::string password(account.password);

  SCOPED_TRACE("// first connection");
  {
    MysqlClient cli;
    cli.set_option(MysqlClient::SslMode(SSL_MODE_DISABLED));
    cli.set_option(MysqlClient::GetServerPublicKey(true));

    cli.username(username);
    cli.password(password);

    auto connect_res = cli.connect(shared_router_->host(),
                                   shared_router_->port(param, is_tcp));
    if (!expect_success) {
      // server will treat the public-key-request as wrong password.
      ASSERT_ERROR(connect_res);
    } else {
      ASSERT_NO_ERROR(connect_res);

      ASSERT_NO_ERROR(cli.ping());
    }
  }

  SCOPED_TRACE("// reuse");
  if (expect_success) {
    MysqlClient cli;
    cli.set_option(MysqlClient::SslMode(SSL_MODE_DISABLED));
    cli.set_option(MysqlClient::GetServerPublicKey(true));

    cli.username(username);
    cli.password(password);

    ASSERT_NO_ERROR(cli.connect(shared_router_->host(),
                                shared_router_->port(param, is_tcp)));
  }
}

//
// openid_connection
//

TEST_P(ReuseConnectionTest, classic_protocol_connect_openid_connect) {
  auto [param, is_tcp] = GetParam();

#ifdef SKIP_AUTHENTICATION_CLIENT_PLUGINS_TESTS
  GTEST_SKIP() << "built with WITH_AUTHENTICATION_CLIENT_PLUGINS=OFF";
#endif

  if (!shared_server_->has_openid_connect()) GTEST_SKIP();

  SCOPED_TRACE("// create the JWT token for authentication.");

  TempDirectory jwtdir;
  auto id_token_res = create_openid_connect_id_token_file(
      "openid_user1",                  // subject
      "https://myissuer.com",          // ${identity_provider}.name
      120,                             // expiry in seconds
      CMAKE_SOURCE_DIR                 //
      "/router/tests/component/data/"  //
      "openid_key.pem",                // private-key of the identity-provider
      jwtdir.name()                    // out-dir
  );
  ASSERT_NO_ERROR(id_token_res);

  auto id_token = *id_token_res;

  SCOPED_TRACE("// setup mysql connection");
  MysqlClient cli;

  auto account = SharedServer::openid_connect_account();

  SCOPED_TRACE(
      "// set the JWT-token in the authentication_openid_connect_client "
      "plugin.");

  cli.set_option(MysqlClient::PluginDir(plugin_output_directory().c_str()));

  auto plugin_res = cli.find_plugin("authentication_openid_connect_client",
                                    MYSQL_CLIENT_AUTHENTICATION_PLUGIN);
  ASSERT_NO_ERROR(plugin_res);

  plugin_res->set_option(
      MysqlClient::Plugin::StringOption("id-token-file", id_token.c_str()));

  SCOPED_TRACE("// connecting to server");

  cli.username(account.username);
  cli.password(account.password);

  bool expect_success = true;
  if (param.client_ssl_mode == kDisabled ||
      (is_tcp ? param.server_ssl_mode == kDisabled : false)) {
    expect_success = false;
  }

  auto connect_res = cli.connect(shared_router()->host(),
                                 shared_router()->port(param, is_tcp));
  if (expect_success) {
    ASSERT_NO_ERROR(connect_res);

    {
      auto query_res = query_one_result(cli, "SELECT USER(), SCHEMA()");
      ASSERT_NO_ERROR(query_res);

      EXPECT_THAT(*query_res, ElementsAre(ElementsAre(
                                  account.username + "@localhost", "<NULL>")));
    }
  } else {
    ASSERT_ERROR(connect_res);
    if (is_tcp && (param.server_ssl_mode == kDisabled ||
                   param.server_ssl_mode == kAsClient)) {
      EXPECT_EQ(connect_res.error().value(), 1045);
    } else {
      EXPECT_EQ(connect_res.error().value(), 2000);
    }
  }
}

TEST_P(ReuseConnectionTest,
       classic_protocol_connect_openid_connect_as_default) {
  auto [param, is_tcp] = GetParam();

#ifdef SKIP_AUTHENTICATION_CLIENT_PLUGINS_TESTS
  GTEST_SKIP() << "built with WITH_AUTHENTICATION_CLIENT_PLUGINS=OFF";
#endif

  if (!shared_server_->has_openid_connect()) GTEST_SKIP();

  SCOPED_TRACE("// create the JWT token for authentication.");

  TempDirectory jwtdir;
  auto id_token_res = create_openid_connect_id_token_file(
      "openid_user1",                  // subject
      "https://myissuer.com",          // ${identity_provider}.name
      120,                             // expiry in seconds
      CMAKE_SOURCE_DIR                 //
      "/router/tests/component/data/"  //
      "openid_key.pem",                // private-key of the identity-provider
      jwtdir.name()                    // out-dir
  );
  ASSERT_NO_ERROR(id_token_res);

  auto id_token = *id_token_res;

  SCOPED_TRACE("// setup mysql connection");

  MysqlClient cli;

  auto account = SharedServer::openid_connect_account();

  SCOPED_TRACE(
      "// set the JWT-token in the authentication_openid_connect_client "
      "plugin.");

  cli.set_option(MysqlClient::PluginDir(plugin_output_directory().c_str()));

  // set the id-token-file path
  auto plugin_res = cli.find_plugin("authentication_openid_connect_client",
                                    MYSQL_CLIENT_AUTHENTICATION_PLUGIN);
  ASSERT_NO_ERROR(plugin_res);

  SCOPED_TRACE("// connecting to server");

  cli.username(account.username);
  cli.password(account.password);
  cli.set_option(MysqlClient::DefaultAuthentication(
      "authentication_openid_connect_client"));

  SCOPED_TRACE("// setting id-token-path: " + id_token);
  ASSERT_TRUE(plugin_res->set_option(
      MysqlClient::Plugin::StringOption("id-token-file", id_token.c_str())));

  auto connect_res = cli.connect(shared_router()->host(),
                                 shared_router()->port(param, is_tcp));
  if ((param.client_ssl_mode == kPassthrough ||
       param.client_ssl_mode == kPreferred ||
       param.client_ssl_mode == kRequired) &&
      (is_tcp ? param.server_ssl_mode != kDisabled : true)) {
    ASSERT_NO_ERROR(connect_res);
    {
      auto query_res = query_one_result(cli, "SELECT USER(), SCHEMA()");
      ASSERT_NO_ERROR(query_res);

      EXPECT_THAT(*query_res, ElementsAre(ElementsAre(
                                  account.username + "@localhost", "<NULL>")));
    }
  } else {
    ASSERT_ERROR(connect_res);
    if (param.client_ssl_mode == kDisabled) {
      EXPECT_EQ(connect_res.error().value(), 2000);
    } else {
      EXPECT_EQ(connect_res.error().value(), 1045);
    }
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_reuse_openid_connect) {
  auto [param, is_tcp] = GetParam();

#ifdef SKIP_AUTHENTICATION_CLIENT_PLUGINS_TESTS
  GTEST_SKIP() << "built with WITH_AUTHENTICATION_CLIENT_PLUGINS=OFF";
#endif

  if (!shared_server_->has_openid_connect()) GTEST_SKIP();

  auto account = SharedServer::openid_connect_account();

  SCOPED_TRACE("// create the JWT token for authentication.");

  TempDirectory jwtdir;
  auto id_token_res = create_openid_connect_id_token_file(
      "openid_user1",                  // subject
      "https://myissuer.com",          // ${identity_provider}.name
      120,                             // expiry in seconds
      CMAKE_SOURCE_DIR                 //
      "/router/tests/component/data/"  //
      "openid_key.pem",                // private-key of the identity-provider
      jwtdir.name()                    // out-dir
  );
  ASSERT_NO_ERROR(id_token_res);

  auto id_token = *id_token_res;

  {
    SCOPED_TRACE("// connecting to server");
    MysqlClient cli;

    SCOPED_TRACE("// locate plugin dir");

    cli.set_option(MysqlClient::PluginDir(plugin_output_directory().c_str()));

    // set the id-token-file path
    auto plugin_res = cli.find_plugin("authentication_openid_connect_client",
                                      MYSQL_CLIENT_AUTHENTICATION_PLUGIN);
    ASSERT_NO_ERROR(plugin_res);

    SCOPED_TRACE("// set the JWT-token in the plugin.");

    plugin_res->set_option(
        MysqlClient::Plugin::StringOption("id-token-file", id_token.c_str()));

    cli.username(account.username);
    cli.password(account.password);

    bool expect_success = true;
    if (param.client_ssl_mode == kDisabled ||
        (is_tcp ? param.server_ssl_mode == kDisabled : false)) {
      expect_success = false;
    }

    auto connect_res = cli.connect(shared_router()->host(),
                                   shared_router()->port(param, is_tcp));
    if (expect_success) {
      ASSERT_NO_ERROR(connect_res);

      {
        auto query_res = query_one_result(cli, "SELECT USER(), SCHEMA()");
        ASSERT_NO_ERROR(query_res);

        EXPECT_THAT(*query_res,
                    ElementsAre(ElementsAre(account.username + "@localhost",
                                            "<NULL>")));
      }
    } else {
      ASSERT_ERROR(connect_res);
      if (is_tcp ? (param.server_ssl_mode == kDisabled ||
                    param.server_ssl_mode == kAsClient)
                 : false) {
        EXPECT_EQ(connect_res.error().value(), 1045);
      } else {
        EXPECT_EQ(connect_res.error().value(), 2000);
      }

      return;
    }
  }

  {
    SCOPED_TRACE("// connecting to server");
    MysqlClient cli;

    SCOPED_TRACE("// locate plugin dir");

    cli.set_option(MysqlClient::PluginDir(plugin_output_directory().c_str()));

    // set the id-token-file path
    auto plugin_res = cli.find_plugin("authentication_openid_connect_client",
                                      MYSQL_CLIENT_AUTHENTICATION_PLUGIN);
    ASSERT_NO_ERROR(plugin_res);

    SCOPED_TRACE("// set the JWT-token in the plugin.");

    plugin_res->set_option(
        MysqlClient::Plugin::StringOption("id-token-file", id_token.c_str()));

    cli.username(account.username);
    cli.password(account.password);

    auto connect_res = cli.connect(shared_router()->host(),
                                   shared_router()->port(param, is_tcp));
    ASSERT_NO_ERROR(connect_res);

    {
      auto query_res = query_one_result(cli, "SELECT USER(), SCHEMA()");
      ASSERT_NO_ERROR(query_res);

      EXPECT_THAT(*query_res, ElementsAre(ElementsAre(
                                  account.username + "@localhost", "<NULL>")));
    }
  }
}

//
// xproto
//

namespace xcl {
std::ostream &operator<<(std::ostream &os, XError const &err) {
  os << err.error() << ": " << err.what();

  return os;
}
}  // namespace xcl

TEST_P(ReuseConnectionTest, x_protocol_crud_find_unknown_collection) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);

  if (param.client_ssl_mode == kDisabled) {
    ASSERT_ERROR(sess_res);
    EXPECT_EQ(sess_res.error().error(), 2510);

    return;
  }

  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::find");
  Mysqlx::Crud::Find msg;

  // set required fields
  // - collection
  auto *coll = msg.mutable_collection();
  {
    auto *name = coll->mutable_name();
    name->assign("nosuchcollection");
  }
  {
    auto *schema = coll->mutable_schema();
    schema->assign("xproto");
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_find(msg, &xerr);
    ASSERT_EQ(xerr.error(), 1146) << xerr;
    // Table does not exist
  }
}

namespace Mysqlx::Notice {
bool operator==(const Warning &lhs, const Warning &rhs) {
  if (!lhs.has_code() && !rhs.has_code()) return true;
  if (lhs.has_code() && !rhs.has_code()) return false;
  if (!lhs.has_code() && rhs.has_code()) return false;

  return lhs.code() == rhs.code();
}
}  // namespace Mysqlx::Notice

TEST_P(ReuseConnectionTest, x_protocol_crud_find) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::find");
  Mysqlx::Crud::Find msg;

  {
    // set required fields
    // - collection
    auto *coll = msg.mutable_collection();
    {
      auto *name = coll->mutable_name();
      name->assign("somecollection");
    }
    {
      auto *schema = coll->mutable_schema();
      schema->assign("xproto");
    }
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_find(msg, &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;

    EXPECT_EQ(exec_res->get_warnings(), xcl::XQuery_result::Warnings{});
    EXPECT_EQ(exec_res->has_resultset(), true);

    const auto *row = exec_res->get_next_row();
    std::string string_v;
    ASSERT_TRUE(row->get_string(0, &string_v));
    // content is {_id: "0000027323879689"}
  }

  SCOPED_TRACE("// sentinel");
  {
    xcl::XError xerr;
    sess->execute_sql("DO 1", &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_delete) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Crud::Delete msg;

  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "somecollection"
  schema: "xproto"
})",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_delete(msg, &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;
    EXPECT_FALSE(exec_res->has_resultset());
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_delete_no_such_table) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::find");
  Mysqlx::Crud::Delete msg;

  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "nosuchcollection"
  schema: "xproto"
})",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_delete(msg, &xerr);
    ASSERT_EQ(xerr.error(), 1146) << xerr;  // table does not exist
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_insert) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");

  Mysqlx::Crud::Insert msg;
  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "somecollection"
  schema: "xproto"
}
row {
  field {
    type: LITERAL
    literal {
      type: V_STRING
      v_string {
        # empty document.
        value: "{}"
      }
    }
  }
})",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_insert(msg, &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;
    EXPECT_FALSE(exec_res->has_resultset());
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_insert_no_row_data) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Crud::Insert msg;

  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "somecollection"
  schema: "xproto"
})",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_insert(msg, &xerr);
    ASSERT_EQ(xerr.error(), 5013) << xerr;  // missing row data for insert
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_update) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Crud::Update msg;

  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "somecollection"
  schema: "xproto"
}
operation {
  operation: ITEM_SET
  source {}
}
)",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_update(msg, &xerr);
    ASSERT_EQ(xerr.error(), 0) << xerr;
    EXPECT_FALSE(exec_res->has_resultset());
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_update_no_row_data) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Crud::Update msg;

  {
    TextFormatParser parser;

    ASSERT_TRUE(parser.ParseFromString(
        R"(
collection {
  name: "somecollection"
  schema: "xproto"
})",
        &msg))
        << parser.errors().to_string();
  }

  SCOPED_TRACE("// send message");
  {
    xcl::XError xerr;
    auto exec_res = sess->get_protocol().execute_update(msg, &xerr);
    ASSERT_EQ(xerr.error(), 5050) << xerr;  // invalid update expression list
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_stmt) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Prepare::Prepare msg;

  msg.set_stmt_id(1);

  {
    // set required fields
    // - collection
    auto *prep = msg.mutable_stmt();
    {
      auto *stmt = prep->mutable_stmt_execute();
      stmt->set_stmt("DO ?");
    }

    prep->set_type(Mysqlx::Prepare::Prepare_OneOfMessage::Type::
                       Prepare_OneOfMessage_Type_STMT);
  }

  SCOPED_TRACE("// send message");
  {
    auto xerr = sess->get_protocol().send(msg);
    ASSERT_EQ(xerr.error(), 0) << xerr;
  }

  SCOPED_TRACE("// recv message");
  {
    auto xerr = sess->get_protocol().recv_ok();
    ASSERT_EQ(xerr.error(), 0) << xerr;
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_stmt_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message");
  Mysqlx::Prepare::Prepare msg;

  msg.set_stmt_id(1);

  {
    // set required fields
    // - collection
    auto *prep = msg.mutable_stmt();
    {
      auto *stmt = prep->mutable_stmt_execute();
      stmt->set_stmt("?");
    }

    prep->set_type(Mysqlx::Prepare::Prepare_OneOfMessage::Type::
                       Prepare_OneOfMessage_Type_STMT);
  }

  SCOPED_TRACE("// send message");
  {
    auto xerr = sess->get_protocol().send(msg);
    ASSERT_EQ(xerr.error(), 0) << xerr;
  }

  SCOPED_TRACE("// recv message");
  {
    auto xerr = sess->get_protocol().recv_ok();
    ASSERT_EQ(xerr.error(), 1064) << xerr;
    // You have an error in your SQL syntax
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_deallocate_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message: prepare::deallocate()");
  {
    Mysqlx::Prepare::Deallocate msg;

    msg.set_stmt_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5110) << xerr;
      // Statement with ID=1 was not prepared
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_deallocate) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message: prepare::prepare()");
  {
    Mysqlx::Prepare::Prepare msg;

    msg.set_stmt_id(1);

    {
      // set required fields
      // - collection
      auto *prep = msg.mutable_stmt();
      {
        auto *stmt = prep->mutable_stmt_execute();
        stmt->set_stmt("DO ?");
      }

      prep->set_type(Mysqlx::Prepare::Prepare_OneOfMessage::Type::
                         Prepare_OneOfMessage_Type_STMT);
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  SCOPED_TRACE("// prepare message: prepare::deallocate()");
  {
    Mysqlx::Prepare::Deallocate msg;

    msg.set_stmt_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_execute_fail) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare message: prepare::execute()");
  {
    Mysqlx::Prepare::Execute msg;

    msg.set_stmt_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5110) << xerr;
      // Statement with ID=1 was not prepared
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_prepare_execute) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// prepare::prepare()");
  {
    Mysqlx::Prepare::Prepare msg;

    msg.set_stmt_id(1);

    {
      // set required fields
      // - collection
      auto *prep = msg.mutable_stmt();
      {
        auto *stmt = prep->mutable_stmt_execute();
        stmt->set_stmt("SELECT ?");
      }

      prep->set_type(Mysqlx::Prepare::Prepare_OneOfMessage::Type::
                         Prepare_OneOfMessage_Type_STMT);
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  SCOPED_TRACE("// prepare::execute()");
  {
    Mysqlx::Prepare::Execute msg;

    msg.set_stmt_id(1);

    {
      auto *args = msg.mutable_args();
      {
        auto *arg = args->Add();
        arg->set_type(Mysqlx::Datatypes::Any::Type::Any_Type_SCALAR);
        auto *scalar = arg->mutable_scalar();
        scalar->set_v_unsigned_int(1);
        scalar->set_type(Mysqlx::Datatypes::Scalar::Type::Scalar_Type_V_UINT);
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      xcl::XError xerr;
      auto exec_res = sess->get_protocol().recv_resultset(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;

      EXPECT_EQ(exec_res->get_warnings(), xcl::XQuery_result::Warnings{});
      EXPECT_EQ(exec_res->has_resultset(), true);

      auto *row = exec_res->get_next_row(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
      ASSERT_TRUE(row);
      uint64_t v;
      ASSERT_TRUE(row->get_uint64(0, &v));
      EXPECT_EQ(v, 1);
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_expect_open) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// expect::open()");
  {
    Mysqlx::Expect::Open msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_expect_close_no_open) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// expect::close()");
  {
    Mysqlx::Expect::Close msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5158) << xerr;
      // Expect block currently not open
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_expect_open_close) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// expect::open()");
  {
    Mysqlx::Expect::Open msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  SCOPED_TRACE("// expect::close()");
  {
    Mysqlx::Expect::Open msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

/**
 * check the error-path of CrudCreateView.
 */
TEST_P(ReuseConnectionTest, x_protocol_crud_create_view_no_such_table) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::create_view()");
  {
    Mysqlx::Crud::CreateView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    {
      auto *stmt = msg.mutable_stmt();
      auto *coll = stmt->mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("nosuchcollection");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 1146) << xerr;
      // Table does not exist
    }
  }
}

/**
 * check the success-path of CrudCreateView.
 */
TEST_P(ReuseConnectionTest, x_protocol_crud_create_view_drop_view) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::create_view()");
  {
    Mysqlx::Crud::CreateView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    {
      auto *stmt = msg.mutable_stmt();
      auto *coll = stmt->mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("somecollection");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  // cleanup again.

  SCOPED_TRACE("// crud::drop_view()");
  {
    Mysqlx::Crud::DropView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_modify_view_fail_unknown_table) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::modify_view()");
  {
    Mysqlx::Crud::ModifyView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5012) << xerr;
      // The field that defines the select statement is required
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_modify_view) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  // setup

  SCOPED_TRACE("// crud::create_view()");
  {
    Mysqlx::Crud::CreateView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    {
      auto *stmt = msg.mutable_stmt();
      auto *coll = stmt->mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("somecollection");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  Scope_guard teardown([&]() {
    SCOPED_TRACE("// crud::drop_view()");
    {
      Mysqlx::Crud::DropView msg;

      {
        auto *coll = msg.mutable_collection();
        {
          auto *name = coll->mutable_name();
          name->assign("someview");
        }
        {
          auto *schema = coll->mutable_schema();
          schema->assign("xproto");
        }
      }

      SCOPED_TRACE("// send message");
      {
        auto xerr = sess->get_protocol().send(msg);
        ASSERT_EQ(xerr.error(), 0) << xerr;
      }

      SCOPED_TRACE("// recv message");
      {
        auto xerr = sess->get_protocol().recv_ok();
        ASSERT_EQ(xerr.error(), 0) << xerr;
      }
    }
  });

  SCOPED_TRACE("// crud::modify_view()");
  {
    Mysqlx::Crud::ModifyView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    {
      auto *stmt = msg.mutable_stmt();
      auto *coll = stmt->mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("somecollection");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
      // The field that defines the select statement is required
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_crud_drop_view_fail_unknown_table) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// crud::drop_view()");
  {
    Mysqlx::Crud::DropView msg;

    {
      auto *coll = msg.mutable_collection();
      {
        auto *name = coll->mutable_name();
        name->assign("someview");
      }
      {
        auto *schema = coll->mutable_schema();
        schema->assign("xproto");
      }
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 1051) << xerr;
      // Unknown table
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_cursor_close_not_open) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// cursor::close()");
  {
    Mysqlx::Cursor::Close msg;

    msg.set_cursor_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5111) << xerr;
      // Cursor with ID=1 was not opened.
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_cursor_fetch_not_open) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// cursor::fetch()");
  {
    Mysqlx::Cursor::Fetch msg;

    msg.set_cursor_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5111) << xerr;
      // Cursor with ID=1 was not opened.
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_cursor_open_no_stmt_prepared) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// cursor::fetch()");
  {
    Mysqlx::Cursor::Open msg;

    msg.set_cursor_id(1);

    {
      auto *stmt = msg.mutable_stmt();

      stmt->set_type(Mysqlx::Cursor::Open::OneOfMessage::Type::
                         Open_OneOfMessage_Type_PREPARE_EXECUTE);

      auto *prep = stmt->mutable_prepare_execute();
      prep->set_stmt_id(1);
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 5110) << xerr;
      // Statement with ID=1 was not opened.
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_cursor_open_fetch_close) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  // setup
  SCOPED_TRACE("// prepare::prepare()");
  {
    Mysqlx::Prepare::Prepare msg;

    msg.set_stmt_id(1);

    {
      // set required fields
      // - collection
      auto *prep = msg.mutable_stmt();
      {
        auto *stmt = prep->mutable_stmt_execute();
        stmt->set_stmt("SELECT 1");
      }

      prep->set_type(Mysqlx::Prepare::Prepare_OneOfMessage::Type::
                         Prepare_OneOfMessage_Type_STMT);
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  xcl::XQuery_result::Metadata cursor_metadata;
  SCOPED_TRACE("// cursor::open()");
  {
    Mysqlx::Cursor::Open msg;

    msg.set_cursor_id(1);

    {
      auto *stmt = msg.mutable_stmt();

      stmt->set_type(Mysqlx::Cursor::Open::OneOfMessage::Type::
                         Open_OneOfMessage_Type_PREPARE_EXECUTE);

      auto *prep = stmt->mutable_prepare_execute();
      prep->set_stmt_id(1);
    }

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      xcl::XError xerr;
      auto open_res = sess->get_protocol().recv_resultset(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;

      // capture the metadata for the fetch.
      cursor_metadata = open_res->get_metadata();

      // required to take the open-cursor result off the wire and allow other
      // commands.
      EXPECT_FALSE(open_res->next_resultset(&xerr));
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }

  // we should have a resultset.
  EXPECT_EQ(cursor_metadata.size(), 1);

  SCOPED_TRACE("// cursor::fetch() - 0 rows");
  {
    Mysqlx::Cursor::Fetch msg;

    msg.set_cursor_id(1);
    msg.set_fetch_rows(0);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      xcl::XError xerr;

      auto exec_res = sess->get_protocol().recv_resultset(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;

      exec_res->set_metadata(cursor_metadata);

      EXPECT_EQ(exec_res->get_warnings(), xcl::XQuery_result::Warnings{});
      EXPECT_EQ(exec_res->has_resultset(), true);

      auto *row = exec_res->get_next_row(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
      ASSERT_FALSE(row);
    }
  }

  SCOPED_TRACE("// cursor::fetch() - 1 row");
  {
    Mysqlx::Cursor::Fetch msg;

    msg.set_cursor_id(1);
    msg.set_fetch_rows(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      xcl::XError xerr;
      auto exec_res = sess->get_protocol().recv_resultset(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;

      exec_res->set_metadata(cursor_metadata);

      EXPECT_EQ(exec_res->get_warnings(), xcl::XQuery_result::Warnings{});
      EXPECT_EQ(exec_res->has_resultset(), true);

      auto *row = exec_res->get_next_row(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
      ASSERT_TRUE(row);

      EXPECT_TRUE(row->valid());
      EXPECT_EQ(row->get_number_of_fields(), 1);

      int64_t v;
      ASSERT_TRUE(row->get_int64(0, &v));
      EXPECT_EQ(v, 1);
    }
  }

  SCOPED_TRACE("// cursor::fetch() - done");
  {
    Mysqlx::Cursor::Fetch msg;

    msg.set_cursor_id(1);
    msg.set_fetch_rows(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      xcl::XError xerr;
      auto exec_res = sess->get_protocol().recv_resultset(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;

      exec_res->set_metadata(cursor_metadata);

      EXPECT_EQ(exec_res->get_warnings(), xcl::XQuery_result::Warnings{});
      EXPECT_EQ(exec_res->has_resultset(), true);

      auto *row = exec_res->get_next_row(&xerr);
      ASSERT_EQ(xerr.error(), 0) << xerr;
      ASSERT_FALSE(row);
    }
  }

  SCOPED_TRACE("// cursor::close()");
  {
    Mysqlx::Cursor::Close msg;

    msg.set_cursor_id(1);

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_session_close) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::close()");
  {
    Mysqlx::Session::Close msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_session_reset) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::reset()");
  {
    Mysqlx::Session::Reset msg;

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

/**
 * check auth-start fails after connection.
 *
 * session::close() first.
 */
TEST_P(ReuseConnectionTest,
       x_protocol_session_authenticate_start_unexpected_message) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::auth_start()");
  {
    Mysqlx::Session::AuthenticateStart msg;

    msg.set_mech_name("NATIVE");

    SCOPED_TRACE("// send message");
    {
      auto xerr = sess->get_protocol().send(msg);
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }

    SCOPED_TRACE("// recv message");
    {
      auto xerr = sess->get_protocol().recv_ok();
      ASSERT_EQ(xerr.error(), 1047) << xerr;
      // Unexpected message received
    }
  }
}

TEST_P(ReuseConnectionTest,
       x_protocol_session_authenticate_start_sha256_password_empty) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::auth_start()");
  {
    auto account = SharedServer::sha256_empty_password_account();

    auto xerr = sess->reauthenticate(account.username.c_str(),
                                     account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest,
       x_protocol_session_authenticate_start_sha256_password) {
  auto [param, is_tcp] = GetParam();

  SCOPED_TRACE("// connect");

  auto sess_res = xsess(param, is_tcp);
  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::auth_start()");
  {
    auto account = SharedServer::sha256_password_account();

    auto xerr = sess->reauthenticate(account.username.c_str(),
                                     account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest,
       x_protocol_session_authenticate_start_caching_sha2_password_empty) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// connect");
  auto sess_res = xsess(param, is_tcp);

  if (param.client_ssl_mode == kDisabled) {
    ASSERT_ERROR(sess_res);
    EXPECT_EQ(sess_res.error().error(), 2510);

    return;
  }

  if (is_tcp && param.server_ssl_mode == kDisabled) {
    ASSERT_ERROR(sess_res);
    // Invalid authentication method PLAIN
    EXPECT_EQ(sess_res.error().error(), 1251);

    return;
  }

  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::auth_start()");
  {
    auto account = SharedServer::caching_sha2_empty_password_account();

    auto xerr = sess->reauthenticate(account.username.c_str(),
                                     account.password.c_str(), "");

    if (param.client_ssl_mode == kDisabled ||
        (is_tcp && param.server_ssl_mode == kDisabled)) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest,
       x_protocol_session_authenticate_start_caching_sha2_password) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// connect");
  auto sess_res = xsess(param, is_tcp);

  if (param.client_ssl_mode == kDisabled) {
    ASSERT_ERROR(sess_res);
    EXPECT_EQ(sess_res.error().error(), 2510);

    return;
  }

  if (is_tcp && param.server_ssl_mode == kDisabled) {
    ASSERT_ERROR(sess_res);
    // Invalid authentication method PLAIN
    EXPECT_EQ(sess_res.error().error(), 1251);

    return;
  }

  ASSERT_NO_ERROR(sess_res);

  auto sess = std::move(sess_res.value());

  SCOPED_TRACE("// session::auth_start()");
  {
    auto account = SharedServer::caching_sha2_password_account();

    auto xerr = sess->reauthenticate(account.username.c_str(),
                                     account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled ||
        (is_tcp && param.server_ssl_mode == kDisabled)) {
      ASSERT_EQ(xerr.error(), 1045) << xerr;
      // Access denied for user ...@'localhost'
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_connect_sha256_password_empty) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// setup");
  auto sess = xcl::create_session();
  auto account = SharedServer::sha256_empty_password_account();

  SCOPED_TRACE("// connect");
  {
    auto xerr = sess->connect(
        shared_router_->host(), shared_router_->xport(param, is_tcp),
        account.username.c_str(), account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else if (is_tcp && param.server_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 1251) << xerr;
      // Invalid authentication method PLAIN
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_connect_sha256_password) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  auto sess = xcl::create_session();
  auto account = SharedServer::sha256_password_account();

  SCOPED_TRACE("// connect");
  {
    auto xerr = sess->connect(
        shared_router_->host(), shared_router_->xport(param, is_tcp),
        account.username.c_str(), account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else if (is_tcp && param.server_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 1251) << xerr;
      // Invalid authentication method PLAIN
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_connect_caching_sha2_password_empty) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// setup");

  auto sess = xcl::create_session();

  if (param.client_ssl_mode == kDisabled ||
      param.server_ssl_mode == kDisabled) {
    sess->set_mysql_option(
        xcl::XSession::Mysqlx_option::Authentication_method,
        std::vector<std::string>{"MYSQL41", "SHA256_MEMORY"});
  }

  auto account = SharedServer::caching_sha2_empty_password_account();

  SCOPED_TRACE("// connect");
  {
    auto xerr = sess->connect(
        shared_router_->host(), shared_router_->xport(param, is_tcp),
        account.username.c_str(), account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else if (param.server_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 1045) << xerr;
      // Access denied for user ...
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, x_protocol_connect_caching_sha2_password) {
  auto [param, is_tcp] = GetParam();

  // reset auth-cache for caching-sha2-password
  shared_server_->flush_prileges();

  SCOPED_TRACE("// setup");
  auto sess = xcl::create_session();

  if (param.client_ssl_mode == kDisabled ||
      param.server_ssl_mode == kDisabled) {
    sess->set_mysql_option(
        xcl::XSession::Mysqlx_option::Authentication_method,
        std::vector<std::string>{"MYSQL41", "SHA256_MEMORY"});
  }

  auto account = SharedServer::caching_sha2_password_account();

  SCOPED_TRACE("// connect");
  {
    auto xerr = sess->connect(
        shared_router_->host(), shared_router_->xport(param, is_tcp),
        account.username.c_str(), account.password.c_str(), "");
    if (param.client_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 2510) << xerr;
      // Authentication failed, check username and password or try a secure
      // connection
    } else if (param.server_ssl_mode == kDisabled) {
      ASSERT_EQ(xerr.error(), 1045) << xerr;
      // Access denied for user ...
    } else {
      ASSERT_EQ(xerr.error(), 0) << xerr;
    }
  }
}

TEST_P(ReuseConnectionTest, classic_protocol_charset_after_connect) {
  auto [param, is_tcp] = GetParam();

  MysqlClient cli;

  auto account = SharedServer::caching_sha2_empty_password_account();

  cli.username(account.username);
  cli.password(account.password);

  cli.set_option(MysqlClient::CharsetName("latin1"));

  ASSERT_NO_ERROR(
      cli.connect(shared_router_->host(), shared_router_->port(param, is_tcp)));

  {
    auto cmd_res = query_one_result(
        cli, "select @@character_set_client, @@collation_connection");
    ASSERT_NO_ERROR(cmd_res);

    EXPECT_THAT(*cmd_res,
                ElementsAre(ElementsAre("latin1", "latin1_swedish_ci")));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Spec, ReuseConnectionTest,
    ::testing::Combine(::testing::ValuesIn(reuse_connection_params),
                       ::testing::ValuesIn(is_tcp_values)),
    [](auto &info) {
      auto param = std::get<0>(info.param);
      auto is_tcp = std::get<1>(info.param);

      return "ssl_modes_" + param.testname + (is_tcp ? "_tcp" : "_socket");
    });

int main(int argc, char *argv[]) {
  net::impl::socket::init();

  // init openssl as otherwise libmysqlxclient may fail at SSL_CTX_new
  TlsLibraryContext tls_lib_ctx;

  ProcessManager::set_origin(Path(argv[0]).dirname());
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
