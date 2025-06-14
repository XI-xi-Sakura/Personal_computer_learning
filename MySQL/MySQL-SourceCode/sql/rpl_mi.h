/* Copyright (c) 2006, 2025, Oracle and/or its affiliates.

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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef RPL_MI_H
#define RPL_MI_H

#include <sys/types.h>
#include <time.h>
#include <atomic>

#include "compression.h"  // COMPRESSION_ALGORITHM_NAME_BUFFER_SIZE
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "mysql/binlog/event/binlog_event.h"  // enum_binlog_checksum_alg
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql_com.h"
#include "sql/binlog.h"
#include "sql/log_event.h"                // Format_description_log_event
#include "sql/rpl_gtid.h"                 // Gtid
#include "sql/rpl_info.h"                 // Rpl_info
#include "sql/rpl_rli.h"                  // rli->get_log_lock()
#include "sql/rpl_trx_boundary_parser.h"  // Transaction_boundary_parser
#include "sql/sql_const.h"
#include "strmake.h"

class Rpl_info_handler;
class Server_ids;
class THD;
struct MYSQL;

#define DEFAULT_CONNECT_RETRY 60

/*****************************************************************************
  Replication IO Thread

  Master_info contains:
    - information about how to connect to a master
    - current master log name
    - current master log offset
    - misc control variables

  Master_info is initialized once from the master.info repository if such
  exists. Otherwise, data members corresponding to master.info fields
  are initialized with defaults specified by master-* options. The
  initialization is done through mi_init_info() call.

  Logically, the format of master.info repository is presented as follows:

  log_name
  log_pos
  source_host
  source_user
  source_pass
  source_port
  source_connect_retry

  To write out the contents of master.info to disk a call to flush_info()
  is required. Currently, it is needed every time we read and queue data
  from the master.

  To clean up, call end_info()

*****************************************************************************/

class Master_info : public Rpl_info {
  friend class Rpl_info_factory;

 public:
  /// In case we start replication from the first binary log file and
  /// source log name is empty, we use first_source_log_name instead of
  /// 'master_log_name' in the error log
  static constexpr auto first_source_log_name = "<First log>";

  /**
    Host name or ip address stored in the master.info.
  */
  char host[HOSTNAME_LENGTH + 1];

  /*
    Check if the channel is configured.

    @param mi Pointer to Master_info.

    @retval true  The channel is configured.
    @retval false The channel is not configured.
  */
  static bool is_configured(Master_info *mi) { return mi && mi->host[0]; }

 private:
  /**
    If true, USER/PASSWORD was specified when running START REPLICA.
  */
  bool start_user_configured;
  /**
    User's name stored in the master.info.
  */
  char user[USERNAME_LENGTH + 1];
  /**
    User's password stored in the master.info.
  */
  char password[MAX_PASSWORD_LENGTH + 1];
  /**
    User specified when running START REPLICA.
  */
  char start_user[USERNAME_LENGTH + 1];
  /**
    Password specified when running START REPLICA.
  */
  char start_password[MAX_PASSWORD_LENGTH + 1];
  /**
    Stores the authentication plugin specified when running START REPLICA.
  */
  char start_plugin_auth[FN_REFLEN + 1];
  /**
    Stores the authentication plugin directory specified when running
    START REPLICA.
  */
  char start_plugin_dir[FN_REFLEN + 1];

  /// Information on the current and last queued transactions
  Gtid_monitoring_info *gtid_monitoring_info;

#ifdef HAVE_PSI_INTERFACE
  /**
    PSI key for the `rotate_lock`
  */
  PSI_mutex_key *key_info_rotate_lock;
  /**
    PSI key for the `rotate_cond`
  */
  PSI_mutex_key *key_info_rotate_cond;
#endif
  /**
    Lock to protect from rotating the relay log when in the middle of a
    transaction.
  */
  mysql_mutex_t rotate_lock;
  /**
    Waiting condition that will block the process/thread requesting a relay log
    rotation in the middle of a transaction. The process/thread will wait until
    the transaction is written to the relay log and the rotation is,
    successfully accomplished.
  */
  mysql_cond_t rotate_cond;
  /**
    If a rotate was requested while the relay log was in a transaction.
  */
  std::atomic<bool> rotate_requested{false};

