/* Copyright (c) 2005, 2025, Oracle and/or its affiliates.

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

#ifndef RPL_RLI_H
#define RPL_RLI_H

#include <sys/types.h>
#include <time.h>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "lex_string.h"
#include "map_helpers.h"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "mysql/binlog/event/binlog_event.h"
#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/my_loglevel.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/thread_type.h"
#include "prealloced_array.h"  // Prealloced_array
#include "sql/binlog.h"        // MYSQL_BIN_LOG
#include "sql/changestreams/apply/metrics/applier_metrics.h"
#include "sql/changestreams/apply/metrics/applier_metrics_stub.h"
#include "sql/log_event.h"  //Gtid_log_event
#include "sql/psi_memory_key.h"
#include "sql/query_options.h"
#include "sql/rpl_gtid.h"         // Gtid_set
#include "sql/rpl_info.h"         // Rpl_info
#include "sql/rpl_mta_submode.h"  // enum_mts_parallel_type
#include "sql/rpl_replica_until_options.h"
#include "sql/rpl_tblmap.h"  // table_mapping
#include "sql/rpl_trx_boundary_parser.h"
#include "sql/rpl_utility.h"  // Deferred_log_events
#include "sql/sql_class.h"    // THD
#include "sql/system_variables.h"
#include "sql/table.h"
#include "strmake.h"

class Commit_order_manager;
class Master_info;
class Rpl_filter;
class Rpl_info_handler;
class Slave_committed_queue;
class Slave_worker;
class String;
struct LEX_SOURCE_INFO;
struct db_worker_hash_entry;

extern uint sql_replica_skip_counter;

typedef Prealloced_array<Slave_worker *, 4> Slave_worker_array;

typedef struct slave_job_item {
  Log_event *data;
  my_off_t relay_pos;
  char event_relay_log_name[FN_REFLEN + 1];
  bool m_is_after_metrics_breakpoint;
} Slave_job_item;

/**
  This class is used to store the type and value for
  Assign_gtids_to_anonymous_transactions parameter of Change replication source
  command on slave.
*/
class Assign_gtids_to_anonymous_transactions_info {
 public:
  /**
    This accepted value of the type of the
    Assign_gtids_to_anonymous_transactions info OFF :   Anonymous gtid events
    won't be converted to Gtid event. LOCAL:  Anonymous gtid events will be
    converted to Gtid event & the UUID used while create GTIDs will be the one
    of replica which is the server where this transformation of anonymous to
    gtid event happens. UUID: Anonymous gtid events will be converted to Gtid
    event & the UUID used while create GTIDs will be the one specified via
    Change replication source command to the parameter
    ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS
  */
  enum class enum_type { AGAT_OFF = 1, AGAT_LOCAL, AGAT_UUID };
  /**
    The default constructor initializes parameters to their default value
  */
  Assign_gtids_to_anonymous_transactions_info() {
    set_info(enum_type::AGAT_OFF, "");
    m_sidno = 0;
  }
  rpl_sidno get_sidno() const { return m_sidno; }
  enum_type get_type() const;
  std::string get_value() const;
  /*
    Here the assign_gtids_to_anonymous_transactions_value contains the textual
    representation of the UUID used while creating a GTID.
   */
  bool set_info(enum_type assign_gtids_to_anonymous_transactions_type,
                const char *assign_gtids_to_anonymous_transactions_value);

 private:
  /** This stores the type of Assign_gtids_to_anonymous_transactions info */
  enum_type m_type;
  /** Stores the UUID in case the m_type is not OFF */
  std::string m_value;
  // The sidno corresponding to the UUID value.
  rpl_sidno m_sidno;
};
/*******************************************************************************
Replication SQL Thread

Relay_log_info contains:
  - the current relay log
  - the current relay log offset
  - master log name
  - master log sequence corresponding to the last update
  - misc information specific to the SQL thread

Relay_log_info is initialized from a repository, i.e. table or file, if there is
one. Otherwise, data members are initialized with defaults by calling
init_relay_log_info().

The applier metadata shall be updated whenever: (i) the relay log file
is rotated, (ii) SQL Thread is stopped, (iii) while processing a Xid_log_event,
(iv) after a Query_log_event (i.e. commit or rollback) and (v) after processing
any statement written to the binary log without a transaction context.

The Xid_log_event is a commit for transactional engines and must be handled
differently to provide reliability/data integrity. In this case, positions
are updated within the context of the current transaction. So

  . If the relay.info is stored in a transactional repository and the server
  crashes before successfully committing the transaction the changes to the
  position table will be rolled back along with the data.

  . If the relay.info is stored in a non-transactional repository, for instance,
  a file or a system table created using MyIsam, and the server crashes before
  successfully committing the transaction the changes to the position table
  will not be rolled back but data will.

In particular, when there are mixed transactions, i.e a transaction that updates
both transaction and non-transactional engines, the Xid_log_event is still used
but reliability/data integrity cannot be achieved as we shall explain in what
follows.

Changes to non-transactional engines, such as MyIsam, cannot be rolled back if a
failure happens. For that reason, there is no point in updating the positions
within the boundaries of any on-going transaction. This is true for both commit
and rollback. If a failure happens after processing the pseudo-transaction but
before updating the positions, the transaction will be re-executed when the
slave is up most likely causing an error that needs to be manually circumvented.
This is a well-known issue when non-transactional statements are executed.

Specifically, if rolling back any transaction, positions are updated outside the
transaction boundaries. However, there may be a problem in this scenario even
when only transactional engines are updated. This happens because if there is a
rollback and such transaction is written to the binary log, a non-transactional
engine was updated or a temporary table was created or dropped within its
boundaries.

In particular, in both STATEMENT and MIXED logging formats, this happens because
any temporary table is automatically dropped after a shutdown/startup.
See BUG#26945 for further details.

Statements written to the binary log outside the boundaries of a transaction are
DDLs or maintenance commands which are not transactional. These means that they
cannot be rolled back if a failure happens. In such cases, the positions are
updated after processing the events. If a failure happens after processing the
statement but before updating the positions, the statement will be
re-executed when the slave is up most likely causing an error that needs to be
manually circumvented. This is a well-known issue when non-transactional
statements are executed.

The --sync-relay-log-info does not have effect when a system table, either
transactional or non-transactional is used.

To correctly recovery from failures, one should combine transactional system
tables along with the --relay-log-recovery.
*******************************************************************************/
class Relay_log_info : public Rpl_info {
  friend class Rpl_info_factory;

 public:
  /**
    Set of possible return values for the member methods related to
    `PRIVILEGE_CHECKS_USER` management.
   */
  enum class enum_priv_checks_status : int {
    /** Function ended successfully */
    SUCCESS = 0,
    /** Value for user is anonymous (''@'...') */
    USER_ANONYMOUS,
    /** Value for the username part of the user is larger than 32 characters */
    USERNAME_TOO_LONG,
    /** Value for the hostname part of the user is larger than 255 characters */
    HOSTNAME_TOO_LONG,
    /** Value for the hostname part of the user has illegal characters */
    HOSTNAME_SYNTAX_ERROR,
    /**
      Value for the username part of the user is NULL but the value for the
      hostname is not NULL.
     */
    USERNAME_NULL_HOSTNAME_NOT_NULL,
    /**
      Provided user doesn't exists.
     */
    USER_DOES_NOT_EXIST,
    /**
      Provided user doesn't have the necessary privileges to execute the needed
      operations.
     */
    USER_DOES_NOT_HAVE_PRIVILEGES,
    /** Values provided for the internal variables are corrupted. */
    USER_DATA_CORRUPTED,
    /**
      Provided user doesn't have `FILE` privileges during the execution of a
      `LOAD DATA`event.
     */
    LOAD_DATA_EVENT_NOT_ALLOWED
  };

  enum class enum_require_row_status : int {
    /** Function ended successfully */
    SUCCESS = 0,
    /** Value for `privilege_checks_user` is not empty */
    PRIV_CHECKS_USER_NOT_NULL
  };

  /*
    The per-channel filter associated with this RLI
  */
  Rpl_filter *rpl_filter;
  /**
     Flags for the state of the replication.
   */
  enum enum_state_flag {
    /** The replication thread is inside a statement */
    IN_STMT,

    /** Flag counter.  Should always be last */
    STATE_FLAGS_COUNT
  };

  /**
    Identifies what is the replica policy on primary keys in tables.
  */
  enum enum_require_table_primary_key {
    /**No policy, used on PFS*/
    PK_CHECK_NONE = 0,
    /**
      The replica sets the value of sql_require_primary_key according to
      the source replicated value.
    */
    PK_CHECK_STREAM = 1,
    /** The replica enforces tables to have primary keys for a given channel*/
    PK_CHECK_ON = 2,
    /** The replica does not enforce any policy around primary keys*/
    PK_CHECK_OFF = 3,
    /** The replica generates GIPKs for incoming keyless tables*/
    PK_CHECK_GENERATE = 4
  };

  /**
    Stores the information related to the ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS
    parameter of CHANGE REPLICATION SOURCE
  */
  Assign_gtids_to_anonymous_transactions_info
      m_assign_gtids_to_anonymous_transactions_info;

  /*
    The SQL thread owns one Relay_log_info, and each client that has
    executed a BINLOG statement owns one Relay_log_info. This function
    returns zero for the Relay_log_info object that belongs to the SQL
    thread and nonzero for Relay_log_info objects that belong to
    clients.
  */
  inline bool belongs_to_client() {
    assert(info_thd);
    return !info_thd->slave_thread;
  }
/* Instrumentation key for performance schema for mts_temp_table_LOCK */
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key m_key_mta_temp_table_LOCK;
#endif
  /*
     Lock to protect race condition while transferring temporary table from
     worker thread to coordinator thread and vice-versa
   */
  mysql_mutex_t mts_temp_table_LOCK;
  /*
     Lock to acquire by methods that concurrently update lwm of committed
     transactions and the min waited timestamp and its index.
  */
  mysql_mutex_t mts_gaq_LOCK;
  mysql_cond_t logical_clock_cond;
  /*
    If true, events with the same server id should be replicated. This
    field is set on creation of a relay log info structure by copying
    the value of ::replicate_same_server_id and can be overridden if
    necessary. For example of when this is done, check sql_binlog.cc,
    where the BINLOG statement can be used to execute "raw" events.
   */
  bool replicate_same_server_id;

