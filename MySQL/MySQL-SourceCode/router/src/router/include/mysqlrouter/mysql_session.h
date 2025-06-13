/*
  Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#ifndef _ROUTER_MYSQL_SESSION_H_
#define _ROUTER_MYSQL_SESSION_H_

#include "mysql/harness/logging/logger.h"
#include "mysqlrouter/router_mysql_export.h"

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <mysql.h>  // enum mysql_ssl_mode
#include <string.h>

#include "mysql/harness/stdx/expected.h"
#include "mysqlrouter/log_filter.h"
#include "secure_string.h"  // NOLINT(build/include_subdir)

namespace mysqlrouter {

class MysqlError {
 public:
  MysqlError() = default;
  MysqlError(unsigned int code, std::string message, std::string sql_state)
      : code_{code},
        message_{std::move(message)},
        sql_state_{std::move(sql_state)} {}

  operator bool() { return code_ != 0; }

  std::string message() const { return message_; }
  std::string sql_state() const { return sql_state_; }
  unsigned int value() const { return code_; }

 private:
  unsigned int code_{0};
  std::string message_;
  std::string sql_state_;
};

namespace impl {
/**
 * gettable, settable option for mysql_option's.
 *
 * adapts scalar types like int/bool/... mysql_option's to
 * mysql_options()/mysql_get_option().
 *
 * - mysql_options() expects a '&int'
 * - mysql_get_option() expects a '&int'
 */
template <mysql_option Opt, class ValueType>
class Option {
 public:
  using value_type = ValueType;

  constexpr Option() = default;
  constexpr explicit Option(value_type v) : v_{std::move(v)} {}

  // get the option id
  constexpr mysql_option option() const noexcept { return Opt; }

  // get address of the storage.
  constexpr const void *data() const { return std::addressof(v_); }

  // get address of the storage.
  constexpr void *data() { return std::addressof(v_); }

  // set the value of the option
  constexpr void value(value_type v) { v_ = v; }

  // get the value of the option
  constexpr value_type value() const { return v_; }

 private:
  value_type v_{};
};

/**
 * gettable, settable option for 'const char *' based mysql_option's.
 *
 * adapts 'const char *' based mysql_option to
 * mysql_options()/mysql_get_option().
 *
 * - mysql_options() expects a 'const char *'
 * - mysql_get_option() expects a '&(const char *)'
 */
template <mysql_option Opt>
class Option<Opt, const char *> {
 public:
  using value_type = const char *;

  Option() = default;
  constexpr explicit Option(value_type v) : v_{std::move(v)} {}

  constexpr mysql_option option() const noexcept { return Opt; }

  constexpr const void *data() const { return v_; }

  constexpr void *data() { return std::addressof(v_); }

  constexpr void value(value_type v) { v_ = v; }

  constexpr value_type value() const { return v_; }

 private:
  value_type v_{};
};

template <mysql_option Opt>
class Option<Opt, std::nullptr_t> {
 public:
  using value_type = std::nullptr_t;

  Option() = default;
  // accept a void *, but ignore it.
  constexpr explicit Option(value_type) {}

  constexpr mysql_option option() const noexcept { return Opt; }

  constexpr const void *data() const { return nullptr; }

  constexpr void *data() { return nullptr; }

  constexpr value_type value() const { return nullptr; }
};
}  // namespace impl

// mysql_options() may be used with MYSQL * == nullptr to get global values.

class ROUTER_MYSQL_EXPORT MySQLSession {
 public:
  static constexpr int kDefaultConnectTimeout = 5;
  static constexpr int kDefaultReadTimeout = 30;
  class ResultRow;
  typedef std::vector<const char *> Row;
  typedef std::function<bool(const Row &)> RowProcessor;
  typedef std::function<bool(const ResultRow &)> ResultRowProcessor;

  typedef std::function<void(unsigned, MYSQL_FIELD *)> FieldValidator;
  typedef std::function<void()> OnResultSetEnd;