 public:
  /**
    Returns if USER/PASSWORD was specified when running
    START REPLICA.

    @return true or false.
  */
  bool is_start_user_configured() const { return start_user_configured; }
  /**
    Returns if DEFAULT_AUTH was specified when running START REPLICA.

    @return true or false.
  */
  bool is_start_plugin_auth_configured() const {
    return (start_plugin_auth[0] != 0);
  }
  /**
    Returns if PLUGIN_DIR was specified when running START REPLICA.

    @return true or false.
  */
  bool is_start_plugin_dir_configured() const {
    return (start_plugin_dir[0] != 0);
  }
  /**
    Defines that USER/PASSWORD was specified or not when running
    START REPLICA.

    @param config is true or false.
  */
  void set_start_user_configured(bool config) {
    start_user_configured = config;
  }
  /**
    Sets either user's name in the master.info repository when CHANGE
    MASTER is executed or user's name used in START REPLICA if USER is
    specified.

    @param user_arg is user's name.
  */
  void set_user(const char *user_arg) {
    if (user_arg && start_user_configured) {
      strmake(start_user, user_arg, sizeof(start_user) - 1);
    } else if (user_arg) {
      strmake(user, user_arg, sizeof(user) - 1);
    }
  }
  /**
    Returns user's size name. See @c get_user().

    @return user's size name.
  */
  size_t get_user_size() const {
    return (start_user_configured ? sizeof(start_user) : sizeof(user));
  }
  /**
    If an user was specified when running START REPLICA, this function returns
    such user. Otherwise, it returns the user stored in master.info.

    @return user's name.
  */
  const char *get_user() const {
    return start_user_configured ? start_user : user;
  }
  /**
    Stores either user's password in the master.info repository when CHANGE
    MASTER is executed or user's password used in START REPLICA if PASSWORD
    is specified.

    @param password_arg is user's password.

  */
  void set_password(const char *password_arg);
  /**
    Returns either user's password in the master.info repository or
    user's password used in START REPLICA.

    @param[out] password_arg is user's password.
    @param[out] password_arg_size is user's password size.

    @return false if there is no error, otherwise true is returned.
  */
  bool get_password(char *password_arg, size_t *password_arg_size);
  /**
    Cleans in-memory password defined by START REPLICA.
  */
  void reset_start_info();
  /**
    Returns the DEFAULT_AUTH defined by START REPLICA.

    @return DEFAULT_AUTH.
  */
  const char *get_start_plugin_auth() { return start_plugin_auth; }
  /**
    Returns the PLUGIN_DIR defined by START REPLICA.

    @return PLUGIN_DIR.
  */
  const char *get_start_plugin_dir() { return start_plugin_dir; }
  /**
    Stores the DEFAULT_AUTH defined by START REPLICA.
  */
  void set_plugin_auth(const char *src) {
    if (src) strmake(start_plugin_auth, src, sizeof(start_plugin_auth) - 1);
  }
  /**
    Stores the DEFAULT_AUTH defined by START REPLICA.
  */
  void set_plugin_dir(const char *src) {
    if (src) strmake(start_plugin_dir, src, sizeof(start_plugin_dir) - 1);
  }

  bool ssl;  // enables use of SSL connection if true
  char ssl_ca[FN_REFLEN], ssl_capath[FN_REFLEN], ssl_cert[FN_REFLEN];
  char ssl_cipher[FN_REFLEN], ssl_key[FN_REFLEN];
  char tls_version[FN_REFLEN];
  /*
    Ciphersuites used for TLS 1.3 communication with the master server.
    tls_ciphersuites = NULL means that TLS 1.3 default ciphersuites
    are enabled. To allow a value that can either be NULL or a string,
    it is represented by the pair:
      first:  true if tls_ciphersuites is set to NULL
      second: the string value when first is false
  */
  std::pair<bool, std::string> tls_ciphersuites = {true, ""};
  char ssl_crl[FN_REFLEN], ssl_crlpath[FN_REFLEN];
  char public_key_path[FN_REFLEN];
  bool ssl_verify_server_cert;
  bool get_public_key;