  /*
    Protected with internal locks.
    Must get data_lock when resetting the logs.
  */
  MYSQL_BIN_LOG relay_log;

  /*
    Identifies when the recovery process is going on.
    See sql/rpl_replica.h:init_recovery for further details.
  */
  bool is_relay_log_recovery;

  /* The following variables are safe to read any time */

  /*
    When we restart replica thread we need to have access to the previously
    created temporary tables. Modified only on init/end and by the SQL
    thread, read only by SQL thread.
  */
  TABLE *save_temporary_tables;

  /* parent Master_info structure */
  Master_info *mi;

  /* number of temporary tables open in this channel */
  std::atomic<int32> atomic_channel_open_temp_tables{0};

  /** the status of the commit timestamps for the relay log */
  enum {
    /*
      no GTID log event has been processed, so it is not known if this log
      has commit timestamps
    */
    COMMIT_TS_UNKNOWN,
    // the immediate master does not support commit timestamps
    COMMIT_TS_NOT_FOUND,
    // the immediate master supports commit timestamps
    COMMIT_TS_FOUND
  } commit_timestamps_status;

  /**
    @return the pointer to the Gtid_monitoring_info.
  */
  Gtid_monitoring_info *get_gtid_monitoring_info() {
    return gtid_monitoring_info;
  }

  /**
    Stores the details of the transaction which has just started processing.

    This function is called by the STS applier or MTS worker when applying a
    Gtid.

    @param  gtid_arg         the gtid of the trx
    @param  original_ts_arg  the original commit timestamp of the transaction
    @param  immediate_ts_arg the immediate commit timestamp of the transaction
    @param  skipped          true if the transaction was gtid skipped
  */
  void started_processing(Gtid gtid_arg, ulonglong original_ts_arg,
                          ulonglong immediate_ts_arg, bool skipped = false) {
    gtid_monitoring_info->start(gtid_arg, original_ts_arg, immediate_ts_arg,
                                skipped);
  }

  /**
    Stores the details of the transaction which has just started processing.

    This function is called by the MTS coordinator when queuing a Gtid to
    a worker.

    @param  gtid_log_ev_arg the gtid log event of the trx
  */
  void started_processing(Gtid_log_event *gtid_log_ev_arg) {
    Gtid gtid = {0, 0};
    if (gtid_log_ev_arg->get_type() == ASSIGNED_GTID) {
      gtid = {gtid_log_ev_arg->get_sidno(true), gtid_log_ev_arg->get_gno()};
    }
    started_processing(gtid, gtid_log_ev_arg->original_commit_timestamp,
                       gtid_log_ev_arg->immediate_commit_timestamp);
  }

  /**
    When the processing of a transaction is completed, that timestamp is
    recorded, the information is copied to last_processed_trx and the
    information in processing_trx is cleared.

    If the transaction was "applied" but GTID-skipped, the copy will not
    happen and the last_processed_trx will keep its current value.
  */
  void finished_processing() { gtid_monitoring_info->finish(); }

  /**
    @return True if there is a transaction being currently processed
  */
  bool is_processing_trx() {
    return gtid_monitoring_info->is_processing_trx_set();
  }

  /**
   Clears the processing_trx structure fields. Normally called when there is an
   error while processing the transaction.
  */
  void clear_processing_trx() { gtid_monitoring_info->clear_processing_trx(); }

  /**
    Clears the Gtid_monitoring_info fields.
  */
  void clear_gtid_monitoring_info() { gtid_monitoring_info->clear(); }

  /**
   When a transaction is retried, the error number and message, and total number
   of retries are stored. The timestamp for this error is also set here.

   @param transient_errno_arg        Transient error number.
   @param transient_err_message_arg  Transient error message.
   @param trans_retries_arg          Number of times this transaction has been
                                     retried so far.
  */
  void retried_processing(uint transient_errno_arg,
                          const char *transient_err_message_arg,
                          ulong trans_retries_arg) {
    gtid_monitoring_info->store_transient_error(
        transient_errno_arg, transient_err_message_arg, trans_retries_arg);
  }

  /*
    If on init_info() call error_on_rli_init_info is true that means
    that previous call to init_info() terminated with an error, RESET
    SLAVE must be executed and the problem fixed manually.
   */
  bool error_on_rli_init_info;

  /**
    Retrieves the username part of the `PRIVILEGE_CHECKS_USER` option of `CHANGE
    MASTER TO` statement.

    @return a string holding the username part of the user or an empty string.
   */
  std::string get_privilege_checks_username() const;

  /**
    Retrieves the host part of the `PRIVILEGE_CHECKS_USER` option of `CHANGE
    MASTER TO` statement.

    @return a string holding the host part of the user or an empty string.
   */
  std::string get_privilege_checks_hostname() const;

  /**
    Returns whether or not there is no user configured for
    `PRIVILEGE_CHECKS_USER`.

    @return true if there is no user configured for `PRIVILEGE_CHECKS_USER` and
            false otherwise.
   */
  bool is_privilege_checks_user_null() const;

  /**
    Returns whether or not the internal data regarding `PRIVILEGE_CHECKS_USER`
    is corrupted. This may happen, for instance, if the user tries to change the
    Relay_log_info repository manually or after a server crash.

    @return true if the data is corrupted, false otherwise.
   */
  bool is_privilege_checks_user_corrupted() const;

  /**
    Clears the info related to the data initialized from
    `PRIVILEGE_CHECKS_USER`.
   */
  void clear_privilege_checks_user();

  /**
    Sets the flag that tells whether or not the data regarding the
    `PRIVILEGE_CHECKS_USER` is corrupted.

    @param is_corrupted the flag value.
   */
  void set_privilege_checks_user_corrupted(bool is_corrupted);

  /**
    Initializes data related to `PRIVILEGE_CHECKS_USER`, specifically the user
    name and the user hostname.

    @param param_privilege_checks_username the username part of the user.
    @param param_privilege_checks_hostname the hostname part of the user.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status set_privilege_checks_user(
      char const *param_privilege_checks_username,
      char const *param_privilege_checks_hostname);

  /**
    Checks the validity and integrity of the data related to
    `PRIVILEGE_CHECKS_USER`, specifically the user name and the user
    hostname. Also checks if the user exists.

    This method takes no parameters as it checks the values stored in the
    internal member variables.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status check_privilege_checks_user();

  /**
    Checks the validity and integrity of the data related to
    `PRIVILEGE_CHECKS_USER`, specifically the user name and the user
    hostname. Also checks if the user exists.

    @param param_privilege_checks_username the username part of the user.
    @param param_privilege_checks_hostname the hostname part of the user.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status check_privilege_checks_user(
      char const *param_privilege_checks_username,
      char const *param_privilege_checks_hostname);
  /**
    Checks the existence of user provided as part of the `PRIVILEGE_CHECKS_USER`
    option.

    @param param_privilege_checks_username the username part of the user.
    @param param_privilege_checks_hostname the host part of the user.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status check_applier_acl_user(
      char const *param_privilege_checks_username,
      char const *param_privilege_checks_hostname);

  /**
    Returns a printable representation of the username and hostname currently
    being used in the applier security context or empty strings other wise.

    @return an `std::pair` containing the username and the hostname printable
            representations.
   */
  std::pair<const char *, const char *>
  print_applier_security_context_user_host() const;

  /**
    Outputs the error message associated with applier thread user privilege
    checks error `error_code`.

    The output stream to which is outputted is decided based on `to_client`
    which, if set to `true` will output the message to the client session and if
    `false` will output to the server log.

    @param level the message urgency level, e.g., `ERROR_LEVEL`,
                 `WARNING_LEVEL`, etc.
    @param status_code the status code to output the associated error message
                       for.
    @param to_client a flag indicating if the message should be sent to the
                     client session or to the server log.
    @param channel_name name of the channel for which the error is being
                        reported.
    @param user_name username for which the error is being reported.
    @param host_name hostname for which the error is being reported.
   */
  void report_privilege_check_error(enum loglevel level,
                                    enum_priv_checks_status status_code,
                                    bool to_client,
                                    char const *channel_name = nullptr,
                                    char const *user_name = nullptr,
                                    char const *host_name = nullptr) const;

  /**
    Initializes the security context associated with the `PRIVILEGE_CHECKS_USER`
    user that is to be used by the provided THD object.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status initialize_security_context(THD *thd);
  /**
    Initializes the security context associated with the `PRIVILEGE_CHECKS_USER`
    user that is to be used by the applier thread.

    @return a status code describing the state of the data initialization.
   */
  enum_priv_checks_status initialize_applier_security_context();

  /**
    Returns whether the slave is running in row mode only.

    @return true if row_format_required is active, false otherwise.
   */
  bool is_row_format_required() const;

  /**
    Sets the flag that tells whether or not the slave is running in row mode
    only.

    @param require_row the flag value.
   */
  void set_require_row_format(bool require_row);

  /**
     Returns what is the slave policy concerning primary keys on
     replicated tables.

     @return STREAM if it replicates the source values, ON if it enforces the
             need on primary keys, OFF if it does no enforce any restrictions,
             GENERATE if a GIPK is added to the table.
   */
  enum_require_table_primary_key get_require_table_primary_key_check() const;

  /**
    Sets the field that tells what is the slave policy concerning primary keys
    on replicated tables.

    @param require_pk the policy value.
  */
  void set_require_table_primary_key_check(
      enum_require_table_primary_key require_pk);

  /*
    This will be used to verify transactions boundaries of events being applied

    Its output is used to detect when events were not logged using row based
    logging.
  */
  Replication_transaction_boundary_parser transaction_parser;

  /**
    Marks the applier position information as being invalid or not.

    @param invalid value to set the position/file info as invalid or not
  */
  void set_applier_source_position_info_invalid(bool invalid);

  /**
    Returns if the applier positions are marked as being invalid or not.

    @return true if applier position information is not reliable,
            false otherwise.
  */
  bool is_applier_source_position_info_invalid() const;