  // text representations of SSL modes
  static const char kSslModeDisabled[];
  static const char kSslModePreferred[];
  static const char kSslModeRequired[];
  static const char kSslModeVerifyCa[];
  static const char kSslModeVerifyIdentity[];
  //
  // mysql_option's
  //
  // (sorted by appearance in documentation)

  // type for mysql_option's which set/get a 'bool'
  template <mysql_option Opt>
  using BooleanOption = impl::Option<Opt, bool>;

  // type for mysql_option's which set/get a 'unsigned int'
  template <mysql_option Opt>
  using IntegerOption = impl::Option<Opt, unsigned int>;

  // type for mysql_option's which set/get a 'unsigned long'
  template <mysql_option Opt>
  using LongOption = impl::Option<Opt, unsigned long>;

  // type for mysql_option's which set/get a 'const char *'
  template <mysql_option Opt>
  using ConstCharOption = impl::Option<Opt, const char *>;

  using DefaultAuthentication = ConstCharOption<MYSQL_DEFAULT_AUTH>;
  using EnableCleartextPlugin = BooleanOption<MYSQL_ENABLE_CLEARTEXT_PLUGIN>;
  using InitCommand = ConstCharOption<MYSQL_INIT_COMMAND>;
  using BindAddress = ConstCharOption<MYSQL_OPT_BIND>;
  using CanHandleExpiredPasswords =
      BooleanOption<MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS>;
  using Compress = BooleanOption<MYSQL_OPT_COMPRESS>;
  using CompressionAlgorithms =
      ConstCharOption<MYSQL_OPT_COMPRESSION_ALGORITHMS>;
  using ConnectAttributeReset = BooleanOption<MYSQL_OPT_CONNECT_ATTR_RESET>;
  using ConnectAttributeDelete = BooleanOption<MYSQL_OPT_CONNECT_ATTR_DELETE>;
  using ConnectTimeout = IntegerOption<MYSQL_OPT_CONNECT_TIMEOUT>;
  using GetServerPublicKey = BooleanOption<MYSQL_OPT_GET_SERVER_PUBLIC_KEY>;
  using LoadDataLocalDir = ConstCharOption<MYSQL_OPT_LOAD_DATA_LOCAL_DIR>;
  using LocalInfile = IntegerOption<MYSQL_OPT_LOCAL_INFILE>;
  using MaxAllowedPacket = LongOption<MYSQL_OPT_MAX_ALLOWED_PACKET>;
  using NamedPipe = BooleanOption<MYSQL_OPT_NAMED_PIPE>;
  using NetBufferLength = LongOption<MYSQL_OPT_NET_BUFFER_LENGTH>;
  using OptionalResultsetMetadata =
      BooleanOption<MYSQL_OPT_OPTIONAL_RESULTSET_METADATA>;
  // TCP/UnixSocket/...
  using Protocol = IntegerOption<MYSQL_OPT_PROTOCOL>;
  using ReadTimeout = IntegerOption<MYSQL_OPT_READ_TIMEOUT>;
  using RetryCount = IntegerOption<MYSQL_OPT_RETRY_COUNT>;
  using SslCa = ConstCharOption<MYSQL_OPT_SSL_CA>;
  using SslCaPath = ConstCharOption<MYSQL_OPT_SSL_CAPATH>;
  using SslCert = ConstCharOption<MYSQL_OPT_SSL_CERT>;
  using SslCipher = ConstCharOption<MYSQL_OPT_SSL_CIPHER>;
  using SslCrl = ConstCharOption<MYSQL_OPT_SSL_CRL>;
  using SslCrlPath = ConstCharOption<MYSQL_OPT_SSL_CRLPATH>;
  using SslFipsMode = IntegerOption<MYSQL_OPT_SSL_FIPS_MODE>;
  using SslKey = ConstCharOption<MYSQL_OPT_SSL_KEY>;
  using SslMode = IntegerOption<MYSQL_OPT_SSL_MODE>;
  using TlsCipherSuites = ConstCharOption<MYSQL_OPT_TLS_CIPHERSUITES>;
  using TlsVersion = ConstCharOption<MYSQL_OPT_TLS_VERSION>;
  using WriteTimeout = IntegerOption<MYSQL_OPT_WRITE_TIMEOUT>;
  using ZstdCompressionLevel = IntegerOption<MYSQL_OPT_ZSTD_COMPRESSION_LEVEL>;