  MYSQL *mysql;
  uint32 file_id; /* for 3.23 load data infile */
  Relay_log_info *rli;
  uint port;
  uint connect_retry;
  /*
     The difference in seconds between the clock of the master and the clock of
     the slave (second - first). It must be signed as it may be <0 or >0.
     clock_diff_with_master is computed when the I/O thread starts; for this the
     I/O thread does a SELECT UNIX_TIMESTAMP() on the master.
     "how late the slave is compared to the master" is computed like this:
     clock_of_slave - last_timestamp_executed_by_SQL_thread -
     clock_diff_with_master

  */
  long clock_diff_with_master;
  float heartbeat_period;         // interface with CHANGE REPLICATION SOURCE or
                                  // master.info
  ulonglong received_heartbeats;  // counter of received heartbeat events

  ulonglong last_heartbeat;

  /// The decompressed size of the transaction that is currently in progress of
  /// being queued, if any. This is private to the reciever.
  int64_t m_queueing_transaction_size{0};
  /// The size of the gtid event for the  transaction that is currently in
  /// progress of being queued. This is private to the receiver.
  int64_t m_queueing_transaction_gtid_event_size{0};

  Server_ids *ignore_server_ids;

  ulong master_id;
  /*
    to hold checksum alg in use until IO thread has received FD.
    Initialized to novalue, then set to the queried from master
    @@global.binlog_checksum and deactivated once FD has been received.
  */
  mysql::binlog::event::enum_binlog_checksum_alg checksum_alg_before_fd;
  ulong retry_count;
  char master_uuid[UUID_LENGTH + 1];
  char bind_addr[HOSTNAME_LENGTH + 1];

  /*
    Name of a network namespace where a socket for connection to a master
    should be created.
  */
  char network_namespace[NAME_LEN];

  bool is_set_network_namespace() const { return network_namespace[0] != 0; }

  const char *network_namespace_str() const {
    return is_set_network_namespace() ? network_namespace : "";
  }
  /*
    describes what compression algorithm and level is used between
    master/slave communication protocol
  */
  char compression_algorithm[COMPRESSION_ALGORITHM_NAME_BUFFER_SIZE];
  int zstd_compression_level;
  NET_SERVER server_extn;  // maintain compress context info.

  int mi_init_info();
  void end_info();
  int flush_info(bool force = false);
  void set_relay_log_info(Relay_log_info *info);

  bool shall_ignore_server_id(ulong s_id);

  /*
     A buffer to hold " for channel <channel_name>
     used in error messages per channel
   */
  char for_channel_str[CHANNEL_NAME_LENGTH + 31];
  char for_channel_uppercase_str[CHANNEL_NAME_LENGTH + 31];

  /**
    @return The pointer to the Gtid_monitoring_info
  */
  Gtid_monitoring_info *get_gtid_monitoring_info() {
    return gtid_monitoring_info;
  }

  /**
    Stores the details of the transaction the receiver thread has just started
    queueing.

    @param  gtid_arg         the gtid of the trx
    @param  original_ts_arg  the original commit timestamp of the transaction
    @param  immediate_ts_arg the immediate commit timestamp of the transaction
  */
  void started_queueing(Gtid gtid_arg, ulonglong original_ts_arg,
                        ulonglong immediate_ts_arg) {
    gtid_monitoring_info->start(gtid_arg, original_ts_arg, immediate_ts_arg);
  }

  /**
    Track statistics after the receiver has finished queueing a transaction.

    In gtid_monitoring_info, the transaction timestamp is recorded and the
    information is copied to last_queued_trx and cleared from queueing_trx.

    In applier_metrics, the size/count of received transactions are incremented.
  */
  void finished_queueing() {
    gtid_monitoring_info->finish();

    auto &applier_metrics = rli->get_applier_metrics();
    // Transactions before the metrics breakpoint count their sizes at commit
    // time. Transactions after the metrics breakpoint count their sizes here,
    // at the time they are fully received. See
    // Applier_metrics_interface::is_after_metrics_breakpoint for details.
    if (rli->get_applier_metrics().is_after_metrics_breakpoint()) {
      applier_metrics.inc_transactions_received_size_sum(
          m_queueing_transaction_size);
      applier_metrics.inc_transactions_received_count(1);
    }
  }