  /*
    Let's call a group (of events) :
      - a transaction
      or
      - an autocommitting query + its associated events (INSERT_ID,
    TIMESTAMP...)
    We need these rli coordinates :
    - relay log name and position of the beginning of the group we currently are
    executing. Needed to know where we have to restart when replication has
    stopped in the middle of a group (which has been rolled back by the slave).
    - relay log name and position just after the event we have just
    executed. This event is part of the current group.
    Formerly we only had the immediately above coordinates, plus a 'pending'
    variable, but this dealt wrong with the case of a transaction starting on a
    relay log and finishing (committing) on another relay log. Case which can
    happen when, for example, the relay log gets rotated because of
    max_binlog_size.
  */
 protected:
  /**
     Event group means a group of events of a transaction. group_relay_log_name
     and group_relay_log_pos record the place before where all event groups
     are applied. When slave starts, it resume to apply events from
     group_relay_log_pos. They will be initialized to the begin of the first
     relay log file if it is a new slave(including SLAVE RESET). Then,
     group_relay_log_pos is advanced after each transaction is applied
     successfully in single thread slave. For MTS, group_relay_log_pos
     is updated by mts checkpoint mechanism. group_relay_log_pos and
     group_relay_log_name are stored into relay_log_info file/table
     periodically. When server startup, they are loaded from relay log info
     file/table.
   */
  char group_relay_log_name[FN_REFLEN];
  ulonglong group_relay_log_pos;
  char event_relay_log_name[FN_REFLEN];
  ulonglong event_relay_log_pos;
  ulonglong future_event_relay_log_pos;

 public:
  /**
     Process an event and based on its type () set group beginning and end
     @param ev - event within a group (including first and last)
   */
  void set_group_source_log_start_end_pos(const Log_event *ev);
  /**
     Get event group positions in source binary log on a replica which is
     processed by a worker in MTA or coordinator in STA.
     @return source event group start and end position in binary log
   */
  std::tuple<ulonglong, ulonglong> get_group_source_log_start_end_pos() const;

 private:
  /**
   * Event group beginning event has been seen. Event group may begin with two
   * events marked as beginning.
   * @see set_group_source_log_start_end_pos
   */
  bool group_source_log_seen_start_pos;
  /**
   * @see set_group_source_log_start_end_pos, get_group_source_log_start_end_pos
   */
  ulonglong group_source_log_start_pos;
  /**
   * @see set_group_source_log_start_end_pos, get_group_source_log_start_end_pos
   */
  ulonglong group_source_log_end_pos;

 protected:
  /* current event's start position in relay log */
  my_off_t event_start_pos;
  /*
     Original log name and position of the group we're currently executing
     (whose coordinates are group_relay_log_name/pos in the relay log)
     in the master's binlog. These concern the *group*, because in the master's
     binlog the log_pos that comes with each event is the position of the
     beginning of the group.

    Note: group_master_log_name, group_master_log_pos must only be
    written from the thread owning the Relay_log_info (SQL thread if
    !belongs_to_client(); client thread executing BINLOG statement if
    belongs_to_client()).
  */
  char group_master_log_name[FN_REFLEN];
  volatile my_off_t group_master_log_pos;

 private:
  Gtid_set *gtid_set;
  /*
    Identifies when this object belongs to the SQL thread and was not
    created for a client thread or some other purpose including
    Slave_worker instance initializations. Ends up serving the same
    purpose as the belongs_to_client method, but its value is set
    earlier on in the class constructor.
  */
  bool rli_fake;
  /* Flag that ensures the retrieved GTID set is initialized only once. */
  bool gtid_retrieved_initialized;

  /// Flag that ensures the relay log is sanitized only once.
  bool relay_log_sanitized = false;

  /**
    Stores information on the last processed transaction or the transaction
    that is currently being processed.

    STS:
    - timestamps of the currently applying/last applied transaction

    MTS:
    - coordinator thread: timestamps of the currently scheduling/last scheduled
      transaction in a worker's queue
    - worker thread: timestamps of the currently applying/last applied
      transaction
  */
  Gtid_monitoring_info *gtid_monitoring_info;

  /**
     It will be set to true when receiver truncated relay log for some reason.
     The truncated data may already be read by applier. So applier need to check
     it each time the binlog_end_pos is updated.
   */
  bool m_relay_log_truncated = false;

  /**
    The user name part of the user passed on to `PRIVILEGE_CHECKS_USER`.
   */
  std::string m_privilege_checks_username;

  /**
    The host name part of the user passed on to `PRIVILEGE_CHECKS_USER`.
   */
  std::string m_privilege_checks_hostname;

  /**
    Tells whether or not the internal data regarding `PRIVILEGE_CHECKS_USER` is
    corrupted. This may happen if the user tries to change the Relay_log_info
    repository by hand.
   */
  bool m_privilege_checks_user_corrupted;

  /**
   Tells if the slave is only accepting events logged with row based logging.
   It also blocks
     Operations with temporary table creation/deletion
     Operations with LOAD DATA
     Events: INTVAR_EVENT, RAND_EVENT, USER_VAR_EVENT
  */
  bool m_require_row_format;

  /**
    Identifies what is the slave policy on primary keys in tables.
    If set to STREAM it just replicates the value of sql_require_primary_key.
    If set to ON it fails when the source tries to replicate a table creation
    or alter operation that does not have a primary key.
    If set to OFF it does not enforce any policies on the channel for primary
    keys.
    If set to GENERATE it adds GIPKs to tables that are created without a PK
    in the replica applier threads.
  */
  enum_require_table_primary_key m_require_table_primary_key_check;

  /**
    Are positions invalid. If true it means the applier related position
    information (group_master_log_name and group_master_log_pos) might
    be outdated.

    Check also is_group_master_log_pos_invalid
  */
  bool m_is_applier_source_position_info_invalid;

 public:
  bool is_relay_log_truncated() { return m_relay_log_truncated; }

  Tsid_map *get_tsid_map() { return gtid_set->get_tsid_map(); }

  Checkable_rwlock *get_tsid_lock() { return get_tsid_map()->get_tsid_lock(); }

  void add_logged_gtid(rpl_sidno sidno, rpl_gno gno) {
    get_tsid_lock()->assert_some_lock();
    assert(sidno <= get_tsid_map()->get_max_sidno());
    gtid_set->ensure_sidno(sidno);
    gtid_set->_add_gtid(sidno, gno);
  }

  /**
    Adds a GTID set to received GTID set.

    @param gtid_set the gtid_set to add

    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status add_gtid_set(const Gtid_set *gtid_set);

  const Gtid_set *get_gtid_set() const { return gtid_set; }

  bool reinit_sql_thread_io_cache(const char *log, bool need_data_lock);

  /**
     Check if group_relay_log_name is in index file.

     @param [out] errmsg An error message is returned if error happens.

     @retval    false    It is valid.
     @retval    true     It is invalid. In this case, *errmsg is set to point to
                         the error message.
*/
  bool is_group_relay_log_name_invalid(const char **errmsg);
  /**
     Reset group_relay_log_name and group_relay_log_pos to the start of the
     first relay log file. The caller must hold data_lock.

     @param[out]     errmsg    An error message is set into it if error happens.

     @retval    false    Success
     @retval    true     Error
   */
  bool reset_group_relay_log_pos(const char **errmsg);
  /*
    Update the error number, message and timestamp fields. This function is
    different from va_report() as va_report() also logs the error message in the
    log apart from updating the error fields.
  */
  void fill_coord_err_buf(loglevel level, int err_code,
                          const char *buff_coord) const;

  /**
    Flag that the group_master_log_pos is invalid. This may occur
    (for example) after CHANGE REPLICATION SOURCE TO RELAY_LOG_POS.  This will
    be unset after the first event has been executed and the
    group_master_log_pos is valid again.

    Check also m_is_applier_position_info_invalid
  */
  bool is_group_master_log_pos_invalid;

  /*
    Handling of the relay_log_space_limit optional constraint.
  */
  std::atomic<ulonglong> log_space_limit, log_space_total;

  // This flag is used by a coordinator to check if the receiver waits for
  // a relay log space. If yes, it will enable aggressive relay log
  // purge.
  std::atomic_bool is_receiver_waiting_for_rl_space;

  // This is file to which coordinator moved after enforced purge
  // It is used by the receiver to check if all possible files were purged
  // before making a decision on whether transaction may fit into the
  // relay_log_space_limit. It is used to avoid possibly infinite waiting in
  // case a transaction and required relay log metadata is bigger than
  // 'relay_log_space_limit'. This filename is protected with the
  // log_space_lock
  std::string coordinator_log_after_purge{""};

  time_t last_master_timestamp;

  /**
    Reset the delay.
    This is used by RESET REPLICA to clear the delay.
  */
  void clear_sql_delay() { sql_delay = 0; }

  /*
    Needed for problems when slave stops and we want to restart it
    skipping one or more events in the master log that have caused
    errors, and have been manually applied by DBA already.
  */
  std::atomic<uint32> slave_skip_counter;
  std::atomic<ulong>
      abort_pos_wait; /* Incremented on change replication source */
  mysql_mutex_t log_space_lock;
  mysql_cond_t log_space_cond;

  /*
     Condition and its parameters from START REPLICA UNTIL clause.

     UNTIL condition is tested with is_until_satisfied() method that is
     called by exec_relay_log_event(). is_until_satisfied() caches the result
     of the comparison of log names because log names don't change very often;
     this cache is invalidated by parts of code which change log names with
     notify_*_log_name_updated() methods. (They need to be called only if SQL
     thread is running).
   */
  enum {
    UNTIL_NONE = 0,
    UNTIL_MASTER_POS,
    UNTIL_RELAY_POS,
    UNTIL_SQL_BEFORE_GTIDS,
    UNTIL_SQL_AFTER_GTIDS,
    UNTIL_SQL_AFTER_MTS_GAPS,
    UNTIL_SQL_VIEW_ID,
    UNTIL_DONE
  } until_condition;

  char cached_charset[6];

  /*
    trans_retries varies between 0 to replica_transaction_retries and counts how
    many times the slave has retried the present transaction; gets reset to 0
    when the transaction finally succeeds. retried_trans is a cumulative
    counter: how many times the slave has retried a transaction (any) since
    slave started.
  */
  ulong trans_retries, retried_trans;

  /*
    If the end of the hot relay log is made of master's events ignored by the
    slave I/O thread, these two keep track of the coords (in the master's
    binlog) of the last of these events seen by the slave I/O thread. If not,
    ign_master_log_name_end[0] == 0.
    As they are like a Rotate event read/written from/to the relay log, they
    are both protected by rli->relay_log.LOCK_binlog_end_pos.
  */
  char ign_master_log_name_end[FN_REFLEN];
  ulonglong ign_master_log_pos_end;