  using PluginDir = ConstCharOption<MYSQL_PLUGIN_DIR>;
  using ReportDataTruncation = BooleanOption<MYSQL_REPORT_DATA_TRUNCATION>;
  using ServerPluginKey = ConstCharOption<MYSQL_SERVER_PUBLIC_KEY>;
  using ReadDefaultFile = ConstCharOption<MYSQL_READ_DEFAULT_FILE>;
  using ReadDefaultGroup = ConstCharOption<MYSQL_READ_DEFAULT_GROUP>;
  using CharsetDir = ConstCharOption<MYSQL_SET_CHARSET_DIR>;
  using CharsetName = ConstCharOption<MYSQL_SET_CHARSET_NAME>;
  using SharedMemoryBasename = ConstCharOption<MYSQL_SHARED_MEMORY_BASE_NAME>;

  class Transaction {
   public:
    Transaction() {}
    Transaction(Transaction &&other) : session_{other.session_} {
      other.session_ = nullptr;
    }

    Transaction(MySQLSession *session, const bool consisten_snapshot = false)
        : session_(session) {
      session_->execute(consisten_snapshot
                            ? "START TRANSACTION WITH CONSISTENT SNAPSHOT"
                            : "START TRANSACTION");
    }

    ~Transaction() {
      try {
        rollback();
      } catch (...) {
        // ignore errors during rollback on d-tor
      }
    }

    void commit() {
      if (session_) {
        session_->execute("COMMIT");
        session_ = nullptr;
      }
    }

    void rollback() {
      if (session_) {
        session_->execute("ROLLBACK");
        session_ = nullptr;
      }
    }

    Transaction &operator=(Transaction &&other) {
      std::swap(session_, other.session_);
      return *this;
    }

   private:
    MySQLSession *session_{nullptr};
  };

  class Error : public std::runtime_error {
   public:
    // NOTE Not all calls to constructors provide the 3rd argument.  To save
    //      time, only the code where it was needed atm was upgraded from 2 to
    //      3 args; upgrade elsewhere if needed

    Error(const char *error, unsigned int code,
          const std::string message = "<not set>")
        : std::runtime_error(error), code_(code), message_(message) {}

    Error(const std::string &error, unsigned int code,
          const std::string &message = "<not set>")
        : std::runtime_error(error), code_(code), message_(message) {}

    unsigned int code() const { return code_; }
    std::string message() const { return message_; }

   private:
    const unsigned int code_;
    const std::string message_;
  };

  class ResultRow {
   public:
    class RowIt {
     public:
      RowIt(const ResultRow *parent, uint32_t idx = 0)
          : idx_{idx}, parent_{parent} {}
      const char *operator*() const { return (*parent_)[idx_]; }
      void operator++() { ++idx_; }

      bool operator!=(const RowIt &other) const {
        if (parent_ != other.parent_) return false;
        return idx_ != other.idx_;
      }

     private:
      uint32_t idx_{0};
      const ResultRow *parent_;
    };
    ResultRow(Row row) : row_{std::move(row)} {}
    virtual ~ResultRow() = default;
    size_t size() const { return row_.size(); }
    RowIt begin() const { return RowIt(this, 0); }
    RowIt end() const { return RowIt(this, size()); }
    const char *&operator[](size_t i) { return row_[i]; }
    const char *operator[](size_t i) const { return row_[i]; }
    virtual size_t get_data_size(size_t i) const { return strlen(row_[i]); }

   private:
    friend class MySQLSession;
    Row row_;
  };