  /**
    @return True if there is a transaction currently being queued
  */
  bool is_queueing_trx() {
    return gtid_monitoring_info->is_processing_trx_set();
  }

  /**
    @return The pointer to the GTID of the processing_trx of
            Gtid_monitoring_info.
  */
  const Gtid *get_queueing_trx_gtid() {
    return gtid_monitoring_info->get_processing_trx_gtid();
  }

  /**
    Clears the processing_trx monitoring info.

    Normally called when there is an error while queueing the transaction.
  */
  void clear_queueing_trx(bool need_lock = false) {
    if (need_lock) mysql_mutex_lock(&data_lock);
    gtid_monitoring_info->clear_processing_trx();
    if (need_lock) mysql_mutex_unlock(&data_lock);
  }

  /**
    Clears all GTID monitoring info.
  */
  void clear_gtid_monitoring_info(bool need_lock = false) {
    if (need_lock) mysql_mutex_lock(&data_lock);
    gtid_monitoring_info->clear();
    if (need_lock) mysql_mutex_unlock(&data_lock);
  }

  ~Master_info() override;

  /**
    Sets the flag that indicates that a relay log rotation has been requested.

    @param[in]         thd     the client thread carrying the command.
   */
  void request_rotate(THD *thd);
  /**
    Clears the flag that indicates that a relay log rotation has been requested
    and notifies requester that the rotation has finished.
   */
  void clear_rotate_requests();
  /**
    Checks whether or not there is a request for rotating the underlying relay
    log.

    @returns true if there is, false otherwise
   */
  bool is_rotate_requested();

 protected:
  char master_log_name[FN_REFLEN]{};
  my_off_t master_log_pos;

 public:
  inline const char *get_master_log_name() const { return master_log_name; }
  inline const char *get_master_log_name_info() const {
    if (m_is_receiver_position_info_invalid) return "INVALID";
    return get_master_log_name();
  }
  inline ulonglong get_master_log_pos() const { return master_log_pos; }
  inline ulonglong get_master_log_pos_info() const {
    if (m_is_receiver_position_info_invalid) return 0;
    return get_master_log_pos();
  }
  inline void set_master_log_name(const char *log_file_name) {
    strmake(master_log_name, log_file_name, sizeof(master_log_name) - 1);
  }
  inline void set_master_log_pos(ulonglong log_pos) {
    m_is_receiver_position_info_invalid = false;
    master_log_pos = log_pos;
  }
  inline const char *get_io_rpl_log_name() {
    return (master_log_name[0] ? master_log_name : "FIRST");
  }
  static size_t get_number_info_mi_fields();

  /**
     returns the column number of a channel in the TABLE repository.
     Mainly used during server startup to load the information required
     from the slave repository tables. See rpl_info_factory.cc
  */
  static uint get_channel_field_num();

  /**
     Returns an array with the expected column names of the primary key
     fields of the table repository.
  */
  static const char **get_table_pk_field_names();

  /**
     Returns an array with the expected column numbers of the primary key
     fields of the table repository.
  */
  static const uint *get_table_pk_field_indexes();

  /**
     Sets bits for columns that are allowed to be `NULL`.

     @param nullable_fields the bitmap to hold the nullable fields.
  */
  static void set_nullable_fields(MY_BITMAP *nullable_fields);

  bool is_auto_position() { return auto_position; }

  void set_auto_position(bool auto_position_param) {
    auto_position = auto_position_param;
  }

  /**
    Checks if Asynchronous Replication Connection Failover feature is enabled.

    @returns true   if Asynchronous Replication Connection Failover feature is
                    enabled.
             false  otherwise.
  */
  bool is_source_connection_auto_failover() {
    return m_source_connection_auto_failover;
  }

  /**
    Enable Asynchronous Replication Connection Failover feature.
  */
  void set_source_connection_auto_failover() {
    m_source_connection_auto_failover = true;
  }

  /**
    Disable Asynchronous Replication Connection Failover feature.
  */
  void unset_source_connection_auto_failover() {
    m_source_connection_auto_failover = false;
  }

  /**
    Return current position in the Failover source list for the
    Asynchronous Replication Connection Failover feature.
    Based on this position next failover source is selected.

    @returns current position in the Failover source list.
  */
  uint get_failover_list_position() { return m_failover_list_position; }