  /*
    Identifies where the SQL Thread should create temporary files for the
    LOAD DATA INFILE. This is used for security reasons.
   */
  char slave_patternload_file[FN_REFLEN];
  size_t slave_patternload_file_size;

  /**
    Identifies the last time a checkpoint routine has been executed.
  */
  struct timespec last_clock;

  /**
    Invalidates cached until_log_name and event_relay_log_name comparison
    result. Should be called after switch to next relay log if
    there chances that sql_thread is running.
  */
  inline void notify_relay_log_change() {
    if (until_condition == UNTIL_RELAY_POS)
      down_cast<Until_position *>(until_option)->notify_log_name_change();
  }

  /**
     Receiver thread notifies that it truncated some data from relay log.
     data_lock will be acquired, so the caller should not hold data_lock.
  */
  void notify_relay_log_truncated();
  /**
     Applier clears the flag after it handled the situation. The caller must
     hold data_lock.
  */
  void clear_relay_log_truncated();

  /**
    The same as @c notify_group_relay_log_name_update but for
    @c group_master_log_name.
  */
  inline void notify_group_master_log_name_update() {
    if (until_condition == UNTIL_MASTER_POS)
      down_cast<Until_position *>(until_option)->notify_log_name_change();
  }

  inline void inc_event_relay_log_pos() {
    event_relay_log_pos = future_event_relay_log_pos;
  }

  /**
    Last executed event group coordinates are updated and optionally
    forcibly flushed to a repository.
    @param log_pos         a value of the executed position to update to
    @param need_data_lock  whether data_lock should be acquired
    @param force           the value is passed to eventual flush_info()
  */
  int inc_group_relay_log_pos(ulonglong log_pos, bool need_data_lock,
                              bool force = false);

  int wait_for_pos(THD *thd, String *log_name, longlong log_pos,
                   double timeout);
  /**
    Wait for a GTID set to be executed.

    @param thd                 The thread for status changes and kill status
    @param gtid                A char array with a GTID set
    @param timeout             Number of seconds to wait before timing out
    @param update_THD_status  Shall the method update the THD stage

    @retval 0  The set is already executed
    @retval -1 There was a timeout waiting for the set
    @retval -2 There was an issue while waiting.
   */
  int wait_for_gtid_set(THD *thd, const char *gtid, double timeout,
                        bool update_THD_status = true);
  /**
    Wait for a GTID set to be executed.

    @param thd                 The thread for status changes and kill status
    @param gtid                A String with a GTID set
    @param timeout             Number of seconds to wait before timing out
    @param update_THD_status  Shall the method update the THD stage

    @retval 0  The set is already executed
    @retval -1 There was a timeout waiting for the set
    @retval -2 There was an issue while waiting.
  */
  int wait_for_gtid_set(THD *thd, String *gtid, double timeout,
                        bool update_THD_status = true);
  /**
    Wait for a GTID set to be executed.

    @param thd                 The thread for status changes and kill status
    @param wait_gtid_set       A GTID_set object
    @param timeout             Number of seconds to wait before timing out
    @param update_THD_status   Shall the method update the THD stage

    @retval 0  The set is already executed
    @retval -1 There was a timeout waiting for the set
    @retval -2 There was an issue while waiting.
  */
  int wait_for_gtid_set(THD *thd, const Gtid_set *wait_gtid_set, double timeout,
                        bool update_THD_status = true);

  void close_temporary_tables();

  RPL_Table_ref *tables_to_lock; /* RBR: Tables to lock  */
  uint tables_to_lock_count;     /* RBR: Count of tables to lock */
  table_mapping m_table_map;     /* RBR: Mapping table-id to table */
  /* RBR: Record Rows_query log event */
  Rows_query_log_event *rows_query_ev;

  bool get_table_data(TABLE *table_arg, table_def **tabledef_var,
                      TABLE **conv_table_var) const {
    assert(tabledef_var && conv_table_var);
    for (Table_ref *ptr = tables_to_lock; ptr != nullptr;
         ptr = ptr->next_global)
      if (ptr->table == table_arg) {
        *tabledef_var = &static_cast<RPL_Table_ref *>(ptr)->m_tabledef;
        *conv_table_var = static_cast<RPL_Table_ref *>(ptr)->m_conv_table;
        DBUG_PRINT("debug", ("Fetching table data for table %s.%s:"
                             " tabledef: %p, conv_table: %p",
                             table_arg->s->db.str, table_arg->s->table_name.str,
                             *tabledef_var, *conv_table_var));
        return true;
      }
    return false;
  }

  /**
    Last charset (6 bytes) seen by slave SQL thread is cached here; it helps
    the thread save 3 @c get_charset() per @c Query_log_event if the charset is
    not changing from event to event (common situation). When the 6 bytes are
    equal to 0 is used to mean "cache is invalidated".
  */
  void cached_charset_invalidate();
  bool cached_charset_compare(char *charset) const;

  void cleanup_context(THD *, bool);
  void slave_close_thread_tables(THD *);
  void clear_tables_to_lock();
  int purge_relay_logs(THD *thd, const char **errmsg, bool delete_only = false);

  /*
    Used to defer stopping the SQL thread to give it a chance
    to finish up the current group of events.
    The timestamp is set and reset in @c sql_slave_killed().
  */
  time_t last_event_start_time;

  /* The original master commit timestamp in microseconds since epoch */
  uint64 original_commit_timestamp;

  /*
    A container to hold on Intvar-, Rand-, Uservar- log-events in case
    the slave is configured with table filtering rules.
    The withhold events are executed when their parent Query destiny is
    determined for execution as well.
  */
  Deferred_log_events *deferred_events;

  /*
    State of the container: true stands for IRU events gathering,
    false does for execution, either deferred or direct.
  */
  bool deferred_events_collecting;

  /*****************************************************************************
    WL#5569 MTS

    legends:
    C  - Coordinator;
    W  - Worker;
    WQ - Worker Queue containing event assignments
  */
  // number's is determined by global replica_parallel_workers
  Slave_worker_array workers;

  // To map a database to a worker
  malloc_unordered_map<std::string,
                       unique_ptr_with_deleter<db_worker_hash_entry>>
      mapping_db_to_worker{key_memory_db_worker_hash_entry};
  bool inited_hash_workers;  //  flag to check if mapping_db_to_worker is inited

  mysql_mutex_t slave_worker_hash_lock;  // for mapping_db_to_worker
  mysql_cond_t slave_worker_hash_cond;   // for mapping_db_to_worker

  /*
    For the purpose of reporting the worker status in performance schema table,
    we need to preserve the workers array after worker thread was killed. So, we
    copy this array into the below vector which is used for reporting
    until next init_workers(). Note that we only copy those attributes that
    would be useful in reporting worker status. We only use a few attributes in
    this object as of now but still save the whole object. The idea is
    to be future proof. We will extend performance schema tables in future
    and then we would use a good number of attributes from this object.
  */

  std::vector<Slave_worker *> workers_copy_pfs;

  /*
    This flag is turned ON when the workers array is initialized.
    Before destroying the workers array we check this flag to make sure
    we are not destroying an uninitialized array. For the purpose of reporting
    the worker status in performance schema table, we need to preserve the
    workers array after worker thread was killed. So, we copy this array into
    workers_copy_pfs array which is used for reporting until next
    init_workers().
  */
  bool workers_array_initialized;

  std::atomic<ulong> pending_jobs;
  mysql_mutex_t pending_jobs_lock;
  mysql_cond_t pending_jobs_cond;
  mysql_mutex_t exit_count_lock;  // mutex of worker exit count
  ulong mts_slave_worker_queue_len_max;
  ulonglong mts_pending_jobs_size;      // actual mem usage by WQ:s
  ulonglong mts_pending_jobs_size_max;  // max of WQ:s size forcing C to wait
  bool mts_wq_oversize;  // C raises flag to wait some memory's released
  Slave_worker
      *last_assigned_worker;  // is set to a Worker at assigning a group
  /*
    master-binlog ordered queue of Slave_job_group descriptors of groups
    that are under processing. The queue size is @c checkpoint_group.
  */
  Slave_committed_queue *gaq;
  /*
    Container for references of involved partitions for the current event group
  */
  // CGAP dynarray holds id:s of partitions of the Current being executed Group
  Prealloced_array<db_worker_hash_entry *, 4> curr_group_assigned_parts;
  // deferred array to hold partition-info-free events
  Prealloced_array<Slave_job_item, 8> curr_group_da;

  bool curr_group_seen_gtid;   // current group started with Gtid-event or not
  bool curr_group_seen_begin;  // current group started with B-event or not
  bool curr_group_isolated;    // current group requires execution in isolation
  bool mts_end_group_sets_max_dbs;  // flag indicates if partitioning info is
                                    // discovered
  volatile ulong
      mts_wq_underrun_w_id;  // Id of a Worker whose queue is getting empty
  /*
     Ongoing excessive overrun counter to correspond to number of events that
     are being scheduled while a WQ is close to be filled up.
     `Close' is defined as (100 - mts_worker_underrun_level) %.
     The counter is incremented each time a WQ get filled over that level
     and decremented when the level drops below.
     The counter therefore describes level of saturation that Workers
     are experiencing and is used as a parameter to compute a nap time for
     Coordinator in order to avoid reaching WQ limits.
  */
  std::atomic<long> mts_wq_excess_cnt;
  ulonglong mts_groups_assigned;    // number of groups (transactions) scheduled
  long mts_worker_underrun_level;   // % of WQ size at which W is considered
                                    // hungry
  ulong mts_coordinator_basic_nap;  // C sleeps to avoid WQs overrun
  ulong
      opt_replica_parallel_workers;  // cache for ::opt_replica_parallel_workers
  ulong
      replica_parallel_workers;  // the one slave session time number of workers
  ulong
      exit_counter;  // Number of workers contributed to max updated group index
  ulonglong max_updated_index;
  ulong recovery_parallel_workers;  // number of workers while recovering
  uint rli_checkpoint_seqno;        // counter of groups executed after the most
                                    // recent CP
  uint checkpoint_group;            // cache for ::opt_mta_checkpoint_group
  MY_BITMAP recovery_groups;        // bitmap used during recovery
  bool recovery_groups_inited;
  ulong mts_recovery_group_cnt;  // number of groups to execute at recovery
  ulong mts_recovery_index;      // running index of recoverable groups
  bool mts_recovery_group_seen_begin;