  MySQLSession();
  virtual ~MySQLSession();

  static mysql_ssl_mode parse_ssl_mode(
      std::string ssl_mode);  // throws std::logic_error
  static const char *ssl_mode_to_string(mysql_ssl_mode ssl_mode) noexcept;

  // throws Error, std::invalid_argument
  virtual void set_ssl_options(mysql_ssl_mode ssl_mode,
                               const std::string &tls_version,
                               const std::string &ssl_cipher,
                               const std::string &ca, const std::string &capath,
                               const std::string &crl,
                               const std::string &crlpath);

  uint64_t connection_id();

  mysql_ssl_mode ssl_mode() const;
  std::string tls_version() const;
  std::string ssl_cipher() const;
  std::string ssl_ca() const;
  std::string ssl_capath() const;
  std::string ssl_crl() const;
  std::string ssl_crlpath() const;

  std::string ssl_cert() const;
  std::string ssl_key() const;

  int connect_timeout() const;
  int read_timeout() const;

  // throws Error
  virtual void set_ssl_cert(const std::string &cert, const std::string &key);

  /**
   * set a mysql option.
   *
   * @code
   * auto res = set_option(ConnectTimeout(10));
   * @endcode
   *
   * @note on error the MysqlError may not always contain the right error-code.
   *
   * @param [in] opt option to set.
   * @returns a MysqlError on error
   * @retval true on success
   */
  template <class SettableMysqlOption>
  stdx::expected<void, MysqlError> set_option(const SettableMysqlOption &opt) {
    if (0 != mysql_options(connection_, opt.option(), opt.data())) {
      return stdx::unexpected(MysqlError(mysql_errno(connection_),
                                         mysql_error(connection_),
                                         mysql_sqlstate(connection_)));
    }

    return {};
  }

  /**
   * get a mysql option.
   *
   * @code
   * ConnectTimeout opt_connect_timeout;
   * auto res = get_option(opt_connect_timeout);
   * if (res) {
   *   std::cerr << opt_connect_timeout.value() << std::endl;
   * }
   * @endcode
   *
   * @param [in,out] opt option to query.
   * @retval true on success.
   * @retval false if option is not known.
   */
  template <class GettableMysqlOption>
  bool get_option(GettableMysqlOption &opt) const {
    if (0 != mysql_get_option(connection_, opt.option(), opt.data())) {
      return false;
    }

    return true;
  }

  // these methods are deprecated, versions with a SecureString password should
  // be used instead
  void connect(const std::string &host, unsigned int port,
               const std::string &username, const std::string &password,
               const std::string &unix_socket,
               const std::string &default_schema,
               int connect_timeout = kDefaultConnectTimeout,
               int read_timeout = kDefaultReadTimeout,
               unsigned long extra_client_flags = 0);
  void connect(const MySQLSession &other, const std::string &username,
               const std::string &password);
  void change_user(const std::string &user, const std::string &password,
                   const std::string &db);

  virtual void connect(const std::string &host, unsigned int port,
                       const std::string &username,
                       const mysql_harness::SecureString &password,
                       const std::string &unix_socket,
                       const std::string &default_schema,
                       int connect_timeout = kDefaultConnectTimeout,
                       int read_timeout = kDefaultReadTimeout,
                       unsigned long extra_client_flags = 0);  // throws Error
  /**
   * Connect using the same settings and parameters that were used for the last
   * other.connect() using provided credentials.
   */
  virtual void connect(const MySQLSession &other, const std::string &username,
                       const mysql_harness::SecureString &password);
  virtual void change_user(const std::string &user,
                           const mysql_harness::SecureString &password,
                           const std::string &db);

  virtual void disconnect();

  virtual void reset();