  /**
    Reset current position to 0, when new failover source list is fetched.
  */
  void reset_failover_list_position() { m_failover_list_position = 0; }

  /**
    Increment current position, so next failover source can be selected on
    failure.
  */
  void increment_failover_list_position() { m_failover_list_position++; }

  /**
    Checks if network error has occurred.

    @returns true   if slave IO thread failure was due to network error,
             false  otherwise.
  */
  bool is_network_error() { return m_network_error; }

  /**
    Sets m_network_error to true. Its used by async replication connection
    failover in case of slave IO thread failure to check if it was due to
    network failure.
  */
  void set_network_error() { m_network_error = true; }

  /**
    Resets m_network_error to false.
  */
  void reset_network_error() { m_network_error = false; }

  /**
    This member function shall return true if there are server
    ids configured to be ignored.

    @return true if there are server ids to be ignored,
            false otherwise.
  */
  bool is_ignore_server_ids_configured();

 private:
  /**
    Format_description_log_event for events received from the master
    by the IO thread and written to the tail of the relay log.

    Use patterns:
     - Created when the IO thread starts and destroyed when the IO
       thread stops.
     - Updated when the IO thread receives a
       Format_description_log_event.
     - Accessed by the IO thread when it de-serializes events (e.g.,
       rotate events, Gtid events).
     - Written by the IO thread to the new relay log on every rotation.
     - Written by a client that executes FLUSH LOGS to the new relay
       log on every rotation.

    Locks:
    All access is protected by Relay_log::LOCK_log.
  */
  Format_description_log_event *mi_description_event;

 public:
  Format_description_log_event *get_mi_description_event() {
    mysql_mutex_assert_owner(rli->relay_log.get_log_lock());
    return mi_description_event;
  }
  void set_mi_description_event(Format_description_log_event *fdle) {
    mysql_mutex_assert_owner(rli->relay_log.get_log_lock());
    delete mi_description_event;
    mi_description_event = fdle;
  }

  bool set_info_search_keys(Rpl_info_handler *to) override;

  const char *get_for_channel_str(bool upper_case = false) const override {
    return reinterpret_cast<const char *>(upper_case ? for_channel_uppercase_str
                                                     : for_channel_str);
  }

  void init_master_log_pos();

 private:
  bool read_info(Rpl_info_handler *from) override;
  bool write_info(Rpl_info_handler *to) override;

  bool auto_position{false};
  bool m_source_connection_auto_failover{false};
  uint m_failover_list_position{0};
  bool m_network_error{false};

  Master_info(
#ifdef HAVE_PSI_INTERFACE
      PSI_mutex_key *param_key_info_run_lock,
      PSI_mutex_key *param_key_info_data_lock,
      PSI_mutex_key *param_key_info_sleep_lock,
      PSI_mutex_key *param_key_info_thd_lock,
      PSI_mutex_key *param_key_info_rotate_lock,
      PSI_mutex_key *param_key_info_data_cond,
      PSI_mutex_key *param_key_info_start_cond,
      PSI_mutex_key *param_key_info_stop_cond,
      PSI_mutex_key *param_key_info_sleep_cond,
      PSI_mutex_key *param_key_info_rotate_cond,
#endif
      uint param_id, const char *param_channel);

  Master_info(const Master_info &info);
  Master_info &operator=(const Master_info &info);

 public:
  /*
    This will be used to verify transactions boundaries of events sent by the
    master server.
    It will also be used to verify transactions boundaries on the relay log
    while collecting the Retrieved_Gtid_Set to make sure of only adding GTIDs
    of fully retrieved transactions.
    Its output is also used to detect when events were not logged using row
    based logging.
  */
  Replication_transaction_boundary_parser transaction_parser;

 private:
  /*
    This is the channel lock. It is a rwlock used to serialize all replication
    administrative commands that cannot be performed concurrently for a given
    replication channel:
    - START REPLICA;
    - STOP REPLICA;
    - CHANGE REPLICATION SOURCE;
    - RESET REPLICA;
    - end_slave() (when mysqld stops)).
    Any of these commands must hold the wrlock from the start till the end.
  */
  Checkable_rwlock *m_channel_lock;