  /*
    While distributing events based on their properties MTS
    Coordinator changes its mts group status.
    Transition normally flowws to follow `=>' arrows on the diagram:

            +----------------------------+
            V                            |
    MTS_NOT_IN_GROUP =>                  |
        {MTS_IN_GROUP => MTS_END_GROUP --+} while (!killed) => MTS_KILLED_GROUP

    MTS_END_GROUP has `->' loop breaking link to MTS_NOT_IN_GROUP when
    Coordinator synchronizes with Workers by demanding them to
    complete their assignments.
  */
  enum {
    /*
       no new events were scheduled after last synchronization,
       includes Single-Threaded-Slave case.
    */
    MTS_NOT_IN_GROUP,

    MTS_IN_GROUP,    /* at least one not-terminal event scheduled to a Worker */
    MTS_END_GROUP,   /* the last scheduled event is a terminal event */
    MTS_KILLED_GROUP /* Coordinator gave up to reach MTS_END_GROUP */
  } mts_group_status;

 private:
  /// @brief The applier metrics aggregator
  cs::apply::instruments::Applier_metrics m_coordinator_metrics;
  /// @brief Empty metric aggregator when metric collection is not active
  cs::apply::instruments::Applier_metrics_stub m_disabled_metric_aggregator;

 public:
  /// @brief Returns the replication applier metrics aggregator
  /// @return the MTA metrics aggregator
  cs::apply::instruments::Applier_metrics_interface &get_applier_metrics();

  /// Number of times queue memory is exceeded
  ulong worker_queue_mem_exceeded_count{0};

  /// Last moment in time the MTA printed a coordinator waited stats
  time_t mta_coordinator_has_waited_stat;

  /**
    Storage for holding newly computed values for the last executed
    event group coordinates while the current group of events is
    being committed, see @c pre_commit, post_commit.
  */
  char new_group_master_log_name[FN_REFLEN];
  my_off_t new_group_master_log_pos;
  char new_group_relay_log_name[FN_REFLEN];
  my_off_t new_group_relay_log_pos;

  /* Returns the number of elements in workers array/vector. */
  inline size_t get_worker_count() {
    if (workers_array_initialized)
      return workers.size();
    else
      return workers_copy_pfs.size();
  }

  /*
    Returns a pointer to the worker instance at index n in workers
    array/vector.
  */
  Slave_worker *get_worker(size_t n) {
    if (workers_array_initialized) {
      if (n >= workers.size()) return nullptr;

      return workers[n];
    } else if (workers_copy_pfs.size()) {
      if (n >= workers_copy_pfs.size()) return nullptr;

      return workers_copy_pfs[n];
    } else
      return nullptr;
  }

  /**
    The method implements updating a slave info table. It's
    specialized differently for STS and MTS.
  */
  virtual bool commit_positions();

  /*Channel defined mts submode*/
  enum_mts_parallel_type channel_mts_submode;
  /* MTS submode  */
  Mts_submode *current_mts_submode;

  /* most of allocation in the coordinator rli is there */
  void init_workers(ulong);

  /* counterpart of the init */
  void deinit_workers();

  /**
     returns true if there is any gap-group of events to execute
                  at slave starting phase.
  */
  inline bool is_mts_recovery() const { return mts_recovery_group_cnt != 0; }

  inline void clear_mts_recovery_groups() {
    if (recovery_groups_inited) {
      bitmap_free(&recovery_groups);
      mts_recovery_group_cnt = 0;
      recovery_groups_inited = false;
    }
  }

  /**
     returns true if events are to be executed in parallel
  */
  inline bool is_parallel_exec() const {
    bool ret = (replica_parallel_workers > 0) && !is_mts_recovery();

    assert(!ret || !workers.empty());

    return ret;
  }

  /**
     returns true if Coordinator is scheduling events belonging to
     the same group and has not reached yet its terminal event.
  */
  inline bool is_mts_in_group() {
    return is_parallel_exec() && mts_group_status == MTS_IN_GROUP;
  }

  /**
     Check if it is time to compute MTS checkpoint.

     @retval true   It is time to compute MTS checkpoint.
     @retval false  It is not MTS or it is not time for computing checkpoint.
  */
  bool is_time_for_mta_checkpoint();
  /**
     While a group is executed by a Worker the relay log can change.
     Coordinator notifies Workers about this event. Worker is supposed
     to commit to the recovery table with the new info.
  */
  void reset_notified_relay_log_change();

  /**
     While a group is executed by a Worker the relay log can change.
     Coordinator notifies Workers about this event. Coordinator and Workers
     maintain a bitmap of executed group that is reset with a new checkpoint.
  */
  void reset_notified_checkpoint(ulong count, time_t new_ts,
                                 bool update_timestamp = false);

  /**
     Called when gaps execution is ended so it is crash-safe
     to reset the last session Workers info.
  */
  bool mts_finalize_recovery();
  /*
   * End of MTS section ******************************************************/

  /* The general cleanup that slave applier may need at the end of query. */
  inline void cleanup_after_query() {
    if (deferred_events) deferred_events->rewind();
  }
  /* The general cleanup that slave applier may need at the end of session. */
  void cleanup_after_session() {
    if (deferred_events) delete deferred_events;
  }

  /**
    Helper function to do after statement completion.

    This function is called from an event to complete the group by
    either stepping the group position, if the "statement" is not
    inside a transaction; or increase the event position, if the
    "statement" is inside a transaction.

    @param event_log_pos
    Master log position of the event. The position is recorded in the
    relay log info and used to produce information for <code>SHOW
    SLAVE STATUS</code>.
  */
  int stmt_done(my_off_t event_log_pos);

  /**
     Set the value of a replication state flag.

     @param flag Flag to set
   */
  void set_flag(enum_state_flag flag) { m_flags |= (1UL << flag); }

  /**
     Get the value of a replication state flag.

     @param flag Flag to get value of

     @return @c true if the flag was set, @c false otherwise.
   */
  bool get_flag(enum_state_flag flag) { return m_flags & (1UL << flag); }

  /**
     Clear the value of a replication state flag.

     @param flag Flag to clear
   */
  void clear_flag(enum_state_flag flag) { m_flags &= ~(1UL << flag); }

 private:
  /**
    Auxiliary function used by is_in_group.

    The execute thread is in the middle of a statement in the
    following cases:
    - User_var/Intvar/Rand events have been processed, but the
      corresponding Query_log_event has not been processed.
    - Table_map or Row events have been processed, and the last Row
      event did not have the STMT_END_F set.

    @retval true Replication thread is inside a statement.
    @retval false Replication thread is not inside a statement.
   */
  bool is_in_stmt() const {
    bool ret = (m_flags & (1UL << IN_STMT));
    DBUG_PRINT("info", ("is_in_stmt()=%d", ret));
    return ret;
  }
  /**
    Auxiliary function used by is_in_group.

    @retval true The execute thread is inside a statement or a
    transaction, i.e., either a BEGIN has been executed or we are in
    the middle of a statement.
    @retval false The execute thread thread is not inside a statement
    or a transaction.
  */
  bool is_in_trx_or_stmt() const {
    bool ret = is_in_stmt() || (info_thd->variables.option_bits & OPTION_BEGIN);
    DBUG_PRINT("info", ("is_in_trx_or_stmt()=%d", ret));
    return ret;
  }

 public:
  /**
    A group is defined as the entire range of events that constitute
    a transaction or auto-committed statement. It has one of the
    following forms:

    (Gtid)? Query(BEGIN) ... (Query(COMMIT) | Query(ROLLBACK) | Xid)
    (Gtid)? (Rand | User_var | Int_var)* Query(DDL)

    Thus, to check if the execute thread is in a group, there are
    two cases:

    - If the master generates Gtid events (5.7.5 or later, or 5.6 or
      later with GTID_MODE=ON), then is_in_group is the same as
      info_thd->owned_gtid.sidno != 0, since owned_gtid.sidno is set
      to non-zero by the Gtid_log_event and cleared to zero at commit
      or rollback.

    - If the master does not generate Gtid events (i.e., master is
      pre-5.6, or pre-5.7.5 with GTID_MODE=OFF), then is_in_group is
      the same as is_in_trx_or_stmt().

    @retval true Replication thread is inside a group.
    @retval false Replication thread is not inside a group.
  */
  bool is_in_group() const {
    bool ret = is_in_trx_or_stmt() || info_thd->owned_gtid.sidno != 0;
    DBUG_PRINT("info", ("is_in_group()=%d", ret));
    return ret;
  }

  int count_relay_log_space();

  /**
    Initialize the relay log info. This function does a set of operations
    on the rli object like initializing variables, loading information from
    repository, setting up name for relay log files and index, MTS recovery
    (if necessary), calculating the received GTID set for the channel and
    storing the updated rli object configuration into the repository.

    When this function is called in a change replication source process and the
    change procedure will purge all the relay log files later, there is no
    reason to try to calculate the received GTID set of the channel based on
    existing relay log files (they will be purged). Allowing reads to existing
    relay log files at this point may lead to put the server in a state where
    it will be no possible to configure it if it was reset when encryption of
    replication log files was ON and the keyring plugin is not available
    anymore.

    @param skip_received_gtid_set_and_relaylog_recovery When true, skips the
    received GTID set and relay log recovery.

    @retval 0 Success.
    @retval 1 Error.
  */
  int rli_init_info(bool skip_received_gtid_set_and_relaylog_recovery = false);
  void end_info();

  /** No flush options given to relay log flush */
  static constexpr int RLI_FLUSH_NO_OPTION{0};
  /** Ignore server sync options and flush */
  static constexpr int RLI_FLUSH_IGNORE_SYNC_OPT{1 << 0};
  /** Flush disresgarding the value of GTID_ONLY */
  static constexpr int RLI_FLUSH_IGNORE_GTID_ONLY{1 << 1};

  int flush_info(const int flush_flags);
  /**
   Clears from `this` Relay_log_info object all attribute values that are
   not to be kept.

   @returns true if there were a problem with clearing the data and false
            otherwise.
   */
  bool clear_info();
  /**
   Checks if the underlying `Rpl_info` handler holds information for the fields
   to be kept between slave resets, while the other fields were cleared.

   @param previous_result the result return from invoking the `check_info`
                          method on `this` object.

   @returns function success state represented by the `enum_return_check`
            enumeration.
   */
  enum_return_check check_if_info_was_cleared(
      const enum_return_check &previous_result) const;
  int flush_current_log();
  void set_master_info(Master_info *info);