  virtual uint64_t prepare(const std::string &query);
  virtual void prepare_execute_with_bind_parameters(
      uint64_t ps_id, std::vector<MYSQL_BIND> bind_parameters,
      const ResultRowProcessor &processor,
      const FieldValidator &validator /*= null_field_validator*/,
      const OnResultSetEnd &on_resultset_end);
  virtual void prepare_remove(uint64_t ps_id);
  virtual void execute(
      const std::string &query);  // throws Error, std::logic_error
  virtual void query(
      const std::string &query, const ResultRowProcessor &processor,
      const FieldValidator &validator);  // throws Error, std::logic_error
  virtual std::unique_ptr<MySQLSession::ResultRow> query_one(
      const std::string &query,
      const FieldValidator &validator);  // throws Error
                                         //
  void query(
      const std::string &query, const RowProcessor &processor,
      const FieldValidator &validator);  // throws Error, std::logic_error
  void query(const std::string &stmt, const RowProcessor &processor) {
    return query(stmt, processor, [](unsigned, MYSQL_FIELD *) {});
  }

  virtual std::unique_ptr<MySQLSession::ResultRow> query_one(
      const std::string &stmt) {
    return query_one(stmt, [](unsigned, MYSQL_FIELD *) {});
  }

  virtual int ping();
  virtual bool execute_nb(
      const std::string &query);  // throws Error, std::logic_error

  virtual uint64_t last_insert_id() noexcept;
  virtual uint64_t affected_rows() noexcept;

  virtual unsigned warning_count() noexcept;

  virtual std::string quote(const std::string &s, char qchar = '\'') const;

  virtual bool is_connected() noexcept { return connection_ && connected_; }
  const std::string &get_address() noexcept { return connection_address_; }

  virtual const char *last_sqlstate();
  virtual const char *last_error();
  virtual unsigned int last_errno();

  virtual const char *ssl_cipher();
  virtual bool has_data_on_socket();
  virtual std::vector<std::string> get_session_tracker_data(
      enum enum_session_state_type type);

  virtual bool is_ssl_session_reused();

  virtual unsigned long server_version();

  MYSQL *get_handle() { return connection_; }

 private:
  // stores selected parameters that were passed to the last successful call to
  // connect()
  struct {
    std::string host;
    unsigned int port{};
    std::string unix_socket;
    std::string default_schema;
  } connect_params_;

  uint64_t last_stmt_id{0};
  std::map<uint64_t, MYSQL_STMT *> stmts_;
  MYSQL *connection_;
  bool connected_;
  std::string connection_address_;
  SQLLogFilter log_filter_;
  unsigned long extra_client_flags_{0};
  uint64_t connection_id_{0};

  class MYSQL_RES_Deleter {
   public:
    void operator()(MYSQL_RES *res) { mysql_free_result(res); }
  };

  using mysql_result_type = std::unique_ptr<MYSQL_RES, MYSQL_RES_Deleter>;

  enum class AsyncQueryState {
    kNone,        // no async query active
    kQuery,       // waiting query to finish
    kStoreResult  // waiting store result to finish
  };
  AsyncQueryState async_state_ = AsyncQueryState::kNone;
  bool async_query_logged_ = false;

  /**
   * run query.
   *
   * There are 3 cases:
   *
   * 1. query returns a resultset
   * 3. query returns no resultset
   * 2. query fails with an error
   *
   * @param q stmt to execute
   *
   * @returns resultset on success, MysqlError on error
   */
  stdx::expected<mysql_result_type, MysqlError> real_query(
      const std::string &q);

  stdx::expected<mysql_result_type, MysqlError> real_query_nb(
      const std::string &q);

  /**
   * log query after running it.
   */
  stdx::expected<mysql_result_type, MysqlError> logged_real_query(
      const std::string &q);

  stdx::expected<mysql_result_type, MysqlError> logged_real_query_nb(
      const std::string &q);

  void throw_mysqlerror(MYSQL_STMT *stmt, uint64_t ps_id);
  // if query be timed and sent to the sql-log.
  mysql_harness::logging::DomainLogger logger_{"sql"};
};

}  // namespace mysqlrouter

#endif