  /* References of the channel, the channel can only be deleted when it is 0. */
  std::atomic<int32> atomic_references{0};

 public:
  /**
    Acquire the channel read lock.
  */
  void channel_rdlock();

  /**
    Acquire the channel write lock.
  */
  void channel_wrlock();

  /**
    Try to acquire the write lock, and fail if it cannot be
    immediately granted.
  */
  int channel_trywrlock();

  /**
    Release the channel lock (whether it is a write or read lock).
  */
  inline void channel_unlock() { m_channel_lock->unlock(); }

  /**
    Assert that some thread holds either the read or the write lock.
  */
  inline void channel_assert_some_lock() const {
    m_channel_lock->assert_some_lock();
  }

  /**
    Assert that some thread holds the write lock.
  */
  inline void channel_assert_some_wrlock() const {
    m_channel_lock->assert_some_wrlock();
  }

  /**
    Increase the reference count to prohibit deleting a channel. This function
    must be protected by channel_map.rdlock(). dec_reference has to be
    called in conjunction with inc_reference().
  */
  void inc_reference() { ++atomic_references; }

  /**
    Decrease the reference count. Doesn't need the protection of
    channel_map.rdlock.
  */
  void dec_reference() { --atomic_references; }

  /**
    It mush be called before deleting a channel and protected by
    channel_map_lock.wrlock().

    @param thd the THD object of current thread
  */
  void wait_until_no_reference(THD *thd);

  /* Set true when the Master_info object was cleared by a RESET REPLICA */
  bool reset;

  /**
    Sync flushed_relay_log_info with current relay log coordinates.

    It will sync the receiver thread relay log coordinates (file name and
    position) with the last master coordinates that were flushed into the
    Master_info repository.

    This function shall be called by Master_info::flush_info() at the end of a
    successful flush of the Master_info content into the repository while still
    holding the data_lock.

    It is also called my load_mi_and_rli_from_repositories(), right after the
    successful call to rli_init_info() that opens the relay log.
  */

  void update_flushed_relay_log_info();

  /**
    Collect relay log coordinates (file name and position) that related to the
    last Master_info master coordinates flushed into the repository.

    @param [out] linfo Where the relay log coordinates shall be stored.
  */

  void get_flushed_relay_log_info(Log_info *linfo);

  /**
    Marks the receiver position information (master_log_name, master_log_pos)
    as being invalid or not.

    @param invalid The value to set the status to invalid or valid
  */
  void set_receiver_position_info_invalid(bool invalid);

  /**
    Returns if receiver position information is valid or invalid

    @return true if receiver position information is not reliable,
            false otherwise.
  */
  bool is_receiver_position_info_invalid() const;

  /**
    Enable or disable the gtid_only mode

    @param gtid_only_mode value to set gtid_only (enable/disable it)
  */
  void set_gtid_only_mode(bool gtid_only_mode);

  /**
    Returns if gtid_only is enabled or not

    @return true if gtid_only mode is active for a channel,
            false otherwise.
  */
  bool is_gtid_only_mode() const;

 private:
  /*
    Holds the relay log coordinates (file name and position) of the last master
    coordinates flushed into Master_info repository.
  */
  Log_info flushed_relay_log_info;

  /**
    Is the replica working in GTID only mode, meaning it does not
    persist position related information when executing or queueing
    transactions.
  */
  bool m_gtid_only_mode;

  /**
    Are positions invalid. When true this means the values for
    receiver position related information might be outdated.
  */
  bool m_is_receiver_position_info_invalid;

 public:
  /*
    Hostname of the server where master_uuid was last read.
  */
  std::string m_uuid_from_host{};

  /*
    Port of the server where master_uuid was last read.
  */
  uint m_uuid_from_port{0};

 private:
  /// Is the collection of metrics enabled?
  bool m_is_metric_collection_enabled{false};

 public:
  /// @brief Enable or disable metric collection
  /// @param value true to enable, false to disable
  void set_applier_metric_collection_status(bool value) {
    m_is_metric_collection_enabled = value;
  }

  /// @brief Returns if metric collection is enabled on this channel
  /// @return True if yes, false if no
  bool is_metric_collection_enabled() { return m_is_metric_collection_enabled; }
};

#endif /* RPL_MI_H */