  inline ulonglong get_future_event_relay_log_pos() {
    return future_event_relay_log_pos;
  }
  inline void set_future_event_relay_log_pos(ulonglong log_pos) {
    future_event_relay_log_pos = log_pos;
  }

  inline const char *get_group_master_log_name() const {
    return group_master_log_name;
  }
  inline const char *get_group_master_log_name_info() const {
    if (m_is_applier_source_position_info_invalid) return "INVALID";
    return get_group_master_log_name();
  }
  inline ulonglong get_group_master_log_pos() const {
    return group_master_log_pos;
  }
  inline ulonglong get_group_master_log_pos_info() const {
    if (m_is_applier_source_position_info_invalid) return 0;
    return get_group_master_log_pos();
  }
  inline void set_group_master_log_name(const char *log_file_name) {
    strmake(group_master_log_name, log_file_name,
            sizeof(group_master_log_name) - 1);
  }
  inline void set_group_master_log_pos(ulonglong log_pos) {
    group_master_log_pos = log_pos;
    // Whenever the position is set, it means it is no longer invalid
    m_is_applier_source_position_info_invalid = false;
  }

  inline const char *get_group_relay_log_name() { return group_relay_log_name; }
  inline ulonglong get_group_relay_log_pos() { return group_relay_log_pos; }
  inline void set_group_relay_log_name(const char *log_file_name) {
    strmake(group_relay_log_name, log_file_name,
            sizeof(group_relay_log_name) - 1);
  }
  inline void set_group_relay_log_name(const char *log_file_name, size_t len) {
    strmake(group_relay_log_name, log_file_name, len);
  }
  inline void set_group_relay_log_pos(ulonglong log_pos) {
    group_relay_log_pos = log_pos;
  }

  inline const char *get_event_relay_log_name() { return event_relay_log_name; }
  inline ulonglong get_event_relay_log_pos() { return event_relay_log_pos; }
  inline void set_event_relay_log_name(const char *log_file_name) {
    strmake(event_relay_log_name, log_file_name,
            sizeof(event_relay_log_name) - 1);
    notify_relay_log_change();
  }

  void set_event_start_pos(my_off_t pos) { event_start_pos = pos; }
  my_off_t get_event_start_pos() { return event_start_pos; }

  inline void set_event_relay_log_pos(ulonglong log_pos) {
    event_relay_log_pos = log_pos;
  }
  inline const char *get_rpl_log_name() const {
    return m_is_applier_source_position_info_invalid
               ? "INVALID"
               : (group_master_log_name[0] ? group_master_log_name : "FIRST");
  }

  static size_t get_number_info_rli_fields();

  /**
     Sets bits for columns that are allowed to be `NULL`.

     @param nullable_fields the bitmap to hold the nullable fields.
  */
  static void set_nullable_fields(MY_BITMAP *nullable_fields);

  /**
    Indicate that a delay starts.

    This does not actually sleep; it only sets the state of this
    Relay_log_info object to delaying so that the correct state can be
    reported by SHOW REPLICA STATUS and SHOW PROCESSLIST.

    Requires rli->data_lock.

    @param delay_end The time when the delay shall end.
  */
  void start_sql_delay(time_t delay_end);

  /* Note that this is cast to uint32 in show_slave_status(). */
  time_t get_sql_delay() { return sql_delay; }
  void set_sql_delay(time_t _sql_delay) { sql_delay = _sql_delay; }
  time_t get_sql_delay_end() { return sql_delay_end; }

  Relay_log_info(bool is_slave_recovery,
#ifdef HAVE_PSI_INTERFACE
                 PSI_mutex_key *param_key_info_run_lock,
                 PSI_mutex_key *param_key_info_data_lock,
                 PSI_mutex_key *param_key_info_sleep_lock,
                 PSI_mutex_key *param_key_info_thd_lock,
                 PSI_mutex_key *param_key_info_data_cond,
                 PSI_mutex_key *param_key_info_start_cond,
                 PSI_mutex_key *param_key_info_stop_cond,
                 PSI_mutex_key *param_key_info_sleep_cond,
#endif
                 uint param_id, const char *param_channel, bool is_rli_fake);
  ~Relay_log_info() override;

  /*
    Determines if a warning message on unsafe execution was
    already printed out to avoid clutering the error log
    with several warning messages.
  */
  bool reported_unsafe_warning;

  /*
    'sql_thread_kill_accepted is set to true when killed status is recognized.
  */
  bool sql_thread_kill_accepted;

  time_t get_row_stmt_start_timestamp() { return row_stmt_start_timestamp; }

  time_t set_row_stmt_start_timestamp() {
    if (row_stmt_start_timestamp == 0) row_stmt_start_timestamp = time(nullptr);

    return row_stmt_start_timestamp;
  }

  void reset_row_stmt_start_timestamp() { row_stmt_start_timestamp = 0; }

  void set_long_find_row_note_printed() { long_find_row_note_printed = true; }

  void unset_long_find_row_note_printed() {
    long_find_row_note_printed = false;
  }

  bool is_long_find_row_note_printed() { return long_find_row_note_printed; }

 public:
  /**
    Delete the existing event and set a new one.  This class is
    responsible for freeing the event, the caller should not do that.

    @return 1 if an error was encountered, 0 otherwise.
  */
  virtual int set_rli_description_event(Format_description_log_event *fdle);

  /**
    Return the current Format_description_log_event.
  */
  Format_description_log_event *get_rli_description_event() const {
    return rli_description_event;
  }

  /**
    adaptation for the slave applier to specific master versions.
  */
  ulong adapt_to_master_version(Format_description_log_event *fdle);
  ulong adapt_to_master_version_updown(ulong master_version,
                                       ulong current_version);
  uchar slave_version_split[3];  // bytes of the slave server version
  /*
    relay log info repository should be updated on relay log
    rotate. But when the transaction is split across two relay logs,
    update the repository will cause unexpected results and should
    be postponed till the 'commit' of the transaction is executed.

    A flag that set to 'true' when this type of 'forced flush'(at the
    time of rotate relay log) is postponed due to transaction split
    across the relay logs.
  */
  bool force_flush_postponed_due_to_split_trans;

  Commit_order_manager *get_commit_order_manager() { return commit_order_mngr; }

  void set_commit_order_manager(Commit_order_manager *mngr) {
    commit_order_mngr = mngr;
  }

  /*
    Following set function is required to initialize the 'until_option' during
    MTS relay log recovery process.

    Ideally initialization of 'until_option' is done through
    rli::init_until_option. This init_until_option requires the main server
    thread object and it makes use of the thd->lex->mi object to initialize the
    'until_option'.

    But MTS relay log recovery process happens before the main server comes
    up at this time the THD object will not be available. Hence the following
    set function does the initialization of 'until_option'.
  */
  void set_until_option(Until_option *option) {
    mysql_mutex_lock(&data_lock);
    until_option = option;
    mysql_mutex_unlock(&data_lock);
  }

  void clear_until_option() {
    mysql_mutex_lock(&data_lock);
    if (until_option) {
      delete until_option;
      until_option = nullptr;
    }
    mysql_mutex_unlock(&data_lock);
  }

  bool set_info_search_keys(Rpl_info_handler *to) override;

  /**
    Get coordinator's RLI. Especially used get the rli from
    a slave thread, like this: thd->rli_slave->get_c_rli();
    thd could be a SQL thread or a worker thread
  */
  virtual Relay_log_info *get_c_rli() { return this; }

  const char *get_for_channel_str(bool upper_case = false) const override;

  /**
    Set replication filter for the channel.
  */
  inline void set_filter(Rpl_filter *channel_filter) {
    rpl_filter = channel_filter;
  }

 protected:
  Format_description_log_event *rli_description_event;

 private:
  /*
    Commit order manager to order commits made by its workers. In context of
    Multi Source Replication each worker will be ordered by the corresponding
    corrdinator's order manager.
   */
  Commit_order_manager *commit_order_mngr;

  /**
    Delay slave SQL thread by this amount of seconds.
    The delay is applied per transaction and based on the immediate master's
    commit time. Exceptionally, if a server in the replication chain does not
    support the commit timestamps in Gtid_log_event, the delay is applied per
    event and is based on the event timestamp.
    This is set with CHANGE REPLICATION SOURCE TO SOURCE_DELAY=X.

    Guarded by data_lock.  Initialized by the client thread executing
    START REPLICA.  Written by client threads executing CHANGE REPLICATION
    SOURCE TO SOURCE_DELAY=X.  Read by SQL thread and by client threads
    executing SHOW REPLICA STATUS.  Note: must not be written while the
    slave SQL thread is running, since the SQL thread reads it without
    a lock when executing flush_info().
  */
  time_t sql_delay;

  /**
    During a delay, specifies the point in time when the delay ends.

    This is used for the SQL_Remaining_Delay column in SHOW REPLICA STATUS.

    Guarded by data_lock. Written by the sql thread.  Read by client
    threads executing SHOW REPLICA STATUS.
  */
  time_t sql_delay_end;

  uint32 m_flags;

  /*
    Historically, the number of entires in applier metadata was the number
    of lines in applier metadata file. Since WL#13959, applier metadata can
    be stored only in table, but the notion of number of line is still
    preserved.
    Before the SOURCE_DELAY parameter was added (WL#344), applier metadata
    had 4 lines. Now it has 5 lines.
  */
  static const int APPLIER_METADATA_LINES_WITH_DELAY = 5;

  /*
    Before the WL#5599, applier metadata had 5 lines. Now it has 6 lines.
  */
  static const int APPLIER_METADATA_LINES_WITH_WORKERS = 6;

  /*
    Before the Id was added (BUG#2334346), applier metadata
    had 6 lines. Now it has 7 lines.
  */
  static const int APPLIER_METADATA_LINES_WITH_ID = 7;

  /*
    Add a channel in the applier metadata
  */
  static const int APPLIER_METADATA_LINES_WITH_CHANNEL = 8;

  /*
    Represents entry id in applier metadata to save
    PRIVILEGE_CHECKS_USERNAME. It is username part of PRIVILEGES_CHECKS_USER
    column in performance_schema.replication_applier_configuration.
  */
  static const int APPLIER_METADATA_LINES_WITH_PRIV_CHECKS_USERNAME = 9;

  /*
    Maximum length of PRIVILEGE_CHECKS_USERNAME.
  */
  static const int PRIV_CHECKS_USERNAME_LENGTH = 32;

  /*
    Represents entry id in applier metadata to save
    PRIVILEGE_CHECKS_HOSTNAME. It is hostname part of PRIVILEGES_CHECKS_USER
    column in performance_schema.replication_applier_configuration.
  */
  static const int APPLIER_METADATA_LINES_WITH_PRIV_CHECKS_HOSTNAME = 10;

  /*
    Maximum length of PRIVILEGE_CHECKS_USERNAME.
  */
  static const int PRIV_CHECKS_HOSTNAME_LENGTH = 255;

  /*
    Represents entry id in applier metadata to save REQUIRE_ROW_FORMAT
  */
  static const int APPLIER_METADATA_LINES_WITH_REQUIRE_ROW_FORMAT = 11;

  /*
    Represents entry id in applier metadata to save
    REQUIRE_TABLE_PRIMARY_KEY_CHECK
  */
  static const int APPLIER_METADATA_LINES_WITH_REQUIRE_TABLE_PRIMARY_KEY_CHECK =
      12;

  /*
    Represent entry id in applier metadata to save
    ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS_TYPE.
  */
  static const int
      APPLIER_METADATA_LINES_WITH_ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS_TYPE =
          13;

  /*
    Represent entry id in applier metadata to save
    ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS_VALUE.
  */
  static const int
      APPLIER_METADATA_LINES_WITH_ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS_VALUE =
          14;
  /*
    Total lines in applier metadata.
    This has to be updated every time a member is added or removed.
    Historically, the number of entires in applier metadata was the number
    of lines in applier metadata file. Since WL#13959, applier metadata can
    be stored only in table, but the notion of number of line is still
    preserved.
  */
  static const int MAXIMUM_APPLIER_METADATA_LINES =
      APPLIER_METADATA_LINES_WITH_ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS_VALUE;

  bool read_info(Rpl_info_handler *from) override;
  bool write_info(Rpl_info_handler *to) override;

  Relay_log_info(const Relay_log_info &info);
  Relay_log_info &operator=(const Relay_log_info &info);

  /*
    Runtime state for printing a note when slave is taking
    too long while processing a row event.
   */
  time_t row_stmt_start_timestamp;
  bool long_find_row_note_printed;

  /**
    sets the suffix required for relay log names in multisource
    replication. When --relay-log option is not provided, the
    names of the relay log files are relaylog.0000x or
    relaylog-CHANNEL.00000x in the case of MSR. However, if
    that option is provided, then the names of the relay log
    files are <relay-log-option>.0000x or
    <relay-log-option>-CHANNEL.00000x in the case of MSR.

    The function adds a channel suffix (according to the channel to
    file name conventions and conversions) to the relay log file.

    @todo: truncate the log file if length exceeds.

    @param[in, out]  buff       buffer to store the complete relay log file name
    @param[in]       buff_size  size of buffer buff
    @param[in]       base_name  the base name of the relay log file
  */
  const char *add_channel_to_relay_log_name(char *buff, uint buff_size,
                                            const char *base_name);

  /*
    Applier thread InnoDB priority.
    When two transactions conflict inside InnoDB, the one with
    greater priority wins.
    Priority must be set before applier thread start so that all
    executed transactions have the same priority.
  */
  int thd_tx_priority;

  /**
    If the SQL thread should or not ignore the set limit for
    write set collection
   */
  bool m_ignore_write_set_memory_limit;

  /**
    Even if a component says all transactions require write sets,
    this variable says the SQL thread transactions can drop them
  */
  bool m_allow_drop_write_set;

  /* The object stores and handles START REPLICA UNTIL option */
  Until_option *until_option;

 public:
  /*
    The boolean is set to true when the binlog (rli_fake) or slave
    (rli_slave) applier thread detaches any engine ha_data
    it has dealt with at time of XA START processing.
    The boolean is reset to false at the end of XA PREPARE,
    XA COMMIT ONE PHASE for the binlog applier, and
    at internal rollback of the slave applier at the same time with
    the engine ha_data re-attachment.
  */
  bool m_is_engine_ha_data_detached;
  /**
    Reference to being applied event. The member is set at event reading
    and gets reset at the end of the event lifetime.
    See more in @c RLI_current_event_raii that provides the main
    interface to the member.
  */
  Log_event *current_event;

  /**
    Raised when slave applies and writes to its binary log statement
    which is not atomic DDL and has no XID assigned. Checked at commit
    time to decide whether it is safe to update slave info table
    within the same transaction as the write to binary log or this
    should be deferred. The deferred scenario applies for not XIDed events
    in which case such update might be lost on recovery.
  */
  bool ddl_not_atomic;

  void set_thd_tx_priority(int priority) { thd_tx_priority = priority; }

  int get_thd_tx_priority() { return thd_tx_priority; }

  void set_ignore_write_set_memory_limit(bool ignore_limit) {
    m_ignore_write_set_memory_limit = ignore_limit;
  }

  bool get_ignore_write_set_memory_limit() {
    return m_ignore_write_set_memory_limit;
  }

  void set_allow_drop_write_set(bool does_not_require_ws) {
    m_allow_drop_write_set = does_not_require_ws;
  }

  bool get_allow_drop_write_set() { return m_allow_drop_write_set; }

  const char *get_until_log_name();
  my_off_t get_until_log_pos();
  bool is_until_satisfied_at_start_slave() {
    return until_option != nullptr &&
           until_option->is_satisfied_at_start_slave();
  }
  bool is_until_satisfied_before_dispatching_event(const Log_event *ev) {
    return until_option != nullptr &&
           until_option->is_satisfied_before_dispatching_event(ev);
  }
  bool is_until_satisfied_after_dispatching_event() {
    return until_option != nullptr &&
           until_option->is_satisfied_after_dispatching_event();
  }
  bool is_until_satisfied_all_transactions_read_from_relay_log() {
    return until_option != nullptr &&
           until_option->is_satisfied_all_transactions_read_from_relay_log();
  }
  /**
   Initialize until option object when starting slave.

   @param[in] thd The thread object of current session.
   @param[in] master_param the parameters of START REPLICA.

   @return int
     @retval 0      Succeeds to initialize until option object.
     @retval <> 0   A defined error number is return if any error happens.
   */
  int init_until_option(THD *thd, const LEX_SOURCE_INFO *master_param);

  /**
    Detaches the engine ha_data from THD. The fact
    is memorized in @c m_is_engine_ha_data_detached flag.

    @param  thd a reference to THD
  */

  void detach_engine_ha_data(THD *thd);

  /**
    Reattaches the engine ha_data to THD. The fact
    is memorized in @c m_is_engine_ha_data_detached flag.

    @param  thd a reference to THD
  */

  void reattach_engine_ha_data(THD *thd);

  /**
    Checks whether engine ha data is detached from THD
    @retval true if the data is detached
    @retval false if the data is not detached
  */
  bool is_engine_ha_data_detached() { return m_is_engine_ha_data_detached; }

  /**
    Execute actions at replicated atomic DLL post rollback time.
    This include marking the current atomic DDL query-log-event
    as having processed.
    This measure is necessary to avoid slave info table update execution
    when @c pre_commit() hook is called as part of DDL's eventual
    implicit commit.
  */
  void post_rollback() {
    static_cast<Query_log_event *>(current_event)->has_ddl_committed = true;
  }

  /**
    The method implements a pre-commit hook to add up a new statement
    typically to a DDL transaction to update the slave info table.
    Note, in the non-transactional repository case the slave info
    is updated after successful commit of the main transaction.

    @return false as success, otherwise true
  */
  bool pre_commit() {
    bool rc = false;

    if (is_transactional()) {
      static_cast<Query_log_event *>(current_event)->has_ddl_committed = true;
      rc = commit_positions();
    }
    return rc;
  }
  /**
    Cleanup of any side effect that pre_commit() inflicts, including
    restore of the last executed group coordinates in case the current group
    has been destined to rollback, and signaling to possible waiters
    in the positive case.

    @param on_rollback  when true the method carries out rollback action
  */
  virtual void post_commit(bool on_rollback);
};

/**
  Negation operator for `enum_priv_checks_status`, to facilitate validation
  against `SUCCESS`. To test for error status, use the `!!` idiom.

  @param status the status code to check against `SUCCESS`

  @return true if the status is `SUCCESS` and false otherwise.
 */
bool operator!(Relay_log_info::enum_priv_checks_status status);

/**
  Negation operator for `enum_require_row_status`, to facilitate validation
  against `SUCCESS`. To test for error status, use the `!!` idiom.

  @param status the status code to check against `SUCCESS`

  @return true if the status is `SUCCESS` and false otherwise.
 */
bool operator!(Relay_log_info::enum_require_row_status status);

bool mysql_show_relaylog_events(THD *thd);

/**
   @param  thd a reference to THD
   @return true if thd belongs to a Worker thread and false otherwise.
*/
inline bool is_mts_worker(const THD *thd) {
  return thd->system_thread == SYSTEM_THREAD_SLAVE_WORKER;
}

/**
 Auxiliary function to check if we have a db partitioned MTS
 */
bool is_mts_db_partitioned(Relay_log_info *rli);

/**
  Checks whether the supplied event encodes a (2pc-aware) DDL
  that has been already committed.

  @param  ev    A reference to Query-log-event
  @return true  when the event is already committed transactional DDL
*/
inline bool is_committed_ddl(Log_event *ev) {
  return ev->get_type_code() == mysql::binlog::event::QUERY_EVENT &&
         /* has been already committed */
         static_cast<Query_log_event *>(ev)->has_ddl_committed;
}

/**
  Checks whether the transaction identified by the argument
  is executed by a slave applier thread is an atomic DDL
  not yet committed (see @c Query_log_event::has_ddl_committed).
  THD::is_operating_substatement_implicitly filters out intermediate
  commits done by non-atomic DDLs.
  The error-tagged atomic statements are regarded as non-atomic
  therefore this predicate returns negative in such case.

  Note that call to is_atomic_ddl() returns "approximate" outcome in
  this case as it misses information about type of tables used by the DDL.

  This can be a problem for binlogging slave, as updates to slave info
  which happen in the same transaction as write of binary log event
  without XID might be lost on recovery. To avoid this problem
  RLI::ddl_not_atomic flag is employed which is set to true when
  non-atomic DDL without XID is written to the binary log.

  "Approximate" outcome is always fine for non-binlogging slave as in
  this case commit happens using one-phase routine for which recovery
  is always correct.

  @param  thd   a pointer to THD describing the transaction context
  @return true  when a slave applier thread is set to commit being processed
                DDL query-log-event, otherwise returns false.
*/
inline bool is_atomic_ddl_commit_on_slave(THD *thd) {
  assert(thd);

  Relay_log_info *rli = thd->rli_slave;

  /* Early return is about an error in the SQL thread initialization */
  if (!rli) return false;

  return ((thd->system_thread == SYSTEM_THREAD_SLAVE_SQL ||
           thd->system_thread == SYSTEM_THREAD_SLAVE_WORKER) &&
          rli->current_event)
             ? (rli->is_transactional() &&
                /* has not yet committed */
                (rli->current_event->get_type_code() ==
                     mysql::binlog::event::QUERY_EVENT &&
                 !static_cast<Query_log_event *>(rli->current_event)
                      ->has_ddl_committed) &&
                /* unless slave binlogger identified non-atomic */
                !rli->ddl_not_atomic &&
                /* slave info is not updated when a part of multi-DROP-TABLE
                   commits */
                !thd->is_commit_in_middle_of_statement &&
                (is_atomic_ddl(thd, true) &&
                 !thd->is_operating_substatement_implicitly) &&
                /* error-tagged atomic DDL do not update yet slave info */
                static_cast<Query_log_event *>(rli->current_event)
                        ->error_code == 0)
             : false;
}

/**
  RAII class to control the slave applier execution context binding
  with a being handled event. The main object of control is Query-log-event
  containing DDL statement.
  The member RLI::current_event is set to refer to an event once it is
  read, e.g by next_event() and is reset to NULL at exiting a
  read-exec loop. Once the event is destroyed RLI::current_event must be reset
  or guaranteed not be accessed anymore.
  In the MTS execution the worker is reliably associated with an event
  only with the latter is not deferred. This includes Query-log-event.
*/
class RLI_current_event_raii {
  Relay_log_info *m_rli;

 public:
  RLI_current_event_raii(Relay_log_info *rli_arg, Log_event *ev)
      : m_rli(rli_arg) {
    m_rli->current_event = ev;
  }
  void set_current_event(Log_event *ev) { m_rli->current_event = ev; }
  ~RLI_current_event_raii() { m_rli->current_event = nullptr; }
};

/**
 @class MDL_lock_guard

 Utility class to allow RAII pattern with `MDL_request` and `MDL_context`
 classes.
 */
class MDL_lock_guard {
 public:
  /**
   Constructor that initializes the object and the target `THD` object but
   doesn't try to acquire any lock.

   @param target THD object, source for the `MDL_context` to use.
   */
  MDL_lock_guard(THD *target);
  /**
   Constructor that initializes the object and the target `THD` object and tries
   to acquire the lock identified by `namespace_arg` with MDL type identified by
   `mdl_type_arg`.

   If the `blocking` parameter is true, it will instantly try to acquire the
   lock and block. If the `blocking` parameter is false, it will first test if
   the lock is already acquired and only try to lock if no conflicting lock is
   already acquired.

   @param target THD object, source for the `MDL_context` to use.
   @param namespace_arg MDL key namespace to acquire the lock from.
   @param mdl_type_arg MDL acquisition type
   @param blocking whether or not the execution should block if the lock is
                   already acquired.
   */
  MDL_lock_guard(THD *target, MDL_key::enum_mdl_namespace namespace_arg,
                 enum_mdl_type mdl_type_arg, bool blocking = false);
  /**
   Destructor that unlocks all acquired locks.
   */
  virtual ~MDL_lock_guard();

  /**
   Uses the target `THD` object MDL context to acquire the lock identified by
   `namespace_arg` with MDL type identified by `mdl_type_arg`.

   If the `blocking` parameter is true, it will instantly try to acquire the
   lock and block. If the `blocking` parameter is false, it will first test if
   the lock is already acquired and only try to lock if no conflicting lock is
   already acquired.

   The lock is determined to have been acquired if the `THD` object MDL context
   hasn't already a lock and the lock is acquired. In other words, if the MDL
   context already has acquired the lock, the method will return failure.

   @param namespace_arg MDL key namespace to acquire the lock from.
   @param mdl_type_arg MDL acquisition type
   @param blocking whether or not the execution should block if the lock is
                   already acquired.

   @return false if the lock has been acquired by this method invocation and
           true if not.
   */
  bool lock(MDL_key::enum_mdl_namespace namespace_arg,
            enum_mdl_type mdl_type_arg, bool blocking = false);
  /**
   Returns whether or not the lock as been acquired within this object
   life-cycle.

   @return true if the lock has been acquired within this object life-cycle.
   */
  bool is_locked();

 private:
  /** The `THD` object holding the MDL context used for acquiring/releasing. */
  THD *m_target;
  /** The MDL request holding the MDL ticket issued upon acquisition */
  MDL_request m_request;
};

/**
  @class Applier_security_context_guard

  Utility class to allow RAII pattern with `Security_context` class.

  At initiliazation, if the `THD` main security context isn't already the
  appropriate one, it copies the `Relay_log_info::info_thd::security_context`
  and replaces it with the one initialized with the `PRIVILEGE_CHECK_USER` user.
  At deinitialization, it copies the backed up security context.

  It also deals with the case where no privilege checks are required, meaning,
  `PRIVILEGE_CHECKS_USER` is `NULL`.

  Usage examples:

  (1)
  @code
      Applier_security_context_guard security_context{rli, thd};
      if (!security_context.has_access({SUPER_ACL})) {
        return ER_NO_ACCESS;
      }
  @endcode

  (4)
  @code
      Applier_security_context_guard security_context{rli, thd};
      if (!security_context.has_access(
              {{CREATE_ACL | INSERT_ACL | UPDATE_ACL, table},
               {SELECT_ACL, table}})) {
        return ER_NO_ACCESS;
      }
  @endcode
 */

class Applier_security_context_guard {
 public:
  /**
    If needed, backs up the current `thd` security context and replaces it with
    a security context for `PRIVILEGE_CHECKS_USER` user.

    @param rli the `Relay_log_info` object that holds the
               `PRIVILEGE_CHECKS_USER` info.
    @param thd the `THD` for which initialize the security context.
   */
  Applier_security_context_guard(Relay_log_info const *rli, THD const *thd);
  /**
    Destructor that restores the backed up security context, if needed.
   */
  virtual ~Applier_security_context_guard();

  // --> Deleted constructors and methods to remove default move/copy semantics
  Applier_security_context_guard(const Applier_security_context_guard &) =
      delete;
  Applier_security_context_guard(Applier_security_context_guard &&) = delete;
  Applier_security_context_guard &operator=(
      const Applier_security_context_guard &) = delete;
  Applier_security_context_guard &operator=(Applier_security_context_guard &&) =
      delete;
  // <--

  /**
    Returns whether or not privilege checks may be skipped within the current
    context.

    @return true if privilege checks may be skipped and false otherwise.
   */
  bool skip_priv_checks() const;
  /**
    Checks if the `PRIVILEGE_CHECKS_USER` user has access to the privilieges
    passed on by `extra_privileges` parameter as well as to the privileges
    passed on at initialization time.

    This particular method checks those privileges against a given table and
    against that table's columns - the ones that are used or changed in the
    event.

    @param extra_privileges set of privileges to check, additionally to those
                            passed on at initialization. It's a list of
                            (privilege, TABLE*, Rows_log_event*) tuples.

    @return true if the privileges are included in the security context and
            false, otherwise.
   */
  bool has_access(
      std::vector<std::tuple<Access_bitmask, TABLE const *, Rows_log_event *>>
          &extra_privileges) const;
  /**
    Checks if the `PRIVILEGE_CHECKS_USER` user has access to the privilieges
    passed on by `extra_privileges` parameter as well as to the privileges
    passed on at initialization time.

    @param extra_privileges set of privileges to check, additionally to those
                            passed on at initialization. It's a list of
                            privileges to be checked against any database.

    @return true if the privileges are included in the security context and
            false, otherwise.
   */
  bool has_access(
      std::initializer_list<std::string_view> extra_privileges) const;

  /**
    Checks if the `PRIVILEGE_CHECKS_USER` user has access to the privilieges
    passed on by `extra_privileges` parameter as well as to the privileges
    passed on at initialization time.

    @param extra_privileges set of privileges to check, additionally to those
                            passed on at initialization. It's a list of
                            privileges to be checked against any database.

    @return true if the privileges are included in the security context and
            false, otherwise.
   */
  bool has_access(std::initializer_list<Access_bitmask> extra_privileges) const;

  /**
    Returns the username for the user for which the security context was
    initialized.

    If `PRIVILEGE_CHECKS_USER` was configured for the target `Relay_log_info`
    object, that one is returned.

    Otherwise, the username associated with the `Security_context` initialized
    for `Relay_log_info::info_thd` will be returned.

    @return an `std::string` holding the username for the active security
            context.
   */
  std::string get_username() const;
  /**
    Returns the hostname for the user for which the security context was
    initialized.

    If `PRIVILEGE_CHECKS_USER` was configured for the target `Relay_log_info`
    object, that one is returned.

    Otherwise, the hostname associated with the `Security_context` initialized
    for `Relay_log_info::info_thd` will be returned.

    @return an `std::string` holding the hostname for the active security
            context.
   */
  std::string get_hostname() const;

 private:
  /**
    The `Relay_log_info` object holding the info required to initialize the
    context.
   */
  Relay_log_info const *m_target;
  /**
    The `THD` object for which the security context will be initialized.
   */
  THD const *m_thd;
  /** Applier security context based on `PRIVILEGE_CHECK_USER` user */
  Security_context m_applier_security_ctx;
  /** Currently in use security context */
  Security_context *m_current;
  /** Backed up security context */
  Security_context *m_previous;
  /** Flag that states if privilege check should be skipped */
  bool m_privilege_checks_none;
  /** Flag that states if there is a logged user */
  bool m_logged_in_acl_user;

  void extract_columns_to_check(TABLE const *table, Rows_log_event *event,
                                std::vector<std::string> &columns) const;
};

#endif /* RPL_RLI_H */
