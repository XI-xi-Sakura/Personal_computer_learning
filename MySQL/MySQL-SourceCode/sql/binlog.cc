/* Copyright (c) 2009, 2025, Oracle and/or its affiliates.

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

#include "sql/binlog.h"

#include "my_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "m_string.h"
#include "map_helpers.h"
#include "my_alloc.h"
#include "my_macros.h"
#include "my_systime.h"
#include "my_thread.h"
#include "mysql/my_loglevel.h"
#include "mysql/strings/int2str.h"
#include "nulls.h"
#include "sql/check_stack.h"
#include "sql/clone_handler.h"
#include "sql/raii/thread_stage_guard.h"
#include "sql_string.h"
#include "strmake.h"
#include "template_utils.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <queue>
#include <sstream>
#include <string>

#include "dur_prop.h"
#include "include/mysqld_errmsg.h"  // ER_OUT_OF_RESOURCES_MSG
#include "mf_wcomp.h"               // wild_one, wild_many
#include "mutex_lock.h"             // Mutex_lock
#include "my_base.h"
#include "my_bitmap.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "my_sqlcommand.h"
#include "my_stacktrace.h"  // my_safe_print_system_time
#include "my_thread_local.h"
#include "mysql/binlog/event/compression/compressor.h"
#include "mysql/binlog/event/compression/payload_event_buffer_istream.h"
#include "mysql/binlog/event/compression/zstd_comp.h"
#include "mysql/binlog/event/control_events.h"
#include "mysql/binlog/event/debug_vars.h"
#include "mysql/binlog/event/rows_event.h"
#include "mysql/binlog/event/statement_events.h"
#include "mysql/binlog/event/table_id.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/containers/buffers/grow_calculator.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/strings/m_ctype.h"
#include "mysql/thread_type.h"
#include "mysqld_error.h"
#include "partition_info.h"
#include "prealloced_array.h"
#include "scope_guard.h"
#include "sql/binlog/decompressing_event_object_istream.h"
#include "sql/binlog/global.h"
#include "sql/binlog/group_commit/bgc_ticket_manager.h"  // Bgc_ticket_manager
#include "sql/binlog/recovery.h"  // binlog::Binlog_recovery
#include "sql/binlog/services/iterator/file_storage.h"
#include "sql/binlog_ostream.h"
#include "sql/binlog_reader.h"
#include "sql/create_field.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"  // DEBUG_SYNC
#include "sql/derror.h"      // ER_THD
#include "sql/discrete_interval.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item_func.h"  // user_var_entry
#include "sql/key.h"
#include "sql/log.h"
#include "sql/log_event.h"           // Rows_log_event
#include "sql/mysqld.h"              // sync_binlog_period ...
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/protocol.h"
#include "sql/psi_memory_key.h"
#include "sql/psi_memory_resource.h"
#include "sql/query_options.h"
#include "sql/raii/sentry.h"  // raii::Sentry<>
#include "sql/rpl_filter.h"
#include "sql/rpl_gtid.h"
#include "sql/rpl_handler.h"  // RUN_HOOK
#include "sql/rpl_mi.h"       // Master_info
#include "sql/rpl_record.h"
#include "sql/rpl_replica.h"
#include "sql/rpl_replica_commit_order_manager.h"  // Commit_order_manager
#include "sql/rpl_rli.h"                           // Relay_log_info
#include "sql/rpl_rli_pdb.h"                       // Slave_worker
#include "sql/rpl_transaction_ctx.h"
#include "sql/rpl_trx_boundary_parser.h"  // Transaction_boundary_parser
#include "sql/rpl_utility.h"
#include "sql/sql_backup_lock.h"  // is_instance_backup_locked et al.
#include "sql/sql_base.h"         // find_temporary_table
#include "sql/sql_bitmap.h"
#include "sql/sql_class.h"  // THD
#include "sql/sql_const.h"
#include "sql/sql_data_change.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_parse.h"  // sqlcom_can_generate_row_events
#include "sql/sql_show.h"   // append_identifier_*
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/transaction_info.h"
#include "sql/xa.h"
#include "sql/xa/sql_cmd_xa.h"  // Sql_cmd_xa_*
#include "sql_partition.h"
#include "string_with_len.h"
#include "thr_lock.h"

class Item;

using mysql::binlog::event::enum_binlog_checksum_alg;
using std::list;
using std::max;
using std::min;
using std::string;

#define FLAGSTR(V, F) ((V) & (F) ? #F " " : "")
#define YESNO(X) ((X) ? "yes" : "no")

/**
  @defgroup Binary_Log Binary Log
  @{
 */

#define MY_OFF_T_UNDEF (~(my_off_t)0UL)

/*
  Constants required for the limit unsafe warnings suppression
 */
// seconds after which the limit unsafe warnings suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT 50
// number of limit unsafe warnings after which the suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT 50

static ulonglong limit_unsafe_suppression_start_time = 0;
static bool unsafe_warning_suppression_is_activated = false;
static int limit_unsafe_warning_count = 0;

static handlerton *binlog_hton;
bool opt_binlog_order_commits = true;

const char *log_bin_index = nullptr;
const char *log_bin_basename = nullptr;

/* Size for IO_CACHE buffer for binlog & relay log */
ulong rpl_read_size;

MYSQL_BIN_LOG mysql_bin_log(&sync_binlog_period);

static int binlog_init(void *p);
static int binlog_start_trans_and_stmt(THD *thd, Log_event *start_event);
static int binlog_close_connection(handlerton *hton, THD *thd);
static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv);
static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv);
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *hton,
                                                      THD *thd);
static int binlog_commit(handlerton *hton, THD *thd, bool all);
static int binlog_rollback(handlerton *hton, THD *thd, bool all);
/*
  This function is used to prepare a transaction. For the binary log SE.

  @param hton The pointer to the binlog SE plugin.
  @param thd The THD session object holding the transaction to be prepared.
  @param all Preparing a transaction (i.e. true) or a statement
             (i.e. false).

  @return 0 if the function is successfully executed, non-zero otherwise
*/
static int binlog_prepare(handlerton *hton, THD *thd, bool all);
/*
  This function is used to mark an X/Open XA distributed transaction as
  being prepared in the server transaction coordinator.

  Is a no-op function, added to the handler API to workaround warnings that
  are triggered for SEs participating in a transaction that requires this
  callback but such callback is not available.

  @param hton The pointer to the binlog SE plugin.
  @param thd The THD session object holding the transaction to be updated.

  @return 0 if the function is successfully executed, non-zero otherwise
*/
static int binlog_set_prepared_in_tc(handlerton *hton, THD *thd);
static void binlog_prepare_row_images(const THD *thd, TABLE *table);
static bool is_loggable_xa_prepare(THD *thd);

namespace {
/**
  Finishes the transaction in the engines. If the `commit_low` flag is set,
  will commit in the engines, otherwise, if the underlying statement is an
  `XA ROLLBACK`, it will rollback in the engines.

  @param thd The THD session object holding the transaction to finalize.
  @param all Finalizing a transaction (i.e. true) or a statement
             (i.e. false).
  @param run_after_commit In the case of a commit being issued, whether or
                          not to run the `after_commit` hook.
 */
void finish_transaction_in_engines(THD *thd, bool all, bool run_after_commit);
}  // namespace

/**
  @brief Checks whether purge conditions are met to be able to run purge
         for binary log files.

  This function checks whether the binary log is open, if the instance
  is not locked for backup.

  @param log The reference to the binary log.
  @return std::pair<bool, int> the first element states whether there is a
  purge condition violation. The second element states what is the associated
  error code, if any.
*/
static std::pair<bool, int> check_purge_conditions(const MYSQL_BIN_LOG &log) {
  // is the binary log open?
  if (!log.is_open()) {
    return std::make_pair(true, 0);
  }

  // go ahead, validations checked successfully
  return std::make_pair(false, 0);
}

/**
  @brief This function abstracts the calculation of the binary log files
         retention lower bound. It is just a function that makes it easier
         to handle the fact that there are two mutually exclusive variables
         that control the purge period and one of them is deprecated.

  NOTE: This function and part of the purge validation functions should
        really move to a retention policy class that abstracts the
        retention policy altogether and its controls. Perhaps we
        can do that once a refactoring is done to also include
        retention based on storage space occupied. Then we can use
        the same retention abstraction for binary and relay logs
        and possibly extend the options to retain (binary) log files
        not only based on time, but also on space used.

  @return time_t the time after which log files are considered expired.
*/
static time_t calculate_auto_purge_lower_time_bound() {
  if (DBUG_EVALUATE_IF("expire_logs_always", true, false)) return time(nullptr);

  int64 expiration_time = 0;
  int64 current_time = time(nullptr);

  if (binlog_expire_logs_seconds > 0)
    expiration_time = current_time - binlog_expire_logs_seconds;

  // check for possible overflow conditions (4 bytes time_t)
  if (expiration_time < std::numeric_limits<time_t>::min())
    expiration_time = std::numeric_limits<time_t>::min();

  // This function should only be called if binlog_expire_logs_seconds
  // is greater than 0
  assert(binlog_expire_logs_seconds > 0);

  return static_cast<time_t>(expiration_time);
}

/**
  @brief Checks if automatic purge conditions are met and therefore the
  purge is allowed to be done. If not met returns true. Otherwise, false.

  @return false if the check is successful. True otherwise.
*/
static bool check_auto_purge_conditions() {
  // purge is disabled
  if (!opt_binlog_expire_logs_auto_purge) return true;

  // no retention window configured
  if (binlog_expire_logs_seconds == 0) return true;

  // go ahead, validations checked successfully
  return false;
}

/**
   Logical binlog file which wraps and hides the detail of lower layer storage
   implementation. Binlog code just use this class to control real storage
 */
class MYSQL_BIN_LOG::Binlog_ofile : public Basic_ostream {
 public:
  ~Binlog_ofile() override {
    DBUG_TRACE;
    close();
    return;
  }

  /**
     Opens the binlog file. It opens the lower layer storage.

     @param[in] log_file_key  The PSI_file_key for this stream
     @param[in] binlog_name  The file to be opened
     @param[in] flags  The flags used by IO_CACHE.
     @param[in] existing True if opening the file, false if creating a new one.

     @retval false  Success
     @retval true  Error
  */
  bool open(
#ifdef HAVE_PSI_INTERFACE
      PSI_file_key log_file_key,
#endif
      const char *binlog_name, myf flags, bool existing = false) {
    DBUG_TRACE;
    assert(m_pipeline_head == nullptr);

#ifndef NDEBUG
    {
#ifndef HAVE_PSI_INTERFACE
      PSI_file_key log_file_key = PSI_NOT_INSTRUMENTED;
#endif
      MY_STAT info;
      if (!mysql_file_stat(log_file_key, binlog_name, &info, MYF(0))) {
        assert(existing == !(my_errno() == ENOENT));
        set_my_errno(0);
      }
    }
#endif

    std::unique_ptr<IO_CACHE_ostream> file_ostream(new IO_CACHE_ostream);
    if (file_ostream->open(log_file_key, binlog_name, flags)) return true;

    m_pipeline_head = std::move(file_ostream);

    /* Setup encryption for new files if needed */
    if (!existing && rpl_encryption.is_enabled()) {
      std::unique_ptr<Binlog_encryption_ostream> encrypted_ostream(
          new Binlog_encryption_ostream());
      if (encrypted_ostream->open(std::move(m_pipeline_head))) return true;
      m_encrypted_header_size = encrypted_ostream->get_header_size();
      m_pipeline_head = std::move(encrypted_ostream);
    }

    return false;
  }

  /**
    Opens an existing binlog file. It opens the lower layer storage reusing the
    existing file password if needed.

    @param[in] log_file_key The PSI_file_key for this stream
    @param[in] binlog_name The file to be opened
    @param[in] flags The flags used by IO_CACHE.

    @retval std::unique_ptr A Binlog_ofile object pointer.
    @retval nullptr Error.
  */
  static std::unique_ptr<Binlog_ofile> open_existing(
#ifdef HAVE_PSI_INTERFACE
      PSI_file_key log_file_key,
#endif
      const char *binlog_name, myf flags) {
    DBUG_TRACE;
    std::unique_ptr<Rpl_encryption_header> header;
    unsigned char magic[BINLOG_MAGIC_SIZE];

    /* Open a simple istream to read the magic from the file */
    IO_CACHE_istream istream;
    if (istream.open(key_file_binlog, key_file_binlog_cache, binlog_name,
                     MYF(MY_WME | MY_DONT_CHECK_FILESIZE), rpl_read_size))
      return nullptr;
    if (istream.read(magic, BINLOG_MAGIC_SIZE) != BINLOG_MAGIC_SIZE)
      return nullptr;

    assert(Rpl_encryption_header::ENCRYPTION_MAGIC_SIZE == BINLOG_MAGIC_SIZE);
    /* Identify the file type by the magic to get the encryption header */
    if (memcmp(magic, Rpl_encryption_header::ENCRYPTION_MAGIC,
               BINLOG_MAGIC_SIZE) == 0) {
      header = Rpl_encryption_header::get_header(&istream);
      if (header == nullptr) return nullptr;
    } else if (memcmp(magic, BINLOG_MAGIC, BINLOG_MAGIC_SIZE) != 0) {
      return nullptr;
    }

    /* Open the binlog_ofile */
    std::unique_ptr<Binlog_ofile> ret_ofile(new Binlog_ofile);
    if (ret_ofile->open(
#ifdef HAVE_PSI_INTERFACE
            log_file_key,
#endif
            binlog_name, flags, true)) {
      return nullptr;
    }

    if (header != nullptr) {
      /* Add the encryption stream on top of IO_CACHE */
      std::unique_ptr<Binlog_encryption_ostream> encrypted_ostream(
          new Binlog_encryption_ostream);
      ret_ofile->m_encrypted_header_size = header->get_header_size();
      encrypted_ostream->open(std::move(ret_ofile->m_pipeline_head),
                              std::move(header));
      ret_ofile->m_pipeline_head = std::move(encrypted_ostream);
      ret_ofile->set_encrypted();
    }
    return ret_ofile;
  }

  void close() {
    m_pipeline_head.reset(nullptr);
    m_position = 0;
    m_encrypted_header_size = 0;
  }

  /**
     Writes data into storage and maintains binlog position.

     @param[in] buffer  the data will be written
     @param[in] length  the length of the data

     @retval false  Success
     @retval true  Error
  */
  bool write(const unsigned char *buffer, my_off_t length) override {
    assert(m_pipeline_head != nullptr);

    if (m_pipeline_head->write(buffer, length)) return true;

    m_position += length;
    return false;
  }

  /**
     Updates some bytes in the binlog file. If is only used for clearing
     LOG_EVENT_BINLOG_IN_USE_F.

     @param[in] buffer  the data will be written
     @param[in] length  the length of the data
     @param[in] offset  the offset of the bytes will be updated

     @retval false  Success
     @retval true  Error
  */
  bool update(const unsigned char *buffer, my_off_t length, my_off_t offset) {
    assert(m_pipeline_head != nullptr);
    return m_pipeline_head->seek(offset) ||
           m_pipeline_head->write(buffer, length);
  }

  /**
     Truncates some data at the end of the binlog file.

     @param[in] offset  where the binlog file will be truncated to.

     @retval false  Success
     @retval true  Error
  */
  bool truncate(my_off_t offset) {
    assert(m_pipeline_head != nullptr);

    if (m_pipeline_head->truncate(offset)) return true;
    m_position = offset;
    return false;
  }

  bool flush() { return m_pipeline_head->flush(); }
  bool sync() { return m_pipeline_head->sync(); }
  bool flush_and_sync() { return flush() || sync(); }
  my_off_t position() { return m_position; }
  bool is_empty() { return position() == 0; }
  bool is_open() { return m_pipeline_head != nullptr; }
  /**
    Returns the encrypted header size of the binary log file.

    @retval 0 The file is not encrypted.
    @retval >0 The encryption header size.
  */
  int get_encrypted_header_size() { return m_encrypted_header_size; }
  /**
    Returns the real file size.

    While position() returns the "file size" from the plain binary log events
    stream point of view, this function considers the encryption header when it
    exists.

    @return The real file size considering the encryption header.
  */
  my_off_t get_real_file_size() { return m_position + m_encrypted_header_size; }
  /**
    Get the pipeline head.

    @retval  Returns the pipeline head or nullptr.
  */
  std::unique_ptr<Truncatable_ostream> get_pipeline_head() {
    return std::move(m_pipeline_head);
  }
  /**
    Check if the log file is encrypted.

    @retval  True if the log file is encrypted.
    @retval  False if the log file is not encrypted.
  */
  bool is_encrypted() { return m_encrypted; }
  /**
    Set that the log file is encrypted.
  */
  void set_encrypted() { m_encrypted = true; }

 private:
  my_off_t m_position = 0;
  int m_encrypted_header_size = 0;
  std::unique_ptr<Truncatable_ostream> m_pipeline_head;
  bool m_encrypted = false;
};

/**
  Helper class to switch to a new thread and then go back to the previous one,
  when the object is destroyed using RAII.

  This class is used to temporarily switch to another session (THD
  structure). It will set up thread specific "globals" correctly
  so that the POSIX thread looks exactly like the session attached to.
  However, PSI_thread info is not touched as it is required to show
  the actual physical view in PFS instrumentation i.e., it should
  depict as the real thread doing the work instead of thread it switched
  to.

  On destruction, the original session (which is supplied to the
  constructor) will be re-attached automatically. For example, with
  this code, the value of @c current_thd will be the same before and
  after execution of the code.

  @code
  {
    for (int i = 0 ; i < count ; ++i)
    {
      // here we are attached to current_thd
      // [...]
      Thd_backup_and_restore switch_thd(current_thd, other_thd[i]);
      // [...]
      // here we are attached to other_thd[i]
      // [...]
    }
    // here we are attached to current_thd
  }
  @endcode

  @warning The class is not designed to be inherited from.
 */

class Thd_backup_and_restore {
 public:
  /**
    Try to attach the POSIX thread to a session.

    @param[in] backup_thd    The thd to restore to when object is destructed.
    @param[in] new_thd       The thd to attach to.
   */

  Thd_backup_and_restore(THD *backup_thd, THD *new_thd)
      : m_backup_thd(backup_thd),
        m_new_thd(new_thd),
        m_new_thd_old_real_id(new_thd->real_id),
        m_new_thd_old_thread_stack(new_thd->thread_stack) {
    assert(m_backup_thd != nullptr && m_new_thd != nullptr);
    // Reset the state of the current thd.
    m_backup_thd->restore_globals();

    m_new_thd->thread_stack = m_backup_thd->thread_stack;
    m_new_thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_mem_cnt_THD)(m_new_thd, &m_backup_cnt_thd);
#endif
  }

  /**
      Restores to previous thd.
   */
  ~Thd_backup_and_restore() {
    /*
      Restore the global variables of the thd we previously attached to,
      to its original state. In other words, detach the m_new_thd.
    */
    m_new_thd->restore_globals();
    m_new_thd->real_id = m_new_thd_old_real_id;
    m_new_thd->thread_stack = m_new_thd_old_thread_stack;

    // Reset the global variables to the original state.
    m_backup_thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_mem_cnt_THD)(m_backup_cnt_thd, &m_dummy_cnt_thd);
#endif
  }

 private:
  THD *m_backup_thd;
  THD *m_new_thd;
  THD *m_backup_cnt_thd;
  THD *m_dummy_cnt_thd;
  my_thread_t m_new_thd_old_real_id;
  const char *m_new_thd_old_thread_stack;
};

/**
  Caches for non-transactional and transactional data before writing
  it to the binary log.

  @todo All the access functions for the flags suggest that the
  encapsuling is not done correctly, so try to move any logic that
  requires access to the flags into the cache.
*/
class binlog_cache_data {
 public:
  binlog_cache_data(class binlog_cache_mngr &cache_mngr, bool trx_cache_arg,
                    ulong *ptr_binlog_cache_use_arg,
                    ulong *ptr_binlog_cache_disk_use_arg)
      : m_cache_mngr(cache_mngr),
        m_pending(nullptr),
        ptr_binlog_cache_use(ptr_binlog_cache_use_arg),
        ptr_binlog_cache_disk_use(ptr_binlog_cache_disk_use_arg) {
    flags.transactional = trx_cache_arg;
  }

  bool open(my_off_t cache_size, my_off_t max_cache_size) {
    return m_cache.open(cache_size, max_cache_size);
  }

  Binlog_cache_storage *get_cache() { return &m_cache; }
  int finalize(THD *thd, Log_event *end_event);
  int finalize(THD *thd, Log_event *end_event, XID_STATE *xs);
  int flush(THD *thd, my_off_t *bytes, bool *wrote_xid,
            bool parallelization_barrier);
  int write_event(Log_event *event);
  void set_event_counter(size_t event_counter) {
    m_event_counter = event_counter;
  }
  size_t get_event_counter() const { return m_event_counter; }
  size_t get_compressed_size() const { return m_compressed_size; }
  size_t get_decompressed_size() const { return m_decompressed_size; }
  mysql::binlog::event::compression::type get_compression_type() const {
    return m_compression_type;
  }

  void set_compressed_size(size_t s) { m_compressed_size = s; }
  void set_decompressed_size(size_t s) { m_decompressed_size = s; }
  void set_compression_type(mysql::binlog::event::compression::type t) {
    m_compression_type = t;
  }

  virtual ~binlog_cache_data() {
    assert(is_binlog_empty());
    m_cache.close();
  }

  bool is_binlog_empty() const {
    DBUG_PRINT("debug", ("%s_cache - pending: 0x%llx, bytes: %llu",
                         (flags.transactional ? "trx" : "stmt"),
                         (ulonglong)pending(), (ulonglong)m_cache.length()));
    return pending() == nullptr && m_cache.is_empty();
  }

  bool is_finalized() const { return flags.finalized; }

  Rows_log_event *pending() const { return m_pending; }

  void set_pending(Rows_log_event *const pending) { m_pending = pending; }

  /// @see handle_deferred_cache_write_incident
  void set_incident(
      std::string_view incident_message =
          "Non-transactional changes were not written to the binlog.");

  /// @see handle_deferred_cache_write_incident
  bool has_incident(void) const;

  bool has_xid() const {
    // There should only be an XID event if we are transactional
    assert((flags.transactional && flags.with_xid) || !flags.with_xid);
    return flags.with_xid;
  }

  bool is_trx_cache() const { return flags.transactional; }

  my_off_t get_byte_position() const { return m_cache.length(); }

  void cache_state_checkpoint(my_off_t pos_to_checkpoint) {
    // We only need to store the cache state for pos > 0
    if (pos_to_checkpoint) {
      cache_state state;
      state.with_rbr = flags.with_rbr;
      state.with_sbr = flags.with_sbr;
      state.with_start = flags.with_start;
      state.with_end = flags.with_end;
      state.with_content = flags.with_content;
      state.event_counter = m_event_counter;
      cache_state_map[pos_to_checkpoint] = state;
    }
  }

  void cache_state_rollback(my_off_t pos_to_rollback) {
    if (pos_to_rollback) {
      std::map<my_off_t, cache_state>::iterator it;
      it = cache_state_map.find(pos_to_rollback);
      if (it != cache_state_map.end()) {
        flags.with_rbr = it->second.with_rbr;
        flags.with_sbr = it->second.with_sbr;
        flags.with_start = it->second.with_start;
        flags.with_end = it->second.with_end;
        flags.with_content = it->second.with_content;
        m_event_counter = it->second.event_counter;
      } else
        assert(it == cache_state_map.end());
    }
    // Rolling back to pos == 0 means cleaning up the cache.
    else {
      flags.with_rbr = false;
      flags.with_sbr = false;
      flags.with_start = false;
      flags.with_end = false;
      flags.with_content = false;
      m_event_counter = 0;
    }
  }

  /**
     Reset the cache to unused state when the transaction is finished. It
     drops all data in the cache and clears the flags of the transaction state.
  */
  virtual void reset() {
    compute_statistics();
    remove_pending_event();

    if (m_cache.reset()) {
      LogErr(WARNING_LEVEL, ER_BINLOG_CANT_RESIZE_CACHE);
    }

    flags.with_xid = false;
    flags.immediate = false;
    flags.finalized = false;
    flags.with_sbr = false;
    flags.with_rbr = false;
    flags.with_start = false;
    flags.with_end = false;
    flags.with_content = false;

    /*
      The truncate function calls reinit_io_cache that calls my_b_flush_io_cache
      which may increase disk_writes. This breaks the disk_writes use by the
      binary log which aims to compute the ratio between in-memory cache usage
      and disk cache usage. To avoid this undesirable behavior, we reset the
      variable after truncating the cache.
    */
    cache_state_map.clear();
    m_event_counter = 0;
    m_compressed_size = 0;
    m_decompressed_size = 0;
    m_compression_type = mysql::binlog::event::compression::NONE;
    assert(is_binlog_empty());
  }

  /**
    Returns information about the cache content with respect to
    the binlog_format of the events.

    This will be used to set a flag on GTID_LOG_EVENT stating that the
    transaction may have SBR statements or not, but the binlog dump
    will show this flag as "rbr_only" when it is not set. That's why
    an empty transaction should return true below, or else an empty
    transaction would be assumed as "rbr_only" even not having RBR
    events.

    When dumping a binary log content using mysqlbinlog client program,
    for any transaction assumed as "rbr_only" it will be printed a
    statement changing the transaction isolation level to READ COMMITTED.
    It doesn't make sense to have an empty transaction "requiring" this
    isolation level change.

    @return true  The cache have SBR events or is empty.
    @return false The cache contains a transaction with no SBR events.
   */
  bool may_have_sbr_stmts() { return flags.with_sbr || !flags.with_rbr; }

  /**
    Check if the binlog cache contains an empty transaction, which has
    two binlog events "BEGIN" and "COMMIT".

    @return true  The binlog cache contains an empty transaction.
    @return false Otherwise.
  */
  bool has_empty_transaction() {
    /*
      The empty transaction has two events in trx/stmt binlog cache
      and no changes: one is a transaction start and other is a transaction
      end (there should be no SBR changing content and no RBR events).
    */
    if (flags.with_start &&   // Has transaction start statement
        flags.with_end &&     // Has transaction end statement
        !flags.with_content)  // Has no other content than START/END
    {
      assert(m_event_counter == 2);  // Two events in the cache only
      assert(!flags.with_sbr);       // No statements changing content
      assert(!flags.with_rbr);       // No rows changing content
      assert(!flags.immediate);      // Not a DDL
      assert(!flags.with_xid);  // Not a XID trx and not an atomic DDL Query
      return true;
    }
    return false;
  }

  /**
    Check if the binlog cache is empty or contains an empty transaction,
    which has two binlog events "BEGIN" and "COMMIT".

    @return true  The binlog cache is empty or contains an empty transaction.
    @return false Otherwise.
  */
  bool is_empty_or_has_empty_transaction() {
    return is_binlog_empty() || has_empty_transaction();
  }

 protected:
  /*
    This structure should have all cache variables/flags that should be restored
    when a ROLLBACK TO SAVEPOINT statement be executed.
  */
  struct cache_state {
    bool with_sbr;
    bool with_rbr;
    bool with_start;
    bool with_end;
    bool with_content;
    size_t event_counter;
  };
  /*
    For every SAVEPOINT used, we will store a cache_state for the current
    binlog cache position. So, if a ROLLBACK TO SAVEPOINT is used, we can
    restore the cache_state values after truncating the binlog cache.
  */
  std::map<my_off_t, cache_state> cache_state_map;
  /*
    In order to compute the transaction size (because of possible extra checksum
    bytes), we need to keep track of how many events are in the binlog cache.
  */
  size_t m_event_counter = 0;

  size_t m_compressed_size = 0;
  size_t m_decompressed_size = 0;
  mysql::binlog::event::compression::type m_compression_type =
      mysql::binlog::event::compression::type::NONE;
  /*
    It truncates the cache to a certain position. This includes deleting the
    pending event. It corresponds to rollback statement or rollback to
    a savepoint. It doesn't change transaction state.
   */
  void truncate(my_off_t pos) {
    DBUG_PRINT("info", ("truncating to position %lu", (ulong)pos));
    remove_pending_event();

    // TODO: check the return value.
    (void)m_cache.truncate(pos);
  }

  /**
     Flush pending event to the cache buffer.
   */
  int flush_pending_event(THD *thd) {
    if (m_pending) {
      m_pending->set_flags(Rows_log_event::STMT_END_F);
      if (int error = write_event(m_pending)) return error;
      thd->clear_binlog_table_maps();
    }
    return 0;
  }

  /**
    Remove the pending event.
   */
  int remove_pending_event() {
    delete m_pending;
    m_pending = nullptr;
    return 0;
  }
  struct Flags {
    /*
      Defines if this is either a trx-cache or stmt-cache, respectively, a
      transactional or non-transactional cache.
    */
    bool transactional : 1;

    /*
      This indicates that the cache should be written without BEGIN/END.
    */
    bool immediate : 1;

    /*
      This flag indicates that the buffer was finalized and has to be
      flushed to disk.
     */
    bool finalized : 1;

    /*
      This indicates that either the cache contain an XID event, or it's
      an atomic DDL Query-log-event. In the latter case the flag is set up
      on the statement level, namely when the Query-log-event is cached
      at time the DDL transaction is not committing.
      The flag therefore gets reset when the cache is cleaned due to
      the statement rollback, e.g in case of a DDL post-caching execution
      error.
      Any statement scope flag among other things must consider its
      reset policy when the statement is rolled back.
    */
    bool with_xid : 1;

    /*
      This indicates that the cache contain statements changing content.
    */
    bool with_sbr : 1;

    /*
      This indicates that the cache contain RBR event changing content.
    */
    bool with_rbr : 1;

    /*
      This indicates that the cache contain s transaction start statement.
    */
    bool with_start : 1;

    /*
      This indicates that the cache contain a transaction end event.
    */
    bool with_end : 1;

    /*
      This indicates that the cache contain content other than START/END.
    */
    bool with_content : 1;
  } flags;

  /// Compress the current transaction "in-place", if possible
  ///
  /// This attempts to compress the transaction if it satisfies the
  /// necessary pre-conditions. Otherwise it does nothing.
  ///
  /// @retval true Error: the cache has been corrupted and the
  /// transaction must be aborted.
  ///
  /// @retval false Success: the transaction was either compressed
  /// successfully, or compression was not attempted, or compression
  /// failed and left the uncompressed transaction intact.
  [[NODISCARD]] bool compress(THD *thd);

 private:
  /*
    Reference to the cache_mngr which owns this cache.
   */
  class binlog_cache_mngr &m_cache_mngr;

  /*
    Storage for byte data. This binlog_cache_data will serialize
    events into bytes and put them into m_cache.
  */
  Binlog_cache_storage m_cache;

  /*
    Pending binrows event. This event is the event where the rows are currently
    written.
   */
  Rows_log_event *m_pending;

  /**
    This function computes binlog cache and disk usage.
  */
  void compute_statistics() {
    if (!is_binlog_empty()) {
      (*ptr_binlog_cache_use)++;
      if (m_cache.disk_writes() != 0) (*ptr_binlog_cache_disk_use)++;
    }
  }

  /*
    Stores a pointer to the status variable that keeps track of the in-memory
    cache usage. This corresponds to either
      . binlog_cache_use or binlog_stmt_cache_use.
  */
  ulong *ptr_binlog_cache_use;

  /*
    Stores a pointer to the status variable that keeps track of the disk
    cache usage. This corresponds to either
      . binlog_cache_disk_use or binlog_stmt_cache_disk_use.
  */
  ulong *ptr_binlog_cache_disk_use;

  binlog_cache_data &operator=(const binlog_cache_data &info);
  binlog_cache_data(const binlog_cache_data &info);
};

class binlog_stmt_cache_data : public binlog_cache_data {
 public:
  binlog_stmt_cache_data(binlog_cache_mngr &cache_mngr, bool trx_cache_arg,
                         ulong *ptr_binlog_cache_use_arg,
                         ulong *ptr_binlog_cache_disk_use_arg)
      : binlog_cache_data(cache_mngr, trx_cache_arg, ptr_binlog_cache_use_arg,
                          ptr_binlog_cache_disk_use_arg) {}

  using binlog_cache_data::finalize;

  int finalize(THD *thd);
};

int binlog_stmt_cache_data::finalize(THD *thd) {
  if (flags.immediate) {
    if (int error = finalize(thd, nullptr)) return error;
  } else {
    Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"), false, false, true,
                            0, true);
    if (int error = finalize(thd, &end_evt)) return error;
  }
  return 0;
}

class binlog_trx_cache_data : public binlog_cache_data {
 public:
  binlog_trx_cache_data(binlog_cache_mngr &cache_mngr, bool trx_cache_arg,
                        ulong *ptr_binlog_cache_use_arg,
                        ulong *ptr_binlog_cache_disk_use_arg)
      : binlog_cache_data(cache_mngr, trx_cache_arg, ptr_binlog_cache_use_arg,
                          ptr_binlog_cache_disk_use_arg),
        m_cannot_rollback(false),
        before_stmt_pos(MY_OFF_T_UNDEF) {}

  void reset() override {
    DBUG_TRACE;
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    m_cannot_rollback = false;
    before_stmt_pos = MY_OFF_T_UNDEF;
    binlog_cache_data::reset();
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    return;
  }

  bool cannot_rollback() const { return m_cannot_rollback; }

  void set_cannot_rollback() { m_cannot_rollback = true; }

  my_off_t get_prev_position() const { return before_stmt_pos; }

  void set_prev_position(my_off_t pos) {
    DBUG_TRACE;
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    before_stmt_pos = pos;
    cache_state_checkpoint(before_stmt_pos);
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    return;
  }

  void restore_prev_position() {
    DBUG_TRACE;
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    binlog_cache_data::truncate(before_stmt_pos);
    cache_state_rollback(before_stmt_pos);
    before_stmt_pos = MY_OFF_T_UNDEF;
    /*
      Binlog statement rollback clears with_xid now as the atomic DDL statement
      marker which can be set as early as at event creation and caching.
    */
    flags.with_xid = false;
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    return;
  }

  void restore_savepoint(my_off_t pos) {
    DBUG_TRACE;
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    binlog_cache_data::truncate(pos);
    if (pos <= before_stmt_pos) before_stmt_pos = MY_OFF_T_UNDEF;
    cache_state_rollback(pos);
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong)before_stmt_pos));
    return;
  }

  using binlog_cache_data::truncate;

  void truncate(THD *thd, bool all);

 private:
  /*
    It will be set true if any statement which cannot be rolled back safely
    is put in trx_cache.
  */
  bool m_cannot_rollback;

  /*
    Binlog position before the start of the current statement.
  */
  my_off_t before_stmt_pos;

  binlog_trx_cache_data &operator=(const binlog_trx_cache_data &info);
  binlog_trx_cache_data(const binlog_trx_cache_data &info);
};

class binlog_cache_mngr {
  /// Indicates that some events did not get into the cache(s) and most
  /// likely it is incomplete. @see handle_deferred_cache_write_incident
  std::string m_incident;

 public:
  binlog_cache_mngr(ulong *ptr_binlog_stmt_cache_use_arg,
                    ulong *ptr_binlog_stmt_cache_disk_use_arg,
                    ulong *ptr_binlog_cache_use_arg,
                    ulong *ptr_binlog_cache_disk_use_arg)
      : stmt_cache(*this, false, ptr_binlog_stmt_cache_use_arg,
                   ptr_binlog_stmt_cache_disk_use_arg),
        trx_cache(*this, true, ptr_binlog_cache_use_arg,
                  ptr_binlog_cache_disk_use_arg) {}

  bool init() {
    return stmt_cache.open(binlog_stmt_cache_size,
                           max_binlog_stmt_cache_size) ||
           trx_cache.open(binlog_cache_size, max_binlog_cache_size);
  }

  binlog_cache_data *get_binlog_cache_data(bool is_transactional) {
    if (is_transactional)
      return &trx_cache;
    else
      return &stmt_cache;
  }

  Binlog_cache_storage *get_stmt_cache() { return stmt_cache.get_cache(); }
  Binlog_cache_storage *get_trx_cache() { return trx_cache.get_cache(); }
  /**
    Convenience method to check if both caches are empty.
   */
  bool is_binlog_empty() const {
    return stmt_cache.is_binlog_empty() && trx_cache.is_binlog_empty();
  }

  int handle_deferred_cache_write_incident(THD *thd);

  /// Check if either of the caches have an incident
  /// @see handle_deferred_cache_write_incident
  bool has_incident() const { return !m_incident.empty(); }

  void set_incident(std::string_view incident_message) {
    assert(!incident_message.empty());
    m_incident = incident_message;
  }

  /*
    clear stmt_cache and trx_cache if they are not empty
  */
  void reset() {
    if (!stmt_cache.is_binlog_empty()) stmt_cache.reset();
    if (!trx_cache.is_binlog_empty()) trx_cache.reset();
  }

#ifndef NDEBUG
  bool dbug_any_finalized() const {
    return stmt_cache.is_finalized() || trx_cache.is_finalized();
  }
#endif

  /*
    Convenience method to flush both caches to the binary log.

    @param bytes_written Pointer to variable that will be set to the
                         number of bytes written for the flush.
    @param wrote_xid     Pointer to variable that will be set to @c
                         true if any XID event was written to the
                         binary log. Otherwise, the variable will not
                         be touched.
    @return Error code on error, zero if no error.
   */
  int flush(THD *thd, my_off_t *bytes_written, bool *wrote_xid) {
    my_off_t stmt_bytes = 0;
    my_off_t trx_bytes = 0;
    assert(stmt_cache.has_xid() == 0);

    bool parallelization_barrier = false;
    if (has_incident()) {
      if (int error = handle_deferred_cache_write_incident(thd)) return error;
      // Request force rotate
      thd->rpl_thd_ctx.binlog_group_commit_ctx().set_force_rotate();
      // Set as parallelization_barrier so that dependency tracker marks all
      // subsequent transactions to depend on it.
      parallelization_barrier = true;
    }

    int error =
        stmt_cache.flush(thd, &stmt_bytes, wrote_xid, parallelization_barrier);
    if (error) return error;
    DEBUG_SYNC(thd, "after_flush_stm_cache_before_flush_trx_cache");
    error =
        trx_cache.flush(thd, &trx_bytes, wrote_xid, parallelization_barrier);
    if (error) return error;
    *bytes_written = stmt_bytes + trx_bytes;
    return 0;
  }

  /**
    Check if at least one of transactions and statement binlog caches
    contains an empty transaction, other one is empty or contains an
    empty transaction.

    @return true  At least one of transactions and statement binlog
                  caches an empty transaction, other one is empty
                  or contains an empty transaction.
    @return false Otherwise.
  */
  bool has_empty_transaction() {
    return (trx_cache.is_empty_or_has_empty_transaction() &&
            stmt_cache.is_empty_or_has_empty_transaction() &&
            !is_binlog_empty());
  }

  binlog_stmt_cache_data stmt_cache;
  binlog_trx_cache_data trx_cache;

 private:
  binlog_cache_mngr &operator=(const binlog_cache_mngr &info);
  binlog_cache_mngr(const binlog_cache_mngr &info);
};

static binlog_cache_mngr *thd_get_cache_mngr(const THD *thd) {
  /*
    If opt_bin_log is not set, binlog_hton->slot == -1 and hence
    thd_get_ha_data(thd, hton) segfaults.
  */
  assert(opt_bin_log);
  return (binlog_cache_mngr *)thd_get_ha_data(thd, binlog_hton);
}

/**
  Checks if the BINLOG_CACHE_SIZE's value is greater than MAX_BINLOG_CACHE_SIZE.
  If this happens, the BINLOG_CACHE_SIZE is set to MAX_BINLOG_CACHE_SIZE.
*/
void check_binlog_cache_size(THD *thd) {
  if (binlog_cache_size > max_binlog_cache_size) {
    if (thd) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX,
          ER_THD(thd, ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX),
          (ulong)binlog_cache_size, (ulong)max_binlog_cache_size);
    } else {
      LogErr(WARNING_LEVEL, ER_BINLOG_CACHE_SIZE_TOO_LARGE, binlog_cache_size,
             (ulong)max_binlog_cache_size);
    }
    binlog_cache_size = static_cast<ulong>(max_binlog_cache_size);
  }
}

/**
  Checks if the BINLOG_STMT_CACHE_SIZE's value is greater than
  MAX_BINLOG_STMT_CACHE_SIZE. If this happens, the BINLOG_STMT_CACHE_SIZE is set
  to MAX_BINLOG_STMT_CACHE_SIZE.
*/
void check_binlog_stmt_cache_size(THD *thd) {
  if (binlog_stmt_cache_size > max_binlog_stmt_cache_size) {
    if (thd) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING,
          ER_BINLOG_STMT_CACHE_SIZE_GREATER_THAN_MAX,
          ER_THD(thd, ER_BINLOG_STMT_CACHE_SIZE_GREATER_THAN_MAX),
          (ulong)binlog_stmt_cache_size, (ulong)max_binlog_stmt_cache_size);
    } else {
      LogErr(WARNING_LEVEL, ER_BINLOG_STMT_CACHE_SIZE_TOO_LARGE,
             binlog_stmt_cache_size, (ulong)max_binlog_stmt_cache_size);
    }
    binlog_stmt_cache_size = static_cast<ulong>(max_binlog_stmt_cache_size);
  }
}

/**
 Check whether binlog_hton has valid slot and enabled
*/
bool binlog_enabled() {
  return (binlog_hton && binlog_hton->slot != HA_SLOT_UNDEF);
}

/*
 Save position of binary log transaction cache.

 SYNPOSIS
   binlog_trans_log_savepos()

   thd      The thread to take the binlog data from
   pos      Pointer to variable where the position will be stored

 DESCRIPTION

   Save the current position in the binary log transaction cache into
   the variable pointed to by 'pos'
*/

static void binlog_trans_log_savepos(THD *thd, my_off_t *pos) {
  DBUG_TRACE;
  assert(pos != nullptr);
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
  assert(mysql_bin_log.is_open());
  *pos = cache_mngr->trx_cache.get_byte_position();
  DBUG_PRINT("return", ("position: %lu", (ulong)*pos));
  cache_mngr->trx_cache.cache_state_checkpoint(*pos);
}

static int binlog_dummy_recover(handlerton *, XA_recover_txn *, uint,
                                MEM_ROOT *) {
  return 0;
}

/**
  Auxiliary class to copy serialized events to the binary log and
  correct some of the fields that are not known until just before
  writing the event.

  This class allows feeding events in parts, so it is practical to use
  in do_write_cache() which reads events from an IO_CACHE where events
  may span multiple cache pages.

  The following fields are fixed before writing the event:
  - end_log_pos is set
  - the checksum is computed if checksums are enabled
  - the length is incremented by the checksum size if checksums are enabled
*/
class Binlog_event_writer : public Basic_ostream {
  MYSQL_BIN_LOG::Binlog_ofile *m_binlog_file;
  bool have_checksum;
  ha_checksum initial_checksum;
  ha_checksum checksum;
  uint32 end_log_pos;
  uchar header[LOG_EVENT_HEADER_LEN];
  my_off_t header_len = 0;
  uint32 event_len = 0;

 public:
  /**
    Constructs a new Binlog_event_writer. Should be called once before
    starting to flush the transaction or statement cache to the
    binlog.

    @param binlog_file to write to.
  */
  Binlog_event_writer(MYSQL_BIN_LOG::Binlog_ofile *binlog_file)
      : m_binlog_file(binlog_file),
        have_checksum(binlog_checksum_options !=
                      mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF),
        initial_checksum(my_checksum(0L, nullptr, 0)),
        checksum(initial_checksum),
        end_log_pos(binlog_file->position()) {
    // Simulate checksum error
    if (DBUG_EVALUATE_IF("fault_injection_crc_value", 1, 0)) checksum--;
  }

  void update_header() {
    event_len = uint4korr(header + EVENT_LEN_OFFSET);

    // Increase end_log_pos
    end_log_pos += event_len;

    // Update event length if it has checksum
    if (have_checksum) {
      int4store(header + EVENT_LEN_OFFSET, event_len + BINLOG_CHECKSUM_LEN);
      end_log_pos += BINLOG_CHECKSUM_LEN;
    }

    // Store end_log_pos
    int4store(header + LOG_POS_OFFSET, end_log_pos);
    // update the checksum
    if (have_checksum) checksum = my_checksum(checksum, header, header_len);
  }

  bool write(const unsigned char *buffer, my_off_t length) override {
    DBUG_TRACE;

    while (length > 0) {
      /* Write event header into binlog */
      if (event_len == 0) {
        /* data in the buf may be smaller than header size.*/
        uint32 header_incr =
            std::min<uint32>(LOG_EVENT_HEADER_LEN - header_len, length);

        memcpy(header + header_len, buffer, header_incr);
        header_len += header_incr;
        buffer += header_incr;
        length -= header_incr;

        if (header_len == LOG_EVENT_HEADER_LEN) {
          update_header();
          if (m_binlog_file->write(header, header_len)) return true;

          event_len -= header_len;
          header_len = 0;
        }
      } else {
        my_off_t write_bytes = std::min<uint64>(length, event_len);

        if (m_binlog_file->write(buffer, write_bytes)) return true;

        // update the checksum
        if (have_checksum)
          checksum = my_checksum(checksum, buffer, write_bytes);

        event_len -= write_bytes;
        length -= write_bytes;
        buffer += write_bytes;

        // The whole event is copied, now add the checksum
        if (have_checksum && event_len == 0) {
          uchar checksum_buf[BINLOG_CHECKSUM_LEN];

          int4store(checksum_buf, checksum);
          if (m_binlog_file->write(checksum_buf, BINLOG_CHECKSUM_LEN))
            return true;
          checksum = initial_checksum;
        }
      }
    }
    return false;
  }
  /**
    Returns true if per event checksum is enabled.
  */
  bool is_checksum_enabled() { return have_checksum; }
};

/*
  this function is mostly a placeholder.
  conceptually, binlog initialization (now mostly done in MYSQL_BIN_LOG::open)
  should be moved here.
*/

static int binlog_init(void *p) {
  binlog_hton = (handlerton *)p;
  binlog_hton->state = opt_bin_log ? SHOW_OPTION_YES : SHOW_OPTION_NO;
  binlog_hton->db_type = DB_TYPE_BINLOG;
  binlog_hton->savepoint_offset = sizeof(my_off_t);
  binlog_hton->close_connection = binlog_close_connection;
  binlog_hton->savepoint_set = binlog_savepoint_set;
  binlog_hton->savepoint_rollback = binlog_savepoint_rollback;
  binlog_hton->savepoint_rollback_can_release_mdl =
      binlog_savepoint_rollback_can_release_mdl;
  binlog_hton->commit = binlog_commit;
  binlog_hton->rollback = binlog_rollback;
  binlog_hton->prepare = binlog_prepare;
  binlog_hton->set_prepared_in_tc = binlog_set_prepared_in_tc;
  binlog_hton->recover = binlog_dummy_recover;
  binlog_hton->flags = HTON_NOT_USER_SELECTABLE | HTON_HIDDEN;
  return 0;
}

static int binlog_deinit(void *) {
  /* Using binlog as TC after the binlog has been unloaded, won't work */
  if (tc_log == &mysql_bin_log) tc_log = nullptr;
  binlog_hton = nullptr;
  return 0;
}

static int binlog_close_connection(handlerton *, THD *thd) {
  DBUG_TRACE;
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
  assert(cache_mngr->is_binlog_empty());
  DBUG_PRINT("debug", ("Set ha_data slot %d to 0x%llx", binlog_hton->slot,
                       (ulonglong) nullptr));
  thd_set_ha_data(thd, binlog_hton, nullptr);
  cache_mngr->~binlog_cache_mngr();
  my_free(cache_mngr);
  return 0;
}

int binlog_cache_data::write_event(Log_event *ev) {
  DBUG_TRACE;

  if (ev != nullptr) {
    DBUG_EXECUTE_IF("simulate_disk_full_at_flush_pending",
                    { DBUG_SET("+d,simulate_file_write_error"); });

    if (binary_event_serialize(ev, &m_cache)) {
      DBUG_EXECUTE_IF("simulate_disk_full_at_flush_pending", {
        DBUG_SET("-d,simulate_file_write_error");
        DBUG_SET("-d,simulate_disk_full_at_flush_pending");
        /*
           after +d,simulate_file_write_error the local cache
           is in unsane state. Since -d,simulate_file_write_error
           revokes the first simulation do_write_cache()
           can't be run without facing an assert.
           So it's blocked with the following 2nd simulation:
        */
        DBUG_SET("+d,simulate_do_write_cache_failure");
      });
      return 1;
    }
    if (ev->get_type_code() == mysql::binlog::event::XID_EVENT ||
        ev->get_type_code() == mysql::binlog::event::XA_PREPARE_LOG_EVENT)
      flags.with_xid = true;
    if (ev->is_using_immediate_logging()) flags.immediate = true;
    /* DDL gets marked as xid-requiring at its caching. */
    if (is_atomic_ddl_event(ev)) flags.with_xid = true;
    /* With respect to the event type being written */
    if (ev->is_sbr_logging_format()) flags.with_sbr = true;
    if (ev->is_rbr_logging_format()) flags.with_rbr = true;
    /* With respect to empty transactions */
    if (ev->starts_group()) flags.with_start = true;
    if (ev->ends_group()) flags.with_end = true;
    if (!ev->starts_group() && !ev->ends_group()) flags.with_content = true;
    m_event_counter++;
    DBUG_PRINT("debug",
               ("event_counter= %lu", static_cast<ulong>(m_event_counter)));
  }
  return 0;
}

void binlog_cache_data::set_incident(std::string_view incident_message) {
  m_cache_mngr.set_incident(incident_message);
}

bool binlog_cache_data::has_incident() const {
  return m_cache_mngr.has_incident();
}

bool MYSQL_BIN_LOG::assign_automatic_gtids_to_flush_group(THD *first_seen) {
  DBUG_TRACE;
  bool error = false;
  bool is_global_tsid_locked = false;

  Scope_guard global_tsid_lock_scope_guard([&is_global_tsid_locked]() {
    if (is_global_tsid_locked) {
      global_tsid_lock->unlock();
    }
  });

  Gtid_state::Locked_sidno_set locked_sidno_set(*gtid_state);
  for (THD *head = first_seen; head; head = head->next_to_commit) {
    if (head->variables.gtid_next.is_automatic() && !is_global_tsid_locked) {
      global_tsid_lock->rdlock();
      is_global_tsid_locked = true;
    }
    auto sidno = gtid_state->specify_transaction_sidno(head, locked_sidno_set);
    head->get_transaction()->get_rpl_transaction_ctx()->set_sidno(sidno);
  }
  locked_sidno_set.lock();
  for (THD *head = first_seen; head; head = head->next_to_commit) {
    assert(head->variables.gtid_next.type != UNDEFINED_GTID);

    /* Generate GTID */
    if (Gtid_specification::is_automatic(head->variables.gtid_next.type)) {
      auto [ctx_sidno, ctx_gno] = head->get_transaction()
                                      ->get_rpl_transaction_ctx()
                                      ->get_gtid_components();

      if (gtid_state->generate_automatic_gtid(head, ctx_sidno, ctx_gno) !=
          RETURN_STATUS_OK) {
        head->commit_error = THD::CE_FLUSH_GNO_EXHAUSTED_ERROR;
        error = true;
      }
    } else {
      DBUG_PRINT("info",
                 ("thd->variables.gtid_next.type=%d "
                  "thd->owned_gtid.sidno=%d",
                  head->variables.gtid_next.type, head->owned_gtid.sidno));
      if (head->variables.gtid_next.type == ASSIGNED_GTID)
        assert(head->owned_gtid.sidno > 0);
      else {
        assert(head->variables.gtid_next.type == ANONYMOUS_GTID);
        assert(head->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS);
      }
    }
  }
  return error;
}

/**
  Write the Gtid_log_event to the binary log (prior to writing the
  statement or transaction cache).

  @param thd Thread that is committing.
  @param cache_data The cache that is flushing.
  @param writer The event will be written to this Binlog_event_writer object.
  @param parallelization_barrier The transaction is a parallelization_barrier
  and the dependency tracker should mark subsequent transactions to depend on
  it.

  @retval false Success.
  @retval true Error.
*/
bool MYSQL_BIN_LOG::write_transaction(THD *thd, binlog_cache_data *cache_data,
                                      Binlog_event_writer *writer,
                                      bool parallelization_barrier) {
  DBUG_TRACE;

  /*
    The GTID for the THD was assigned at
    assign_automatic_gtids_to_flush_group()
  */
  assert(thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS ||
         thd->owned_gtid.sidno > 0);

  int64 sequence_number, last_committed;
  /* Generate logical timestamps for MTS */
  m_dependency_tracker.get_dependency(thd, parallelization_barrier,
                                      sequence_number, last_committed);

  /*
    In case both the transaction cache and the statement cache are
    non-empty, both will be flushed in sequence and logged as
    different transactions. Then the second transaction must only
    be executed after the first one has committed. Therefore, we
    need to set last_committed for the second transaction equal to
    last_committed for the first transaction. This is done in
    binlog_cache_data::flush. binlog_cache_data::flush uses the
    condition trn_ctx->last_committed==SEQ_UNINIT to detect this
    situation, hence the need to set it here.
  */
  thd->get_transaction()->last_committed = SEQ_UNINIT;

  /*
    For delayed replication and also for the purpose of lag monitoring,
    we assume that the commit timestamp of the transaction is the time of
    executing this code (the time of writing the Gtid_log_event to the binary
    log).
  */
  ulonglong immediate_commit_timestamp = my_micro_time();

  /*
    When the original_commit_timestamp session variable is set to a value
    other than UNDEFINED_COMMIT_TIMESTAMP, it means that either the timestamp
    is known ( > 0 ) or the timestamp is not known ( == 0 ).
  */
  ulonglong original_commit_timestamp =
      thd->variables.original_commit_timestamp;
  /*
    When original_commit_timestamp == UNDEFINED_COMMIT_TIMESTAMP, we assume
    that:
    a) it is not known if this thread is a slave applier ( = 0 );
    b) this is a new transaction ( = immediate_commit_timestamp);
  */
  if (original_commit_timestamp == UNDEFINED_COMMIT_TIMESTAMP) {
    /*
      When applying a transaction using replication, assume that the
      original commit timestamp is not known (the transaction wasn't
      originated on the current server).
    */
    if (thd->slave_thread || thd->is_binlog_applier()) {
      original_commit_timestamp = 0;
    } else
    /* Assume that this transaction is original from this server */
    {
      DBUG_EXECUTE_IF("rpl_invalid_gtid_timestamp",
                      // add one our to the commit timestamps
                      immediate_commit_timestamp += 3600000000;);
      original_commit_timestamp = immediate_commit_timestamp;
    }
  } else {
    // Clear the session variable to have cleared states for next transaction.
    thd->variables.original_commit_timestamp = UNDEFINED_COMMIT_TIMESTAMP;
  }

  uint32_t trx_immediate_server_version =
      do_server_version_int(::server_version);
  // Clear the session variable to have cleared states for next transaction.
  thd->variables.immediate_server_version = UNDEFINED_SERVER_VERSION;
  DBUG_EXECUTE_IF("fixed_server_version",
                  trx_immediate_server_version = 888888;);
  DBUG_EXECUTE_IF("gr_fixed_server_version",
                  trx_immediate_server_version = 777777;);

  /*
    When the original_server_version session variable is set to a value
    other than UNDEFINED_SERVER_VERSION, it means that either the
    server version is known or the server_version is not known
    (UNKNOWN_SERVER_VERSION).
  */
  uint32_t trx_original_server_version = thd->variables.original_server_version;

  /*
    When original_server_version == UNDEFINED_SERVER_VERSION, we assume
    that:
    a) it is not known if this thread is a slave applier ( = 0 );
    b) this is a new transaction ( = ::server_version);
  */
  if (trx_original_server_version == UNDEFINED_SERVER_VERSION) {
    /*
      When applying a transaction using replication, assume that the
      original server version is not known (the transaction wasn't
      originated on the current server).
    */
    if (thd->slave_thread || thd->is_binlog_applier()) {
      trx_original_server_version = UNKNOWN_SERVER_VERSION;
    } else
    /* Assume that this transaction is original from this server */
    {
      trx_original_server_version = trx_immediate_server_version;
    }
  } else {
    // Clear the session variable to have cleared states for next transaction.
    thd->variables.original_server_version = UNDEFINED_SERVER_VERSION;
  }
  Gtid_log_event gtid_event(
      thd, cache_data->is_trx_cache(), last_committed, sequence_number,
      cache_data->may_have_sbr_stmts(), original_commit_timestamp,
      immediate_commit_timestamp, trx_original_server_version,
      trx_immediate_server_version);

  // Set the transaction length, based on cache info
  gtid_event.set_trx_length_by_cache_size(cache_data->get_byte_position(),
                                          writer->is_checksum_enabled(),
                                          cache_data->get_event_counter());

  DBUG_PRINT("debug", ("cache_data->get_byte_position()= %llu",
                       cache_data->get_byte_position()));
  DBUG_PRINT("debug", ("cache_data->get_event_counter()= %lu",
                       static_cast<ulong>(cache_data->get_event_counter())));
  DBUG_PRINT("debug", ("writer->is_checksum_enabled()= %s",
                       YESNO(writer->is_checksum_enabled())));
  DBUG_PRINT("debug", ("gtid_event.get_event_length()= %lu",
                       static_cast<ulong>(gtid_event.get_event_length())));
  DBUG_PRINT("info", ("transaction_length= %llu", gtid_event.get_trx_length()));

  bool ret = DBUG_EVALUATE_IF("simulate_write_trans_without_gtid", false,
                              gtid_event.write(writer));
  if (ret) goto end;

  /*
    finally write the transaction data, if it was not compressed
    and written as part of the gtid event already
  */
  ret = mysql_bin_log.write_cache(thd, cache_data, writer);

  if (!ret) {
    // update stats if monitoring is active
    binlog::global_context.monitoring_context()
        .transaction_compression()
        .update(binlog::monitoring::log_type::BINARY,
                cache_data->get_compression_type(), thd->owned_gtid,
                gtid_event.immediate_commit_timestamp,
                cache_data->get_compressed_size(),
                cache_data->get_decompressed_size());
  }

end:
  return ret;
}

int MYSQL_BIN_LOG::gtid_end_transaction(THD *thd) {
  DBUG_TRACE;

  DBUG_PRINT("info", ("query=%s", thd->query().str));

  if (thd->owned_gtid.sidno > 0) {
    assert(thd->variables.gtid_next.type == ASSIGNED_GTID);

    if (!opt_bin_log || (thd->slave_thread && !opt_log_replica_updates)) {
      /*
        If the binary log is disabled for this thread (either by
        log_bin=0 or sql_log_bin=0 or by log_replica_updates=0 for a
        slave thread), then the statement must not be written to the
        binary log.  In this case, we just save the GTID into the
        table directly.

        (This only happens for DDL, since DML will save the GTID into
        table and release ownership inside ha_commit_trans.)
      */
      if (gtid_state->save(thd) != 0) {
        gtid_state->update_on_rollback(thd);
        return 1;
      } else if (!has_commit_order_manager(thd)) {
        /*
          The gtid_state->save implicitly performs the commit, in the following
          stack:
            Gtid_state::save ->
            Gtid_table_persistor::save ->
            Gtid_table_access_context::deinit ->
            System_table_access::close_table ->
            ha_commit_trans ->
            Relay_log_info::pre_commit ->
            Slave_worker::commit_positions(THD*) ->
            Slave_worker::commit_positions(THD*,Log_event*,...) ->
            Slave_worker::flush_info ->
            Rpl_info_handler::flush_info ->
            Rpl_info_table::do_flush_info ->
            Rpl_info_table_access::close_table ->
            System_table_access::close_table ->
            ha_commit_trans ->
            MYSQL_BIN_LOG::commit ->
            ha_commit_low

          If replica-preserve-commit-order is disabled, it does not call
          update_on_commit from this stack. The reason is as follows:

          In the normal case of MYSQL_BIN_LOG::commit, where the transaction is
          going to be written to the binary log, it invokes
          MYSQL_BIN_LOG::ordered_commit, which updates the GTID state (the call
          gtid_state->update_commit_group(first) in process_commit_stage_queue).
          However, when MYSQL_BIN_LOG::commit is invoked from this stack, it is
          because the transaction is not going to be written to the binary log,
          and then MYSQL_BIN_LOG::commit has a special case that calls
          ha_commit_low directly, skipping ordered_commit. Therefore, the GTID
          state is not updated in this stack.

          On the other hand, if replica-preserve-commit-order is enabled, the
          logic that orders commit carries out a subset of the binlog group
          commit from within ha_commit_low, and this includes updating the GTID
          state. In particular, there is the following call stack under
          ha_commit_low:

            ha_commit_low ->
            Commit_order_manager::wait_and_finish ->
            Commit_order_manager::finish ->
            Commit_order_manager::flush_engine_and_signal_threads ->
            Gtid_state::update_commit_group

          Therefore, it is necessary to call update_on_commit only in case we
          are not using replica-preserve-commit-order here.
        */
        gtid_state->update_on_commit(thd);
      }
    } else {
      /*
        If statement is supposed to be written to binlog, we write it
        to the binary log.  Inserting into table and releasing
        ownership will be done in the binlog commit handler.
      */

      /*
        thd->cache_mngr may be uninitialized if the first transaction
        executed by the client is empty.
      */
      if (thd->binlog_setup_trx_data()) return 1;
      binlog_cache_data *cache_data = &thd_get_cache_mngr(thd)->trx_cache;

      // Generate BEGIN event
      Query_log_event qinfo(thd, STRING_WITH_LEN("BEGIN"), true, false, true, 0,
                            true);
      assert(!qinfo.is_using_immediate_logging());

      /*
        Write BEGIN event and then commit (which will generate commit
        event and Gtid_log_event)
      */
      DBUG_PRINT("debug", ("Writing to trx_cache"));
      if (cache_data->write_event(&qinfo) || mysql_bin_log.commit(thd, true))
        return 1;
    }
  } else if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS ||
             /*
               A transaction with an empty owned gtid should call
               end_gtid_violating_transaction(...) to clear the
               flag thd->has_gtid_consistency_violatoin in case
               it is set. It missed the clear in ordered_commit,
               because its binlog transaction cache is empty.
             */
             thd->has_gtid_consistency_violation)

  {
    gtid_state->update_on_commit(thd);
  } else if (thd->variables.gtid_next.type == ASSIGNED_GTID &&
             thd->owned_gtid_is_empty()) {
    assert(thd->has_gtid_consistency_violation == false);
    gtid_state->update_on_commit(thd);
  }

  return 0;
}

std::pair<std::list<std::string>, mysql::utils::Error>
MYSQL_BIN_LOG::get_filename_list() {
  return m_binlog_index_monitor.get_filename_list();
}

bool MYSQL_BIN_LOG::reencrypt_logs() {
  DBUG_TRACE;

  if (!is_open()) return false;

  std::string error_message;
  Log_info linfo;
  int error = 0;
  list<string>::reverse_iterator rit;

  /* Read binary/relay log file names from index file. */
  auto [filename_list, internal_error] = get_filename_list();
  if (internal_error.is_error() ||
      DBUG_EVALUATE_IF("fail_to_open_index_file", true, false)) {
    error_message.assign("I/O error reading index file '");
    error_message.append(m_binlog_index_monitor.get_index_fname());
    error_message.append("'");
    goto err;
  }

  rit = filename_list.rbegin();
  /* Skip the last binary/relay log. */
  if (rit != filename_list.rend()) rit++;
  /* Iterate backwards through binary/relay logs. */
  while (rit != filename_list.rend()) {
    const char *filename = rit->c_str();
    DBUG_EXECUTE_IF("purge_logs_during_reencryption", {
      purge_logs(filename, true, true /*need_lock_index=true*/,
                 true /*need_update_threads=true*/, nullptr, false);
    });
    MUTEX_LOCK(lock, m_binlog_index_monitor.get_index_lock());
    std::unique_ptr<Binlog_ofile> ofile(
        Binlog_ofile::open_existing(key_file_binlog, filename, MYF(MY_WME)));

    if (ofile == nullptr ||
        DBUG_EVALUATE_IF("fail_to_open_log_file", true, false) ||
        DBUG_EVALUATE_IF("fail_to_read_index_file", true, false)) {
      /* If we can not open the log file, check if it exists in index file. */
      error = find_log_pos(&linfo, filename, false);
      DBUG_EXECUTE_IF("fail_to_read_index_file", error = LOG_INFO_IO;);
      if (error == LOG_INFO_EOF) {
        /* If it does not exist in index file, re-encryption has finished. */
        if (current_thd->is_error()) current_thd->clear_error();
        break;
      } else if (error == 0) {
        /* If it exists in index file, failed to open the log file. */
        error_message.assign("Failed to open log file '");
        error_message.append(filename);
        error_message.append("'");
        goto err;
      } else if (error == LOG_INFO_IO) {
        /* Failed to read index file. */
        error_message.assign("I/O error reading index file '");
        error_message.append(m_binlog_index_monitor.get_index_fname());
        error_message.append("'");
        goto err;
      }
    }

    if (ofile->is_encrypted()) {
      std::unique_ptr<Truncatable_ostream> pipeline_head =
          ofile->get_pipeline_head();
      std::unique_ptr<Binlog_encryption_ostream> binlog_encryption_ostream(
          down_cast<Binlog_encryption_ostream *>(pipeline_head.release()));

      auto ret_value = binlog_encryption_ostream->reencrypt();
      if (ret_value.first) {
        error_message.assign("Failed to re-encrypt log file '");
        error_message.append(filename);
        error_message.append("': ");
        error_message.append(ret_value.second.c_str());
        goto err;
      }
    }

    rit++;
  }

  filename_list.clear();

  return false;

err:
  if (current_thd->is_error()) current_thd->clear_error();
  my_error(ER_BINLOG_MASTER_KEY_ROTATION_FAIL_TO_REENCRYPT_LOG, MYF(0),
           error_message.c_str());
  filename_list.clear();

  return true;
}

/// Controls the execution flow when we compress the transaction cache
/// into memory and write back the compressed data to the transaction
/// cache.
///
/// This is meant to be constructed once per transaction, and used
/// once to compress an existing transaction cache.  It relies on RAII
/// to perform final actions in the destructor, so it should normally
/// be constructed on the stack.
///
/// @todo move this to an own file. We need to declare
/// binlog_cache_data in a header file first.
class Binlog_cache_compressor {
 public:
  /// Construct a new Binlog_cache_compressor capable of compressing
  /// the given `binlog_cache_data` object.
  Binlog_cache_compressor(THD &thd, binlog_cache_data &cache)
      : m_thd(thd),
        m_cache(cache),
        m_cache_storage(*cache.get_cache()),
        m_context(thd.rpl_thd_ctx.transaction_compression_ctx()),
        m_managed_buffer_sequence(m_context.managed_buffer_sequence()),
        m_uncompressed_size(m_cache_storage.length()),
        m_compressed_size(m_uncompressed_size),
        m_compression_type(mysql::binlog::event::compression::type::NONE) {}

  Binlog_cache_compressor(const Binlog_cache_compressor &) = delete;
  Binlog_cache_compressor(Binlog_cache_compressor &&) = delete;
  Binlog_cache_compressor &operator=(const Binlog_cache_compressor &) = delete;
  Binlog_cache_compressor &operator=(Binlog_cache_compressor &&) = delete;
  ~Binlog_cache_compressor() {
    m_managed_buffer_sequence.reset();
    // Save statistics for
    // performance_schema.binary_log_transaction_compression_stats.
    m_cache.set_compression_type(m_compression_type);
    m_cache.set_compressed_size(m_compressed_size);
    m_cache.set_decompressed_size(m_uncompressed_size);
  }

  /// Attempt to compress the transaction cache.
  ///
  /// @retval false The transaction cache is either unchanged, or has
  /// been successfully replaced by the compressed transaction.
  ///
  /// @retval true The transaction cache has been corrupted,
  /// e.g. because an IO error occurred while replacing it, so the
  /// transaction has to abort.
  [[NODISCARD]] bool compress() {
    if (!shall_compress()) return false;
    if (setup_compressor()) return false;
    if (setup_buffer_sequence()) return false;
    if (compress_to_buffer_sequence()) return false;
    Transaction_payload_log_event tple{&m_thd};
    if (get_payload_event_from_buffer_sequence(tple)) return false;
    // Errors occurring above this point prevent us from compressing
    // the transaction, but allow us to fallback to
    // uncompressed. Hence we return false.  After this point, we
    // truncate the uncompressed cache.  Therefore, we can no longer
    // fallback to uncompressed. So we return true in the error case
    // below.
    if (overwrite_cache_with_payload_event(tple)) return true;
    return false;
  }

 private:
  /// Determine if compression should be attempted for the current
  /// transaction.
  ///
  /// @retval true compression should be attempted
  /// @retval false compression should not be attempted
  [[NODISCARD]] bool shall_compress() {
    DBUG_TRACE;
    // no compression enabled (ctype == NONE at this point)
    if (!m_thd.variables.binlog_trx_compression) {
      DBUG_PRINT("info", ("fallback to uncompressed: "
                          "binlog_transaction_compression disabled"));
      return false;
    }
    // do not compress if there are incident events
    DBUG_EXECUTE_IF("binlog_compression_inject_incident", {
      static int incident_count{0};
      incident_count++;
      if (incident_count == 1)
        m_cache.set_incident("Compression test incident");
    });
    if (m_cache.has_incident()) {
      DBUG_PRINT("info", ("fallback to uncompressed: has incident"));
      return false;
    }
    // do not compress if there are non-transactional changes
    if (m_thd.get_transaction()->has_modified_non_trans_table(
            Transaction_ctx::STMT) ||
        m_thd.get_transaction()->has_modified_non_trans_table(
            Transaction_ctx::SESSION)) {
      DBUG_PRINT("info",
                 ("fallback to uncompressed: has modified trans table"));
      return false;
    }
    // do not compress if has SBR
    if (m_cache.may_have_sbr_stmts()) {
      DBUG_PRINT("info", ("fallback to uncompressed: may have SBR events"));
      return false;
    }
    // nothing can stop us now!
    return true;
  }

  /// Get and configure the compressor; update m_compressor.
  /// Set the compression_level for m_compressor
  /// @return true on error, false on success.
  [[NODISCARD]] bool setup_compressor() {
    m_compressor = m_context.get_compressor(&m_thd);
    if (m_compressor == nullptr) {
      DBUG_PRINT("info", ("fallback to uncompressed: compressor==nullptr"));
      return true;
    }
    auto zstd_ptr =
        std::dynamic_pointer_cast<mysql::binlog::event::compression::Zstd_comp>(
            m_compressor);
    if (zstd_ptr) {
      zstd_ptr->set_compression_level(
          m_thd.variables.binlog_trx_compression_level_zstd);
    }
    // Allow compressor to optimize space usage based on uncompressed
    // size.
    m_compressor->set_pledged_input_size(m_uncompressed_size);
    return false;
  }

  /// Get and configure the Managed_buffer_sequence; update
  /// m_managed_buffer_sequence.
  ///
  /// @return true on error, false on success.
  [[NODISCARD]] bool setup_buffer_sequence() {
    mysql::containers::buffers::Grow_calculator grow_calculator;
    grow_calculator.set_max_size(
        mysql::binlog::event::Transaction_payload_event::max_payload_length);
    DBUG_EXECUTE_IF("binlog_transaction_compression_max_size_800",
                    { grow_calculator.set_max_size(800); });
    grow_calculator.set_grow_factor(2);
    grow_calculator.set_grow_increment(8192);
    auto compressor_grow_constraint = m_compressor->get_grow_constraint_hint();
    grow_calculator = compressor_grow_constraint.combine_with(grow_calculator);
    m_managed_buffer_sequence.set_grow_calculator(grow_calculator);
    return false;
  }

  /// Compress the transaction cache using the compressor, and and
  /// store the output in the Managed_buffer_sequence.
  ///
  /// @return true on error, false on success.
  [[NODISCARD]] bool compress_to_buffer_sequence() {
    Compressed_ostream stream{m_compressor, m_managed_buffer_sequence};

    THD_STAGE_GUARD(&m_thd, stage_binlog_transaction_compress);

    if (m_cache_storage.copy_to(&stream)) {
      DBUG_PRINT("info", ("fallback to uncompressed: compression failed during "
                          "Compressor::compress"));
      m_compressor->reset();
      return true;
    }
    if (m_compressor->finish(m_managed_buffer_sequence) !=
        mysql::binlog::event::compression::Compress_status::success) {
      m_compressor->reset();
      DBUG_PRINT("info", ("fallback to uncompressed: compression failed during "
                          "Compressor::finish"));
      return true;
    }

    m_compressed_size = m_managed_buffer_sequence.read_part().size();
    m_compression_type = m_compressor->get_type_code();

    return false;
  }

  /// Populate the given Transaction_payload_log_event with compressed
  /// data.
  ///
  /// @return true on error, false on success.
  [[NODISCARD]] bool get_payload_event_from_buffer_sequence(
      Transaction_payload_log_event &tple) {
    tple.set_payload(&m_managed_buffer_sequence.read_part());
    tple.set_compression_type(m_compression_type);
    tple.set_uncompressed_size(m_uncompressed_size);
    tple.set_payload_size(m_compressed_size);
    return false;
  }

  /// Truncate the transaction cache and write the
  /// Transaction_payload_log_event there instead.
  ///
  /// @todo the argument should be const, and then all the functions
  /// down the stack should use const Log_event too (including
  /// Log_event::write)
  ///
  /// @return true on error, false on success.
  [[NODISCARD]] bool overwrite_cache_with_payload_event(
      Transaction_payload_log_event &tple) {
    // Truncate cache file
    if (m_cache_storage.truncate(0)) {
      DBUG_PRINT("info", ("fail: m_cache_storage.truncate failed"));
      return true;
    }
    // Since we deleted all events from the cache, we also need to
    // reset event_counter.
    m_cache.set_event_counter(0);

    // write back the new cache contents
    if (m_cache.write_event(&tple) != 0) {
      DBUG_PRINT("info", ("fail: write_event failed"));
      return true;
    }
    return false;
  }
  /// Session context.
  THD &m_thd;
  /// Transaction cache.
  binlog_cache_data &m_cache;
  /// Storage for the transaction cache.
  Binlog_cache_storage &m_cache_storage;
  /// Session compression context.
  Transaction_compression_ctx &m_context;

  /// Compressor.
  Transaction_compression_ctx::Compressor_ptr_t m_compressor;
  /// Output buffer.
  Transaction_compression_ctx::Managed_buffer_sequence_t
      &m_managed_buffer_sequence;

  /// Size before compression.
  size_t m_uncompressed_size;
  /// Size after compression, if compression succeeded. Otherwise,
  /// size before compression.
  size_t m_compressed_size;
  /// Compression algorithm, if compression succeded; otherwise NONE.
  mysql::binlog::event::compression::type m_compression_type;
};

bool binlog_cache_data::compress(THD *thd) {
  Binlog_cache_compressor binlog_cache_compressor(*thd, *this);
  return binlog_cache_compressor.compress();
}

/**
  This function finalizes the cache preparing for commit or rollback.

  The function just writes all the necessary events to the cache but
  does not flush the data to the binary log file. That is the role of
  the binlog_cache_data::flush function.

  @see binlog_cache_data::flush

  @param thd                The thread whose transaction should be flushed
  @param end_event          The end event either commit/rollback

  @return
    nonzero if an error pops up when flushing the cache.
*/
int binlog_cache_data::finalize(THD *thd, Log_event *end_event) {
  DBUG_TRACE;
  if (!is_binlog_empty()) {
    assert(!flags.finalized);
    if (int error = flush_pending_event(thd)) return error;
    if (int error = write_event(end_event)) return error;
    if (int error = this->compress(thd)) return error;
    DBUG_PRINT("debug", ("flags.finalized: %s", YESNO(flags.finalized)));
    flags.finalized = true;
  }
  return 0;
}

/**
   The method writes XA END query to XA-prepared transaction's cache
   and calls the "basic" finalize().

   @return error code, 0 success
*/

int binlog_cache_data::finalize(THD *thd, Log_event *end_event, XID_STATE *xs) {
  int error = 0;
  char buf[XID::ser_buf_size];
  char query[sizeof("XA END") + 1 + sizeof(buf)];
  int qlen = sprintf(query, "XA END %s", xs->get_xid()->serialize(buf));
  Query_log_event qev(thd, query, qlen, true, false, true, 0);

  if ((error = write_event(&qev))) return error;

  return finalize(thd, end_event);
}

///
///   Handle previous failure writing cache, aka. incident.
///
///   Failure writing to the cache in a statement which can't be rolled-back is
///   handled by setting the 'incident' flag on the cache. In this way the
///   error is deferred and handled later by calling this function which
///   replaces the incomplete cache content with an Incident_log_event.
///
///   It's also possible for any session to mark the cache as having an incident
///   and thus trigger this functionality. The main user of this is the NDB
///   binlog which need to write incident(s) when the NDB connection state
///   changes or when other fatal error occurs,
///   @@see BINLOG::write_incident_commit.
///
///   @param thd Thread variable
///
///   @retval 0 Success.
///   @retval non-zero Error occured writing to cache
///
///   @note This function handles previous error while writing to the cache by
///   attempting to write (something hopefully smaller) to the cache and thus it
///   may also fail, but at least an attempt has been made to get something
///   sensible into the cache. Returning error invokes binlog_error_action.
///
int binlog_cache_mngr::handle_deferred_cache_write_incident(THD *thd) {
  DBUG_TRACE;
  assert(has_incident());
  /// Construct the Incident_log_event before reset()
  Incident_log_event incident_log_ev(
      thd, mysql::binlog::event::Incident_event::INCIDENT_LOST_EVENTS,
      m_incident);
  // Reset BOTH caches and clear the incident
  stmt_cache.reset();
  trx_cache.reset();
  m_incident.clear();
  if (int error = stmt_cache.write_event(&incident_log_ev)) return error;
  if (int error = stmt_cache.finalize(thd, nullptr)) return error;

  if (has_incident()) {
    stmt_cache.reset();
    trx_cache.reset();
    // Failure to write cache deferred again -> return error
    return 1;
  }

  LogErr(ERROR_LEVEL, ER_BINLOG_LOGGING_INCIDENT_TO_STOP_REPLICAS,
         incident_log_ev.get_message());

  assert(!has_incident());
  assert(stmt_cache.is_finalized());

  return 0;
}

/**
  Flush caches to the binary log.

  If the cache is finalized, the cache will be flushed to the binary
  log file. If the cache is not finalized, nothing will be done.

  If flushing fails for any reason, an error will be reported and the
  cache will be reset. Flushing can fail in the following circumstances:

  - It was not possible to write the cache to the file. In this case,
    it does not make sense to keep the cache.

  @see binlog_cache_data::finalize
 */
int binlog_cache_data::flush(THD *thd, my_off_t *bytes_written, bool *wrote_xid,
                             bool parallelization_barrier) {
  /*
    Doing a commit or a rollback including non-transactional tables,
    i.e., ending a transaction where we might write the transaction
    cache to the binary log.

    We can always end the statement when ending a transaction since
    transactions are not allowed inside stored functions. If they
    were, we would have to ensure that we're not ending a statement
    inside a stored function.
  */
  DBUG_TRACE;
  DBUG_PRINT("debug", ("flags.finalized: %s", YESNO(flags.finalized)));
  int error = 0;
  if (flags.finalized) {
    my_off_t bytes_in_cache = m_cache.length();
    Transaction_ctx *trn_ctx = thd->get_transaction();

    DBUG_PRINT("debug", ("bytes_in_cache: %llu", bytes_in_cache));

    trn_ctx->sequence_number = mysql_bin_log.m_dependency_tracker.step();

    /*
      In case of two caches the transaction is split into two groups.
      The 2nd group is considered to be a successor of the 1st rather
      than to have a common commit parent with it.
      Notice that due to a simple method of detection that the current is
      the 2nd cache being flushed, the very first few transactions may be logged
      sequentially (a next one is tagged as if a preceding one is its
      commit parent).
    */
    if (trn_ctx->last_committed == SEQ_UNINIT)
      trn_ctx->last_committed = trn_ctx->sequence_number - 1;

    /*
      The GTID is written prior to flushing the statement cache, if
      the transaction has written to the statement cache; and prior to
      flushing the transaction cache if the transaction has written to
      the transaction cache.  If GTIDs are enabled, then transactional
      and non-transactional updates cannot be mixed, so at most one of
      the caches can be non-empty, so just one GTID will be
      generated. If GTIDs are disabled, then no GTID is generated at
      all; if both the transactional cache and the statement cache are
      non-empty then we get two Anonymous_gtid_log_events, which is
      correct.
    */
    Binlog_event_writer writer(mysql_bin_log.get_binlog_file());

    /* The GTID ownership process might set the commit_error */
    error = (thd->commit_error == THD::CE_FLUSH_ERROR ||
             thd->commit_error == THD::CE_FLUSH_GNO_EXHAUSTED_ERROR);

    DBUG_EXECUTE_IF("simulate_binlog_flush_error", {
      if (rand() % 3 == 0) {
        thd->commit_error = THD::CE_FLUSH_ERROR;
      }
    };);

    DBUG_EXECUTE_IF("fault_injection_reinit_io_cache_while_flushing_to_file",
                    { DBUG_SET("+d,fault_injection_reinit_io_cache"); });

    if (!error)
      if ((error = mysql_bin_log.write_transaction(thd, this, &writer,
                                                   parallelization_barrier)))
        thd->commit_error = THD::CE_FLUSH_ERROR;

    DBUG_EXECUTE_IF("fault_injection_reinit_io_cache_while_flushing_to_file",
                    { DBUG_SET("-d,fault_injection_reinit_io_cache"); });

    if (flags.with_xid && error == 0) *wrote_xid = true;

    /*
      Reset have to be after the if above, since it clears the
      with_xid flag
    */
    reset();
    if (bytes_written) *bytes_written = bytes_in_cache;
  }
  assert(!flags.finalized);
  return error;
}

/**
  This function truncates the transactional cache upon committing or rolling
  back either a transaction or a statement.

  @param thd        The thread whose transaction should be flushed
  @param all        @c true means truncate the transaction, otherwise the
                    statement must be truncated.

*/
void binlog_trx_cache_data::truncate(THD *thd, bool all) {
  DBUG_TRACE;

  DBUG_PRINT("info",
             ("thd->options={ %s %s}, transaction: %s",
              FLAGSTR(thd->variables.option_bits, OPTION_NOT_AUTOCOMMIT),
              FLAGSTR(thd->variables.option_bits, OPTION_BEGIN),
              all ? "all" : "stmt"));

  remove_pending_event();

  /*
    If rolling back an entire transaction or a single statement not
    inside a transaction, we reset the transaction cache.
    Even though formally the atomic DDL statement may not end multi-statement
    transaction the cache needs full resetting as there must
    be no other data in it but belonging to the DDL.
  */
  if (ending_trans(thd, all)) {
    reset();
  }
  /*
    If rolling back a statement in a transaction, we truncate the
    transaction cache to remove the statement.
  */
  else if (get_prev_position() != MY_OFF_T_UNDEF)
    restore_prev_position();

  thd->clear_binlog_table_maps();
}

inline enum xa_option_words get_xa_opt(THD *thd) {
  enum xa_option_words xa_opt = XA_NONE;
  switch (thd->lex->sql_command) {
    case SQLCOM_XA_COMMIT:
      xa_opt =
          static_cast<Sql_cmd_xa_commit *>(thd->lex->m_sql_cmd)->get_xa_opt();
      break;
    default:
      break;
  }

  return xa_opt;
}

/**
   Predicate function yields true when XA transaction is
   being logged having a proper state ready for prepare or
   commit in one phase.

   @param thd    THD pointer of running transaction
   @return true  When the being prepared transaction should be binlogged,
           false otherwise.
*/

inline bool is_loggable_xa_prepare(THD *thd) {
  /*
    simulate_commit_failure is doing a trick with XID_STATE while
    the ongoing transaction is not XA, and therefore to be errored out,
    asserted below. In that case because of the
    latter fact the function returns @c false.
  */
  DBUG_EXECUTE_IF("simulate_commit_failure", {
    XID_STATE *xs = thd->get_transaction()->xid_state();
    assert((thd->is_error() && xs->get_state() == XID_STATE::XA_IDLE) ||
           xs->get_state() == XID_STATE::XA_NOTR);
  });

  return DBUG_EVALUATE_IF(
      "simulate_commit_failure", false,
      thd->get_transaction()->xid_state()->has_state(XID_STATE::XA_IDLE));
}

static int binlog_prepare(handlerton *, THD *thd, bool all) {
  DBUG_TRACE;
  if (!all) {
    thd->get_transaction()->store_commit_parent(
        mysql_bin_log.m_dependency_tracker.get_max_committed_timestamp());
  }
  return 0;
}

static int binlog_set_prepared_in_tc(handlerton *, THD *) { return 0; }

int MYSQL_BIN_LOG::write_xa_to_cache(THD *thd) {
  assert(thd->lex->sql_command == SQLCOM_XA_COMMIT ||
         thd->lex->sql_command == SQLCOM_XA_ROLLBACK);

  if (get_xa_opt(thd) == XA_ONE_PHASE) return 0;

  auto xid_state = thd->get_transaction()->xid_state();
  if (!xid_state->is_binlogged())
    return 0;  // nothing was really logged at prepare

  if (thd->is_error() && DBUG_EVALUATE_IF("simulate_xa_rm_error", 0, 1))
    return 0;  // don't binlog if there are some errors.

  auto xid_to_write = xid_state->get_xid();
  assert(xid_to_write != nullptr);
  assert(!xid_to_write->is_null() ||
         !(thd->variables.option_bits & OPTION_BIN_LOG));

  std::ostringstream oss;
  oss << "XA "
      << (thd->lex->sql_command == SQLCOM_XA_COMMIT ? "COMMIT" : "ROLLBACK")
      << " " << *xid_to_write << std::flush;
  auto query = oss.str();
  Query_log_event qinfo(thd, query.data(), query.length(), false, true, true, 0,
                        false);
  return this->write_event(&qinfo);
}

bool MYSQL_BIN_LOG::write_incident_commit(THD *thd,
                                          std::string_view incident_message) {
  DBUG_TRACE;
  if (incident_message.empty()) return true;

  if (thd->binlog_setup_trx_data()) return true;

  binlog_cache_data *const cache = &thd_get_cache_mngr(thd)->trx_cache;

  // Mark the transaction as having an incident
  cache->set_incident(incident_message);

  // Write a dummy event to form the transaction which will be committed, this
  // event will never end up in the binlog since cache is marked as having an
  // incident and thus it will be replaced with an Incident_log_event
  Query_log_event qinfo(thd, STRING_WITH_LEN("INCIDENT"), true, false, true, 0,
                        true);
  if (cache->write_event(&qinfo) || commit(thd, true)) return true;

  return false;
}

/**
  This function is called once after each statement.

  @todo This function is currently not used any more and will
  eventually be eliminated. The real commit job is done in the
  MYSQL_BIN_LOG::commit function.

  @see MYSQL_BIN_LOG::commit

  @see handlerton::commit
*/
static int binlog_commit(handlerton *, THD *, bool) {
  DBUG_TRACE;
  /*
    Nothing to do (any more) on commit.
   */
  return 0;
}

/**
  This function is called when a transaction or a statement is rolled back.

  @internal It is necessary to execute a rollback here if the
  transaction was rolled back because of executing a ROLLBACK TO
  SAVEPOINT command, but it is not used for normal rollback since
  MYSQL_BIN_LOG::rollback is called in that case.

  @todo Refactor code to introduce a <code>MYSQL_BIN_LOG::rollback(THD
  *thd, SAVEPOINT *sv)</code> function in @c TC_LOG and have that
  function execute the necessary work to rollback to a savepoint.

  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction rollback, and
               @false otherwise.

  @see handlerton::rollback
*/
static int binlog_rollback(handlerton *, THD *thd, bool all) {
  DBUG_TRACE;
  int error = 0;
  if (thd->lex->sql_command == SQLCOM_ROLLBACK_TO_SAVEPOINT)
    error = mysql_bin_log.rollback(thd, all);
  return error;
}

/**
  Write a rollback record of the transaction to the binary log.

  For binary log group commit, the rollback is separated into three
  parts:

  1. First part consists of filling the necessary caches and
     finalizing them (if they need to be finalized). After a cache is
     finalized, nothing can be added to the cache.

  2. Second part execute an ordered flush and commit. This will be
     done using the group commit functionality in @c ordered_commit.

     Since we roll back the transaction early, we call @c
     ordered_commit with the @c skip_commit flag set. The @c
     ha_commit_low call inside @c ordered_commit will then not be
     called.

  3. Third part checks any errors resulting from the flush and handles
     them appropriately.

  @see MYSQL_BIN_LOG::ordered_commit
  @see ha_commit_low
  @see ha_rollback_low

  @param thd Session to commit
  @param all This is @c true if this is a real transaction rollback, and
             @c false otherwise.

  @return Error code, or zero if there were no error.
 */

int MYSQL_BIN_LOG::rollback(THD *thd, bool all) {
  int error = 0;
  bool stuff_logged = false;
  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
  bool is_empty = false;

  DBUG_TRACE;
  DBUG_PRINT("enter",
             ("all: %s, cache_mngr: 0x%llx, thd->is_error: %s", YESNO(all),
              (ulonglong)cache_mngr, YESNO(thd->is_error())));
  /*
    Defer XA-transaction rollback until its XA-rollback event is recorded.
    When we are executing a ROLLBACK TO SAVEPOINT, we
    should only clear the caches since this function is called as part
    of the engine rollback.
    In other cases we roll back the transaction in the engines early
    since this will release locks and allow other transactions to
    start executing.
  */
  if (is_xa_rollback(thd)) {
    auto xs = thd->get_transaction()->xid_state();

    assert(all || !xs->is_binlogged() ||
           (!xs->is_detached() && thd->is_error()));

    is_empty = !xs->is_binlogged();

    if ((error = this->write_xa_to_cache(thd)) != 0) goto end;

    cache_mngr = thd_get_cache_mngr(thd);
  } else if (thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT)
    if ((error = trx_coordinator::rollback_in_engines(thd, all))) goto end;

  /*
    If there is no cache manager, or if there is nothing in the
    caches, there are no caches to roll back, so we're trivially done
    unless XA-ROLLBACK that yet to run rollback_low().
  */
  if (cache_mngr == nullptr || cache_mngr->is_binlog_empty()) {
    goto end;
  }

  DBUG_PRINT("debug", ("all.cannot_safely_rollback(): %s, trx_cache_empty: %s",
                       YESNO(thd->get_transaction()->cannot_safely_rollback(
                           Transaction_ctx::SESSION)),
                       YESNO(cache_mngr->trx_cache.is_binlog_empty())));
  DBUG_PRINT("debug",
             ("stmt.cannot_safely_rollback(): %s, stmt_cache_empty: %s",
              YESNO(thd->get_transaction()->cannot_safely_rollback(
                  Transaction_ctx::STMT)),
              YESNO(cache_mngr->stmt_cache.is_binlog_empty())));

  if (!cache_mngr->stmt_cache.has_incident() &&
      !cache_mngr->stmt_cache.is_binlog_empty()) {
    if (thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
        !thd->lex->query_block->field_list_is_empty() && /* With select */
        !(thd->lex->create_info->options & HA_LEX_CREATE_TMP_TABLE) &&
        thd->is_current_stmt_binlog_format_row()) {
      /*
        In row based binlog format, we reset the binlog statement cache
        when rolling back a single statement 'CREATE...SELECT' transaction,
        since the 'CREATE TABLE' event was put in the binlog statement cache.
      */
      cache_mngr->stmt_cache.reset();
    } else {
      if ((error = cache_mngr->stmt_cache.finalize(thd))) goto end;
      stuff_logged = true;
    }
  }

  if (ending_trans(thd, all)) {
    if (trans_cannot_safely_rollback(thd)) {
      auto xs = thd->get_transaction()->xid_state();
      std::string query{"ROLLBACK"};

      if (is_xa_rollback(thd)) {
        /* this block is relevant only for not prepared yet and "local" xa trx
         */
        assert(
            thd->get_transaction()->xid_state()->has_state(XID_STATE::XA_IDLE));

        std::ostringstream oss;
        oss << "XA ROLLBACK " << *xs->get_xid() << std::flush;
        query = oss.str();
      }
      /*
        If the transaction is being rolled back and contains changes that
        cannot be rolled back, the trx-cache's content is flushed.
      */
      Query_log_event end_evt(thd, query.data(), query.length(), true, false,
                              true, 0, true);
      error = thd->lex->sql_command != SQLCOM_XA_ROLLBACK
                  ? cache_mngr->trx_cache.finalize(thd, &end_evt)
                  : cache_mngr->trx_cache.finalize(thd, &end_evt, xs);
      stuff_logged = true;
    } else {
      /*
        If the transaction is being rolled back and its changes can be
        rolled back, the trx-cache's content is truncated.
      */
      cache_mngr->trx_cache.truncate(thd, all);

      DBUG_EXECUTE_IF("ensure_binlog_cache_is_reset", {
        /* Assert that binlog cache is reset at rollback time. */
        assert(binlog_cache_is_reset);
        binlog_cache_is_reset = false;
      };);
    }
  } else {
    /*
      If a statement is being rolled back, it is necessary to know
      exactly why a statement may not be safely rolled back as in
      some specific situations the trx-cache can be truncated.

      If a temporary table is created or dropped, the trx-cache is not
      truncated. Note that if the stmt-cache is used, there is nothing
      to truncate in the trx-cache.

      If a non-transactional table is updated and the binlog format is
      statement, the trx-cache is not truncated. The trx-cache is used
      when the direct option is off and a transactional table has been
      updated before the current statement in the context of the
      current transaction. Note that if the stmt-cache is used there is
      nothing to truncate in the trx-cache.

      If other binlog formats are used, updates to non-transactional
      tables are written to the stmt-cache and trx-cache can be safely
      truncated, if necessary.
    */
    if (thd->get_transaction()->has_dropped_temp_table(Transaction_ctx::STMT) ||
        thd->get_transaction()->has_created_temp_table(Transaction_ctx::STMT) ||
        (thd->get_transaction()->has_modified_non_trans_table(
             Transaction_ctx::STMT) &&
         thd->variables.binlog_format == BINLOG_FORMAT_STMT)) {
      /*
        If the statement is being rolled back and dropped or created a
        temporary table or modified a non-transactional table and the
        statement-based replication is in use, the statement's changes
        in the trx-cache are preserved.
      */
      cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);
    } else {
      /*
        Otherwise, the statement's changes in the trx-cache are
        truncated.
      */
      cache_mngr->trx_cache.truncate(thd, all);
    }
  }
  if (stuff_logged) {
    Transaction_ctx *trn_ctx = thd->get_transaction();
    trn_ctx->store_commit_parent(
        m_dependency_tracker.get_max_committed_timestamp());
  }

  DBUG_PRINT("debug", ("error: %d", error));
  if ((error == 0 && stuff_logged) || cache_mngr->has_incident()) {
    CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_invoke_before_commit_hook");
    if (RUN_HOOK(
            transaction, before_commit,
            (thd, all, thd_get_cache_mngr(thd)->get_trx_cache(),
             thd_get_cache_mngr(thd)->get_stmt_cache(),
             max<my_off_t>(max_binlog_cache_size, max_binlog_stmt_cache_size),
             false))) {
      // Reset the thread OK status before changing the outcome.
      if (thd->get_stmt_da()->is_ok())
        thd->get_stmt_da()->reset_diagnostics_area();
      my_error(ER_RUN_HOOK_ERROR, MYF(0), "before_commit");
      return RESULT_ABORTED;
    }
    // XA rollback is always accepted.
    assert(!thd->get_transaction()
                ->get_rpl_transaction_ctx()
                ->is_transaction_rollback());

    error = ordered_commit(thd, all, /* skip_commit */ true);

    // Inform hook listeners that a XA ROLLBACK did commit, that
    // is, did log a transaction to the binary log.
    if (!error && is_xa_rollback(thd))
      (void)RUN_HOOK(transaction, after_commit, (thd, all));
  }

  /*
    It should be impossible to have an incident here as all sessions with
    incident will call ordered_commit() and handle the incident during
    BGC. If that fails, error is handled by handle_binlog_flush_or_sync_error().
   */
  assert(!cache_mngr->has_incident());

  if (check_write_error(thd)) {
    /*
      We reach this point if the effect of a statement did not properly get into
      a cache and need to be rolled back.
    */
    cache_mngr->trx_cache.truncate(thd, all);
  }

end:
  // The caches may be empty if an `XA ROLLBACK` was issued just after `XA
  // END`. In that case, the BCG will not be invoked and we need to
  // rollback in SEs and finalize GTID state.
  if (!error && !stuff_logged && is_xa_rollback(thd)) {
    error = trx_coordinator::rollback_in_engines(thd, all);
    if (!error && !thd->is_error()) {
      /*
        XA-rollback ignores the gtid_state, if the transaciton
        is empty.
      */
      if (is_empty && !thd->slave_thread) gtid_state->update_on_rollback(thd);
      /*
        XA-rollback commits the new gtid_state, if transaction
        is not empty.
      */
      else {
        gtid_state->update_on_commit(thd);
        /*
          Inform hook listeners that a XA ROLLBACK did commit, that
          is, did log a transaction to the binary log.
        */
        (void)RUN_HOOK(transaction, after_commit, (thd, all));
      }
    }
  }
  /*
    When a statement errors out on auto-commit mode it is rollback
    implicitly, so the same should happen to its GTID.
  */
  if (!thd->in_active_multi_stmt_transaction())
    gtid_state->update_on_rollback(thd);

  /*
    TODO: some errors are overwritten, which may cause problem,
    fix it later.
  */
  DBUG_PRINT("return", ("error: %d", error));
  return error;
}

/**
  @note
  How do we handle this (unlikely but legal) case:
  @verbatim
    [transaction] + [update to non-trans table] + [rollback to savepoint] ?
  @endverbatim
  The problem occurs when a savepoint is before the update to the
  non-transactional table. Then when there's a rollback to the savepoint, if we
  simply truncate the binlog cache, we lose the part of the binlog cache where
  the update is. If we want to not lose it, we need to write the SAVEPOINT
  command and the ROLLBACK TO SAVEPOINT command to the binlog cache. The latter
  is easy: it's just write at the end of the binlog cache, but the former
  should be *inserted* to the place where the user called SAVEPOINT. The
  solution is that when the user calls SAVEPOINT, we write it to the binlog
  cache (so no need to later insert it). As transactions are never intermixed
  in the binary log (i.e. they are serialized), we won't have conflicts with
  savepoint names when using mysqlbinlog or in the slave SQL thread.
  Then when ROLLBACK TO SAVEPOINT is called, if we updated some
  non-transactional table, we don't truncate the binlog cache but instead write
  ROLLBACK TO SAVEPOINT to it; otherwise we truncate the binlog cache (which
  will chop the SAVEPOINT command from the binlog cache, which is good as in
  that case there is no need to have it in the binlog).
*/

static int binlog_savepoint_set(handlerton *, THD *thd, void *sv) {
  DBUG_TRACE;
  int error = 1;

  String log_query;
  if (log_query.append(STRING_WITH_LEN("SAVEPOINT ")))
    return error;
  else
    append_identifier(thd, &log_query, thd->lex->ident.str,
                      thd->lex->ident.length);

  int errcode = query_error_code(thd, thd->killed == THD::NOT_KILLED);
  Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(), true,
                        false, true, errcode);
  /*
    We cannot record the position before writing the statement
    because a rollback to a savepoint (.e.g. consider it "S") would
    prevent the savepoint statement (i.e. "SAVEPOINT S") from being
    written to the binary log despite the fact that the server could
    still issue other rollback statements to the same savepoint (i.e.
    "S").
    Given that the savepoint is valid until the server releases it,
    ie, until the transaction commits or it is released explicitly,
    we need to log it anyway so that we don't have "ROLLBACK TO S"
    or "RELEASE S" without the preceding "SAVEPOINT S" in the binary
    log.
  */
  if (!(error = mysql_bin_log.write_event(&qinfo)))
    binlog_trans_log_savepos(thd, (my_off_t *)sv);

  return error;
}

bool MYSQL_BIN_LOG::is_current_stmt_binlog_enabled_and_caches_empty(
    const THD *thd) const {
  DBUG_TRACE;
  if (!(thd->variables.option_bits & OPTION_BIN_LOG) ||
      !mysql_bin_log.is_open()) {
    // thd_get_cache_mngr requires binlog to option to be enabled
    return false;
  }
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
  if (cache_mngr == nullptr) {
    return true;
  }
  return cache_mngr->is_binlog_empty();
}

static int binlog_savepoint_rollback(handlerton *, THD *thd, void *sv) {
  DBUG_TRACE;
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
  my_off_t pos = *(my_off_t *)sv;
  assert(pos != ~(my_off_t)0);

  /*
    Write ROLLBACK TO SAVEPOINT to the binlog cache if we have updated some
    non-transactional table. Otherwise, truncate the binlog cache starting
    from the SAVEPOINT command.
  */
  if (trans_cannot_safely_rollback(thd)) {
    String log_query;
    if (log_query.append(STRING_WITH_LEN("ROLLBACK TO ")))
      return 1;
    else {
      /*
        Before writing identifier to the binlog, make sure to
        quote the identifier properly so as to prevent any SQL
        injection on the slave.
      */
      append_identifier(thd, &log_query, thd->lex->ident.str,
                        thd->lex->ident.length);
    }

    int errcode = query_error_code(thd, thd->killed == THD::NOT_KILLED);
    Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(), true,
                          false, true, errcode);
    return mysql_bin_log.write_event(&qinfo);
  }
  // Otherwise, we truncate the cache
  cache_mngr->trx_cache.restore_savepoint(pos);
  /*
    When a SAVEPOINT is executed inside a stored function/trigger we force the
    pending event to be flushed with a STMT_END_F flag and clear the table maps
    as well to ensure that following DMLs will have a clean state to start
    with. ROLLBACK inside a stored routine has to finalize possibly existing
    current row-based pending event with cleaning up table maps. That ensures
    that following DMLs will have a clean state to start with.
   */
  if (thd->in_sub_stmt) thd->clear_binlog_table_maps();
  return 0;
}

/**
   purge logs, master and slave sides both, related error code
   converter.
   Called from @c purge_error_message(), @c MYSQL_BIN_LOG::reset_logs()

   @param  res  an error code as used by purging routines

   @return the user level error code ER_*
*/
static uint purge_log_get_error_code(int res) {
  uint errcode = 0;

  switch (res) {
    case 0:
      break;
    case LOG_INFO_EOF:
      errcode = ER_UNKNOWN_TARGET_BINLOG;
      break;
    case LOG_INFO_IO:
      errcode = ER_IO_ERR_LOG_INDEX_READ;
      break;
    case LOG_INFO_INVALID:
      errcode = ER_BINLOG_PURGE_PROHIBITED;
      break;
    case LOG_INFO_SEEK:
      errcode = ER_FSEEK_FAIL;
      break;
    case LOG_INFO_MEM:
      errcode = ER_OUT_OF_RESOURCES;
      break;
    case LOG_INFO_FATAL:
      errcode = ER_BINLOG_PURGE_FATAL_ERR;
      break;
    case LOG_INFO_IN_USE:
      errcode = ER_LOG_IN_USE;
      break;
    case LOG_INFO_EMFILE:
      errcode = ER_BINLOG_PURGE_EMFILE;
      break;
    case LOG_INFO_BACKUP_LOCK:
      errcode = ER_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK;
      break;
    default:
      errcode = ER_LOG_PURGE_UNKNOWN_ERR;
      break;
  }

  return errcode;
}

/**
  Check whether binlog state allows to safely release MDL locks after
  rollback to savepoint.

  @param thd   The client thread that executes the transaction.

  @return true  - It is safe to release MDL locks.
          false - If it is not.
*/
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *, THD *thd) {
  DBUG_TRACE;
  /**
    If we have not updated any non-transactional tables rollback
    to savepoint will simply truncate binlog cache starting from
    SAVEPOINT command. So it should be safe to release MDL acquired
    after SAVEPOINT command in this case.
  */
  return !trans_cannot_safely_rollback(thd);
}

static bool purge_error_message(THD *thd, int res) {
  uint errcode;

  if ((errcode = purge_log_get_error_code(res)) != 0) {
    my_error(errcode, MYF(0));
    return true;
  }
  my_ok(thd);
  return false;
}

bool is_transaction_empty(THD *thd) {
  DBUG_TRACE;
  int rw_ha_count = check_trx_rw_engines(thd, Transaction_ctx::SESSION);
  rw_ha_count += check_trx_rw_engines(thd, Transaction_ctx::STMT);
  return rw_ha_count == 0;
}

int check_trx_rw_engines(THD *thd, Transaction_ctx::enum_trx_scope trx_scope) {
  DBUG_TRACE;

  int rw_ha_count = 0;
  auto ha_list = thd->get_transaction()->ha_trx_info(trx_scope);

  for (auto const &ha_info : ha_list) {
    if (ha_info.is_trx_read_write()) ++rw_ha_count;
  }
  return rw_ha_count;
}

bool is_empty_transaction_in_binlog_cache(const THD *thd) {
  DBUG_TRACE;

  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
  if (cache_mngr != nullptr && cache_mngr->has_empty_transaction()) {
    return true;
  }

  return false;
}

/**
  This function checks if a transactional table was updated by the
  current transaction.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool trans_has_updated_trans_table(const THD *thd) {
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);

  return (cache_mngr ? !cache_mngr->trx_cache.is_binlog_empty() : 0);
}

/**
  This function checks if a transactional table was updated by the
  current statement.

  @param ha_list Registered storage engine handler list.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool stmt_has_updated_trans_table(Ha_trx_info_list const &ha_list) {
  for (auto const &ha_info : ha_list) {
    if (ha_info.is_trx_read_write() && ha_info.ht() != binlog_hton)
      return (true);
  }
  return (false);
}

/**
  This function checks if a transaction, either a multi-statement
  or a single statement transaction is about to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. true) or a statement
             (i.e. false).
  @return
    @c true if committing a transaction, otherwise @c false.
*/
bool ending_trans(THD *thd, const bool all) {
  return (all || ending_single_stmt_trans(thd, all));
}

/**
  This function checks if a single statement transaction is about
  to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. true) or a statement
             (i.e. false).
  @return
    @c true if committing a single statement transaction, otherwise
    @c false.
*/
bool ending_single_stmt_trans(THD *thd, const bool all) {
  return (!all && !thd->in_multi_stmt_transaction_mode());
}

/**
  This function checks if a transaction cannot be rolled back safely.

  @param thd The client thread that executed the current statement.
  @return
    @c true if cannot be safely rolled back, @c false otherwise.
*/
bool trans_cannot_safely_rollback(const THD *thd) {
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);

  return cache_mngr->trx_cache.cannot_rollback();
}

/**
  This function checks if current statement cannot be rollded back safely.

  @param thd The client thread that executed the current statement.
  @return
    @c true if cannot be safely rolled back, @c false otherwise.
*/
bool stmt_cannot_safely_rollback(const THD *thd) {
  return thd->get_transaction()->cannot_safely_rollback(Transaction_ctx::STMT);
}

/**
  Execute a PURGE BINARY LOGS TO @<log@> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param to_log Name of the last log to purge.

  @retval false success
  @retval true failure
*/
bool purge_source_logs_to_file(THD *thd, const char *to_log) {
  // first run the purge validations
  auto [is_invalid, invalid_error] = check_purge_conditions(mysql_bin_log);
  if (is_invalid) return purge_error_message(thd, invalid_error);

  // lock BACKUP lock for the duration of PURGE operation
  Shared_backup_lock_guard backup_lock{thd};
  switch (backup_lock) {
    case Shared_backup_lock_guard::Lock_result::locked:
      break;
    case Shared_backup_lock_guard::Lock_result::not_locked:
      return purge_error_message(thd, LOG_INFO_BACKUP_LOCK);
    case Shared_backup_lock_guard::Lock_result::oom:
      return purge_error_message(thd, LOG_INFO_MEM);
  }

  char search_file_name[FN_REFLEN];
  constexpr auto auto_purge{false};
  constexpr auto include_to_log{false};
  constexpr auto need_index_lock{true};
  constexpr auto need_update_threads{true};
  mysql_bin_log.make_log_name(search_file_name, to_log);
  auto purge_error = mysql_bin_log.purge_logs(
      search_file_name, include_to_log, need_index_lock, need_update_threads,
      nullptr, auto_purge);
  ha_binlog_index_purge_wait(thd);
  return purge_error_message(thd, purge_error);
}

/**
  Execute a PURGE BINARY LOGS BEFORE @<date@> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param purge_time Date before which logs should be purged.

  @retval false success
  @retval true failure
*/
bool purge_source_logs_before_date(THD *thd, time_t purge_time) {
  // first run the purge validations
  const auto [is_invalid, invalid_error] =
      check_purge_conditions(mysql_bin_log);
  if (is_invalid) return purge_error_message(thd, invalid_error);

  // lock BACKUP lock for the duration of PURGE operation
  Shared_backup_lock_guard backup_lock{thd};
  switch (backup_lock) {
    case Shared_backup_lock_guard::Lock_result::locked:
      break;
    case Shared_backup_lock_guard::Lock_result::not_locked:
      return purge_error_message(thd, LOG_INFO_BACKUP_LOCK);
    case Shared_backup_lock_guard::Lock_result::oom:
      return purge_error_message(thd, LOG_INFO_MEM);
  }

  // purge
  constexpr auto auto_purge{false};
  auto purge_error =
      mysql_bin_log.purge_logs_before_date(purge_time, auto_purge);
  ha_binlog_index_purge_wait(thd);
  return purge_error_message(thd, purge_error);
}

/*
  Helper function to get the error code of the query to be binlogged.
 */
int query_error_code(const THD *thd, bool not_killed) {
  int error;

  if (not_killed) {
    error = thd->is_error() ? thd->get_stmt_da()->mysql_errno() : 0;

    /* thd->get_stmt_da()->sql_errno() might be ER_SERVER_SHUTDOWN or
       ER_QUERY_INTERRUPTED, So here we need to make sure that error
       is not set to these errors when specified not_killed by the
       caller.
    */
    if (error == ER_SERVER_SHUTDOWN || error == ER_QUERY_INTERRUPTED) error = 0;
  } else
    error = thd->killed;

  return error;
}

/**
   Load data's io cache specific hook to be executed
   before a chunk of data is being read into the cache's buffer
   The function instantiates and writes into the binlog
   replication events along LOAD DATA processing.

   @param file  pointer to io-cache
   @retval 0 success
   @retval 1 failure
*/
int log_loaded_block(IO_CACHE *file) {
  DBUG_TRACE;
  LOAD_FILE_INFO *lf_info;
  uint block_len;
  /* buffer contains position where we started last read */
  uchar *buffer = (uchar *)my_b_get_buffer_start(file);
  uint max_event_size = current_thd->variables.max_allowed_packet;
  lf_info = (LOAD_FILE_INFO *)file->arg;
  if (lf_info->thd->is_current_stmt_binlog_format_row()) return 0;
  if (lf_info->last_pos_in_file != HA_POS_ERROR &&
      lf_info->last_pos_in_file >= my_b_get_pos_in_file(file))
    return 0;

  for (block_len = (uint)(my_b_get_bytes_in_buffer(file)); block_len > 0;
       buffer += min(block_len, max_event_size),
      block_len -= min(block_len, max_event_size)) {
    lf_info->last_pos_in_file = my_b_get_pos_in_file(file);
    if (lf_info->logged_data_file) {
      Append_block_log_event a(lf_info->thd, lf_info->thd->db().str, buffer,
                               min(block_len, max_event_size),
                               lf_info->log_delayed);
      if (mysql_bin_log.write_event(&a)) return 1;
    } else {
      Begin_load_query_log_event b(lf_info->thd, lf_info->thd->db().str, buffer,
                                   min(block_len, max_event_size),
                                   lf_info->log_delayed);
      if (mysql_bin_log.write_event(&b)) return 1;
      lf_info->logged_data_file = true;
    }
  }
  return 0;
}

/* Helper function for SHOW BINLOG/RELAYLOG EVENTS */
template <class BINLOG_FILE_READER>
bool show_binlog_events(THD *thd, MYSQL_BIN_LOG *binary_log) {
  Protocol *protocol = thd->get_protocol();
  List<Item> field_list;
  std::string errmsg;
  Log_info linfo;

  DBUG_TRACE;

  assert(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS ||
         thd->lex->sql_command == SQLCOM_SHOW_RELAYLOG_EVENTS);

  if (binary_log->is_open()) {
    LEX_SOURCE_INFO *lex_mi = &thd->lex->mi;
    Query_expression *unit = thd->lex->unit;
    ha_rows event_count, limit_start, limit_end;
    my_off_t pos =
        max<my_off_t>(BIN_LOG_HEADER_SIZE, lex_mi->pos);  // user-friendly
    char search_file_name[FN_REFLEN], *name;
    const char *log_file_name = lex_mi->log_file_name;

    unit->set_limit(thd, thd->lex->current_query_block());
    limit_start = unit->offset_limit_cnt;
    limit_end = unit->select_limit_cnt;

    name = search_file_name;
    if (log_file_name)
      binary_log->make_log_name(search_file_name, log_file_name);
    else
      name = nullptr;  // Find first log

    linfo.index_file_offset = 0;

    if (binary_log->find_log_pos(&linfo, name, true /*need_lock_index=true*/)) {
      errmsg = "Could not find target log";
      goto err;
    }

    linfo.thread_id = thd->thread_id();
    binary_log->register_log_info(&linfo);

    BINLOG_FILE_READER binlog_file_reader(
        opt_source_verify_checksum,
        std::max(thd->variables.max_allowed_packet,
                 binlog_row_event_max_size + MAX_LOG_EVENT_HEADER));

    if (binlog_file_reader.open(linfo.log_file_name, pos)) {
      errmsg = binlog_file_reader.get_error_str();
      goto err;
    }

    /*
      Adjust the pos to the correct starting offset of an event after the
      specified position if it is an invalid starting offset.
    */
    pos = binlog_file_reader.position();

    /*
      For 'in-active' binlog file, it is safe to read all events in it. But
      for 'active' binlog file, it is only safe to read the events before
      get_binlog_end_pos().

      Binlog rotation may happen after calling is_active(). In this case,
      end_pos will NOT be set to 0 while the file is actually not 'active'.
      It is safe, since 'end_pos' still expresses a correct position.
    */
    my_off_t end_pos = binary_log->get_binlog_end_pos();
    if (!binary_log->is_active(linfo.log_file_name)) end_pos = 0;

    DEBUG_SYNC(thd, "after_show_binlog_event_found_file");

    binlog::Decompressing_event_object_istream istream(
        binlog_file_reader, psi_memory_resource(key_memory_show_binlog_events));

    my_off_t last_log_pos = 0;
    event_count = 0;
    std::shared_ptr<Log_event> ev;
    while (istream >> ev) {
      DEBUG_SYNC(thd, "wait_in_show_binlog_events_loop");
      if (event_count >= limit_start &&
          ev->net_send(protocol, linfo.log_file_name, pos)) {
        errmsg = "Net error";
        goto err;
      }
      last_log_pos = ev->common_header->log_pos;
      pos = binlog_file_reader.position();

      if (++event_count == limit_end) break;
      if (end_pos > 0 && pos >= end_pos &&
          (ev->common_header->log_pos != last_log_pos)) {
        break;
      }
    }
    if (istream.has_error()) errmsg = istream.get_error_str();
  }
  // Check that linfo is still on the function scope.
  DEBUG_SYNC(thd, "after_show_binlog_events");

err:
  if (!errmsg.empty()) {
    if (thd->lex->sql_command == SQLCOM_SHOW_RELAYLOG_EVENTS)
      my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SHOW RELAYLOG EVENTS",
               errmsg.c_str());
    else
      my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SHOW BINLOG EVENTS",
               errmsg.c_str());
  } else
    my_eof(thd);

  binary_log->unregister_log_info(&linfo);

  return !errmsg.empty();
}

bool show_binlog_events(THD *thd, MYSQL_BIN_LOG *binary_log) {
  if (binary_log->is_relay_log)
    return show_binlog_events<Relaylog_file_reader>(thd, binary_log);
  return show_binlog_events<Binlog_file_reader>(thd, binary_log);
}

/**
  Execute a SHOW BINLOG EVENTS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval false success
  @retval true failure
*/
bool mysql_show_binlog_events(THD *thd) {
  DBUG_TRACE;

  assert(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS);

  mem_root_deque<Item *> field_list(thd->mem_root);
  Log_event::init_show_field_list(&field_list);
  if (thd->send_result_metadata(field_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    return true;

  /*
    Wait for handlers to insert any pending information
    into the binlog.  For e.g. ndb which updates the binlog asynchronously
    this is needed so that the uses sees all its own commands in the binlog
  */
  ha_binlog_wait(thd);

  return show_binlog_events(thd, &mysql_bin_log);
}

MYSQL_BIN_LOG::MYSQL_BIN_LOG(uint *sync_period, bool relay_log)
    : name(nullptr),
      write_error(false),
      inited(false),
      m_binlog_file(new Binlog_ofile()),
      m_key_LOCK_log(key_LOG_LOCK_log),
      bytes_written(0),
      m_binlog_index_monitor(relay_log),
      file_id(1),
      sync_period_ptr(sync_period),
      sync_counter(0),
      is_relay_log(relay_log),
      checksum_alg_reset(mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF),
      relay_log_checksum_alg(mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF),
      previous_gtid_set_relaylog(nullptr) {
  /*
    We don't want to initialize locks here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
}

MYSQL_BIN_LOG::~MYSQL_BIN_LOG() { delete m_binlog_file; }

/* this is called only once */

void MYSQL_BIN_LOG::cleanup() {
  DBUG_TRACE;
  if (inited) {
    inited = false;
    close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT, true /*need_lock_log=true*/,
          true /*need_lock_index=true*/);
    mysql_mutex_destroy(&LOCK_log);
    m_binlog_index_monitor.cleanup();
    mysql_mutex_destroy(&LOCK_commit);
    mysql_mutex_destroy(&LOCK_after_commit);
    mysql_mutex_destroy(&LOCK_sync);
    mysql_mutex_destroy(&LOCK_binlog_end_pos);
    mysql_mutex_destroy(&LOCK_xids);
    mysql_mutex_destroy(&LOCK_log_info);
    mysql_cond_destroy(&update_cond);
    mysql_cond_destroy(&m_prep_xids_cond);
    if (!is_relay_log) {
      Commit_stage_manager::get_instance().deinit();
    }
  }

  delete m_binlog_file;
  m_binlog_file = nullptr;
}

void MYSQL_BIN_LOG::init_pthread_objects() {
  assert(inited == 0);
  inited = true;

  mysql_mutex_init(m_key_LOCK_log, &LOCK_log, MY_MUTEX_INIT_SLOW);
  m_binlog_index_monitor.init_pthread_objects();
  mysql_mutex_init(m_key_LOCK_commit, &LOCK_commit, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_after_commit, &LOCK_after_commit,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_sync, &LOCK_sync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_binlog_end_pos, &LOCK_binlog_end_pos,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_xids, &LOCK_xids, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_log_info, &LOCK_log_info, MY_MUTEX_INIT_FAST);
  mysql_cond_init(m_key_update_cond, &update_cond);
  mysql_cond_init(m_key_prep_xids_cond, &m_prep_xids_cond);
  if (!is_relay_log) {
    Commit_stage_manager::get_instance().init(
        m_key_LOCK_flush_queue, m_key_LOCK_sync_queue, m_key_LOCK_commit_queue,
        m_key_LOCK_after_commit_queue, m_key_LOCK_done,
        m_key_LOCK_wait_for_group_turn, m_key_COND_done, m_key_COND_flush_queue,
        m_key_COND_wait_for_group_turn);
  }
}

/**
  Check if a string is a valid number.

  @param str			String to test
  @param res			Store value here
  @param allow_wildcards	Set to 1 if we should ignore '%' and '_'

  @note
    For the moment the allow_wildcards argument is not used
    Should be moved to some other file.

  @retval
    1	String is a number
  @retval
    0	String is not a number
*/

static bool is_number(const char *str, ulong *res, bool allow_wildcards) {
  int flag;
  const char *start;
  DBUG_TRACE;

  flag = 0;
  start = str;
  while (*str++ == ' ')
    ;
  if (*--str == '-' || *str == '+') str++;
  while (my_isdigit(files_charset_info, *str) ||
         (allow_wildcards && (*str == wild_many || *str == wild_one))) {
    flag = 1;
    str++;
  }
  if (*str == '.') {
    for (str++; my_isdigit(files_charset_info, *str) ||
                (allow_wildcards && (*str == wild_many || *str == wild_one));
         str++, flag = 1)
      ;
  }
  if (*str != 0 || flag == 0) return false;
  if (res) *res = atol(start);
  return true; /* Number ok */
} /* is_number */

/**
  Find a unique filename for 'filename.#'.

  Set '#' to the highest existing log file extension plus one.

  This function will return nonzero if: (i) the generated name
  exceeds FN_REFLEN; (ii) if the number of extensions is exhausted;
  or (iii) some other error happened while examining the filesystem.

  @return
    nonzero if not possible to get unique filename.
*/

static int find_uniq_filename(char *name, uint32 new_index_number) {
  uint i;
  char buff[FN_REFLEN], ext_buf[FN_REFLEN];
  MY_DIR *dir_info = nullptr;
  struct fileinfo *file_info;
  ulong max_found = 0, next = 0, number = 0;
  size_t buf_length, length;
  char *start, *end;
  int error = 0;
  DBUG_TRACE;

  length = dirname_part(buff, name, &buf_length);
  start = name + length;
  end = strend(start);

  *end = '.';
  length = (size_t)(end - start + 1);

  if ((DBUG_EVALUATE_IF(
          "error_unique_log_filename", 1,
          !(dir_info =
                my_dir(buff, MYF(MY_DONT_SORT)))))) {  // This shouldn't happen
    my_stpcpy(end, ".1");                              // use name+1
    return 1;
  }
  file_info = dir_info->dir_entry;
  for (i = dir_info->number_off_files; i--; file_info++) {
    if (strncmp(file_info->name, start, length) == 0 &&
        is_number(file_info->name + length, &number, false)) {
      max_found = std::max(max_found, number);
    }
  }
  my_dirend(dir_info);

  /* check if reached the maximum possible extension number */
  if (max_found >= MAX_LOG_UNIQUE_FN_EXT) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FILE_EXTENSION_NUMBER_EXHAUSTED, max_found);
    error = 1;
    goto end;
  }

  if (new_index_number > 0) {
    /*
      If "new_index_number" was specified, this means we are handling a
      "RESET BINARY LOGS AND GTIDS TO" command and the binary log was already
      purged so max_found should be 0.
    */
    assert(max_found == 0);
    next = new_index_number;
  } else
    next = max_found + 1;
  if (sprintf(ext_buf, "%06lu", next) < 0) {
    error = 1;
    goto end;
  }
  *end++ = '.';

  /*
    Check if the generated extension size + the file name exceeds the
    buffer size used. If one did not check this, then the filename might be
    truncated, resulting in error.
   */
  if (((strlen(ext_buf) + (end - name)) >= FN_REFLEN)) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FILE_NAME_TOO_LONG, name, ext_buf,
           (strlen(ext_buf) + (end - name)));
    error = 1;
    goto end;
  }

  if (sprintf(end, "%06lu", next) < 0) {
    error = 1;
    goto end;
  }

  /* print warning if reaching the end of available extensions. */
  if (next > MAX_ALLOWED_FN_EXT_RESET_BIN_LOGS)
    LogErr(WARNING_LEVEL, ER_BINLOG_FILE_EXTENSION_NUMBER_RUNNING_LOW, next,
           (MAX_LOG_UNIQUE_FN_EXT - next));

end:
  return error;
}

int MYSQL_BIN_LOG::generate_new_name(char *new_name, const char *log_name,
                                     uint32 new_index_number) {
  fn_format(new_name, log_name, mysql_data_home, "", 4);
  if (!fn_ext(log_name)[0]) {
    if (find_uniq_filename(new_name, new_index_number)) {
      if (current_thd != nullptr)
        my_printf_error(ER_NO_UNIQUE_LOGFILE,
                        ER_THD(current_thd, ER_NO_UNIQUE_LOGFILE),
                        MYF(ME_FATALERROR), log_name);
      LogErr(ERROR_LEVEL, ER_FAILED_TO_GENERATE_UNIQUE_LOGFILE, log_name);
      return 1;
    }
  }
  return 0;
}

/**
  @todo
  The following should be using fn_format();  We just need to
  first change fn_format() to cut the file name if it's too long.
*/
const char *MYSQL_BIN_LOG::generate_name(const char *log_name,
                                         const char *suffix, char *buff) {
  if (!log_name || !log_name[0]) {
    if (is_relay_log || log_bin_supplied)
      strmake(buff, default_logfile_name, FN_REFLEN - strlen(suffix) - 1);
    else
      strmake(buff, default_binlogfile_name, FN_REFLEN - strlen(suffix) - 1);

    return (const char *)fn_format(buff, buff, "", suffix,
                                   MYF(MY_REPLACE_EXT | MY_REPLACE_DIR));
  }
  // get rid of extension to avoid problems

  const char *p = fn_ext(log_name);
  uint length = (uint)(p - log_name);
  strmake(buff, log_name, min<size_t>(length, FN_REFLEN - 1));
  return (const char *)buff;
}

bool MYSQL_BIN_LOG::init_and_set_log_file_name(const char *log_name,
                                               const char *new_name,
                                               uint32 new_index_number) {
  if (new_name && !my_stpcpy(log_file_name, new_name))
    return true;
  else if (!new_name &&
           generate_new_name(log_file_name, log_name, new_index_number))
    return true;

  return false;
}

/**
  Open the logfile and init IO_CACHE.

  @param log_file_key        The file instrumentation key for this file
  @param log_name            The name of the log to open
  @param new_name            The new name for the logfile.
                             NULL forces generate_new_name() to be called.
  @param new_index_number    The binary log file index number to start from
                             after the RESET BINARY LOGS AND GTIDS command is
                             called.

  @return true if error, false otherwise.
*/

bool MYSQL_BIN_LOG::open(PSI_file_key log_file_key, const char *log_name,
                         const char *new_name, uint32 new_index_number) {
  DBUG_TRACE;
  bool ret = false;

  write_error = false;
  myf flags = MY_WME | MY_NABP | MY_WAIT_IF_FULL;
  if (is_relay_log) flags = flags | MY_REPORT_WAITING_IF_FULL;

  if (!(name = my_strdup(key_memory_MYSQL_LOG_name, log_name, MYF(MY_WME)))) {
    goto err;
  }

  if (init_and_set_log_file_name(name, new_name, new_index_number) ||
      DBUG_EVALUATE_IF("fault_injection_init_name", 1, 0))
    goto err;

  db[0] = 0;

  /* Keep the key for reopen */
  m_log_file_key = log_file_key;

  /*
    LOCK_sync guarantees that no thread is calling m_binlog_file to sync data
    to disk when another thread is opening the new file
    (FLUSH LOG or RESET BINARY LOGS AND GTIDS).
  */
  if (!is_relay_log) mysql_mutex_lock(&LOCK_sync);

  ret = m_binlog_file->open(log_file_key, log_file_name, flags);

  if (!is_relay_log) mysql_mutex_unlock(&LOCK_sync);

  if (ret) goto err;

  atomic_log_state = LOG_OPENED;
  return false;

err:
  if (binlog_error_action == ABORT_SERVER) {
    exec_binlog_error_action_abort(
        "Either disk is full, file system is read only or "
        "there was an encryption error while opening the binlog. "
        "Aborting the server.");
  } else
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_OPEN_FOR_LOGGING, log_name, errno);

  my_free(name);
  name = nullptr;
  atomic_log_state = LOG_CLOSED;
  return true;
}

bool MYSQL_BIN_LOG::open_index_file(const char *index_file_name_arg,
                                    const char *log_name,
                                    bool need_lock_index) {
  return m_binlog_index_monitor.open_index_file(
      index_file_name_arg, log_name, m_key_file_log, need_lock_index);
}

/**
  Add the GTIDs from the given relaylog file and also
  update the IO thread transaction parser.

  @param filename Relaylog file to read from.
  @param retrieved_gtids Gtid_set to store the GTIDs found on the relaylog file.
  @param verify_checksum Set to true to verify event checksums.
  @param trx_parser The transaction boundary parser to be used in order to
  only add a GTID to the gtid_set after ensuring the transaction is fully
  stored on the relay log.
  @param partial_trx The trx_monitoring_info of the last incomplete transaction
  found in the relay log.

  @retval false The file was successfully read and all GTIDs from
  Previous_gtids and Gtid_log_event from complete transactions were added to
  the retrieved_set.
  @retval true There was an error during the procedure.
*/
static bool read_gtids_and_update_trx_parser_from_relaylog(
    const char *filename, Gtid_set *retrieved_gtids, bool verify_checksum,
    mysql::binlog::event::Transaction_boundary_parser *trx_parser,
    Gtid_monitoring_info *partial_trx) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("Opening file %s", filename));

  assert(retrieved_gtids != nullptr);
  assert(trx_parser != nullptr);
#ifndef NDEBUG
  unsigned long event_counter = 0;
#endif
  bool error = false;

  Relaylog_file_reader relaylog_file_reader(verify_checksum);
  if (relaylog_file_reader.open(filename)) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FILE_OPEN_FAILED,
           relaylog_file_reader.get_error_str());

    /*
      As read_gtids_from_binlog() will not throw error on truncated
      relaylog files, we should do the same here in order to keep the
      current behavior.
    */
    if (relaylog_file_reader.get_error_type() ==
        Binlog_read_error::CANNOT_GET_FILE_PASSWORD)
      error = true;
    return error;
  }

  Log_event *ev = nullptr;
  bool seen_prev_gtids = false;
  ulong data_len = 0;

  while (!error && (ev = relaylog_file_reader.read_event_object()) != nullptr) {
    DBUG_PRINT("info", ("Read event of type %s", ev->get_type_str()));
#ifndef NDEBUG
    event_counter++;
#endif

    data_len = uint4korr(ev->temp_buf + EVENT_LEN_OFFSET);

    bool info_error{false};
    mysql::binlog::event::Log_event_basic_info log_event_info;
    std::tie(info_error, log_event_info) = extract_log_event_basic_info(
        ev->temp_buf, data_len,
        &relaylog_file_reader.format_description_event());

    if (info_error || trx_parser->feed_event(log_event_info, false)) {
      /*
        The transaction boundary parser found an error while parsing a
        sequence of events from the relaylog. As we don't know if the
        parsing has started from a reliable point (it might started in
        a relay log file that begins with the rest of a transaction
        that started in a previous relay log file), it is better to do
        nothing in this case. The boundary parser will fix itself once
        finding an event that represent a transaction boundary.

        Suppose the following relaylog:

         rl-bin.000011 | rl-bin.000012 | rl-bin.000013 | rl-bin-000014
        ---------------+---------------+---------------+---------------
         PREV_GTIDS    | PREV_GTIDS    | PREV_GTIDS    | PREV_GTIDS
         (empty)       | (UUID:1-2)    | (UUID:1-2)    | (UUID:1-2)
        ---------------+---------------+---------------+---------------
         XID           | QUERY(INSERT) | QUERY(INSERT) | XID
        ---------------+---------------+---------------+---------------
         GTID(UUID:2)  |
        ---------------+
         QUERY(CREATE  |
         TABLE t1 ...) |
        ---------------+
         GTID(UUID:3)  |
        ---------------+
         QUERY(BEGIN)  |
        ---------------+

        As it is impossible to determine the current Retrieved_Gtid_Set by only
        looking to the PREVIOUS_GTIDS on the last relay log file, and scanning
        events on it, we tried to find a relay log file that contains at least
        one GTID event during the backwards search.

        In the example, we will find a GTID only in rl-bin.000011, as the
        UUID:3 transaction was spanned across 4 relay log files.

        The transaction spanning can be caused by "FLUSH RELAY LOGS" commands
        on slave while it is queuing the transaction.

        So, in order to correctly add UUID:3 into Retrieved_Gtid_Set, we need
        to parse the relay log starting on the file we found the last GTID
        queued to know if the transaction was fully retrieved or not.

        Start scanning rl-bin.000011 after resetting the transaction parser
        will generate an error, as XID event is only expected inside a DML,
        but in this case, we can ignore this error and reset the parser.
      */
      trx_parser->reset();
      /*
        We also have to discard the GTID of the partial transaction that was
        not finished if there is one. This is needed supposing that an
        incomplete transaction was replicated with a GTID.

        GTID(1), QUERY(BEGIN), QUERY(INSERT), ANONYMOUS_GTID, QUERY(DROP ...)

        In the example above, without cleaning the partial_trx,
        the GTID(1) would be added to the Retrieved_Gtid_Set after the
        QUERY(DROP ...) event.

        GTID(1), QUERY(BEGIN), QUERY(INSERT), GTID(2), QUERY(DROP ...)

        In the example above the GTID(1) will also be discarded as the
        GTID(1) transaction is not complete.
      */
      if (partial_trx->is_processing_trx_set()) {
        DBUG_PRINT("info",
                   ("Discarding Gtid(%d, %" PRId64 ") as the transaction "
                    "wasn't complete and we found an error in the"
                    "transaction boundary parser.",
                    partial_trx->get_processing_trx_gtid()->sidno,
                    partial_trx->get_processing_trx_gtid()->gno));
        partial_trx->clear_processing_trx();
      }
    }

    switch (ev->get_type_code()) {
      case mysql::binlog::event::FORMAT_DESCRIPTION_EVENT:
      case mysql::binlog::event::ROTATE_EVENT:
        // do nothing; just accept this event and go to next
        break;
      case mysql::binlog::event::PREVIOUS_GTIDS_LOG_EVENT: {
        seen_prev_gtids = true;
        // add events to sets
        Previous_gtids_log_event *prev_gtids_ev =
            (Previous_gtids_log_event *)ev;
        if (prev_gtids_ev->add_to_set(retrieved_gtids) != 0) {
          error = true;
          break;
        }
#ifndef NDEBUG
        char *prev_buffer = prev_gtids_ev->get_str(nullptr, nullptr);
        DBUG_PRINT("info", ("Got Previous_gtids from file '%s': Gtid_set='%s'.",
                            filename, prev_buffer));
        my_free(prev_buffer);
#endif
        break;
      }
      case mysql::binlog::event::GTID_LOG_EVENT:
      case mysql::binlog::event::GTID_TAGGED_LOG_EVENT: {
        /* If we didn't find any PREVIOUS_GTIDS in this file */
        if (!seen_prev_gtids) {
          my_error(ER_BINLOG_LOGICAL_CORRUPTION, MYF(0), filename,
                   "The first global transaction identifier was read, but "
                   "no other information regarding identifiers existing "
                   "on the previous log files was found.");
          error = true;
          break;
        }

        Gtid_log_event *gtid_ev = (Gtid_log_event *)ev;
        rpl_sidno sidno = gtid_ev->get_sidno(retrieved_gtids->get_tsid_map());
        ulonglong immediate_commit_timestamp =
            gtid_ev->immediate_commit_timestamp;
        longlong original_commit_timestamp = gtid_ev->original_commit_timestamp;

        if (sidno < 0) {
          error = true;
          break;
        } else {
          if (retrieved_gtids->ensure_sidno(sidno) != RETURN_STATUS_OK) {
            error = true;
            break;
          } else {
            Gtid gtid = {sidno, gtid_ev->get_gno()};
            /*
              As are updating the transaction boundary parser while reading
              GTIDs from relay log files to fill the Retrieved_Gtid_Set, we
              should not add the GTID here as we don't know if the transaction
              is complete on the relay log yet.
            */
            partial_trx->start(gtid, original_commit_timestamp,
                               immediate_commit_timestamp);
          }
          DBUG_PRINT(
              "info",
              ("Found Gtid in relaylog file '%s': Gtid(%d, %" PRId64 ").",
               filename, sidno, gtid_ev->get_gno()));
        }
        break;
      }
      case mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT:
      default:
        /*
          If we reached the end of a transaction after storing it's GTID
          in partial_trx structure, it is time to add this GTID to the
          retrieved_gtids set because the transaction is complete and there is
          no need for asking this transaction again.
        */
        if (trx_parser->is_not_inside_transaction()) {
          if (partial_trx->is_processing_trx_set()) {
            const Gtid *fully_retrieved_gtid;
            fully_retrieved_gtid = partial_trx->get_processing_trx_gtid();
            DBUG_PRINT("info", ("Adding Gtid to Retrieved_Gtid_Set as the "
                                "transaction was completed at "
                                "relaylog file '%s': Gtid(%d, %" PRId64 ").",
                                filename, fully_retrieved_gtid->sidno,
                                fully_retrieved_gtid->gno));
            retrieved_gtids->_add_gtid(*fully_retrieved_gtid);
            /*
             We don't need to update the last queued structure here. We just
             want to have the information about the partial transaction left in
             the relay log.
            */
            partial_trx->clear();
          }
        }
        break;
    }
    delete ev;
  }

  if (relaylog_file_reader.has_fatal_error()) {
    // This is not a fatal error; the log may just be truncated.
    // @todo but what other errors could happen? IO error?
    LogErr(WARNING_LEVEL, ER_BINLOG_ERROR_READING_GTIDS_FROM_RELAY_LOG, -1);
  }

#ifndef NDEBUG
  LogErr(INFORMATION_LEVEL, ER_BINLOG_EVENTS_READ_FROM_APPLIER_METADATA,
         event_counter, filename);
#endif

  return error;
}

enum enum_read_gtids_from_binlog_status {
  GOT_GTIDS,
  GOT_PREVIOUS_GTIDS,
  NO_GTIDS,
  ERROR,
  TRUNCATED
};
/**
  Reads GTIDs from the given binlog file.

  @param filename File to read from.
  @param all_gtids If not NULL, then the GTIDs from the
  Previous_gtids_log_event and from all Gtid_log_events are stored in
  this object.
  @param prev_gtids If not NULL, then the GTIDs from the
  Previous_gtids_log_events are stored in this object.
  @param first_gtid If not NULL, then the first GTID information from the
  file will be stored in this object.
  @param tsid_map The tsid_map object to use in the rpl_sidno generation
  of the Gtid_log_event. If lock is needed in the tsid_map, the caller
  must hold it.
  @param verify_checksum Set to true to verify event checksums.
  @param is_relay_log Set to true, if filename is a Relay Log, false if it is a
  Binary Log.
  @retval GOT_GTIDS The file was successfully read and it contains
  both Gtid_log_events and Previous_gtids_log_events.
  This is only possible if either all_gtids or first_gtid are not null.
  @retval GOT_PREVIOUS_GTIDS The file was successfully read and it
  contains Previous_gtids_log_events but no Gtid_log_events.
  For binary logs, if no all_gtids and no first_gtid are specified,
  this function will be done right after reading the PREVIOUS_GTIDS
  regardless of the rest of the content of the binary log file.
  @retval NO_GTIDS The file was successfully read and it does not
  contain GTID events.
  @retval ERROR Out of memory, or IO error, or malformed event
  structure, or the file is malformed (e.g., contains Gtid_log_events
  but no Previous_gtids_log_event).
  @retval TRUNCATED The file was truncated before the end of the
  first Previous_gtids_log_event.
*/
static enum_read_gtids_from_binlog_status read_gtids_from_binlog(
    const char *filename, Gtid_set *all_gtids, Gtid_set *prev_gtids,
    Gtid *first_gtid, Tsid_map *tsid_map, bool verify_checksum,
    bool is_relay_log) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("Opening file %s", filename));

#ifndef NDEBUG
  unsigned long event_counter = 0;
  /*
    We assert here that both all_gtids and prev_gtids, if specified,
    uses the same tsid_map as the one passed as a parameter. This is just
    to ensure that, if the tsid_map needed some lock and was locked by
    the caller, the lock applies to all the GTID sets this function is
    dealing with.
  */
  if (all_gtids) assert(all_gtids->get_tsid_map() == tsid_map);
  if (prev_gtids) assert(prev_gtids->get_tsid_map() == tsid_map);
#endif

  Binlog_file_reader binlog_file_reader(verify_checksum);
  if (binlog_file_reader.open(filename)) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FILE_OPEN_FAILED,
           binlog_file_reader.get_error_str());
    /*
      We need to revisit the recovery procedure for relay log
      files. Currently, it is called after this routine.
      /Alfranio
    */
    if (binlog_file_reader.get_error_type() ==
        Binlog_read_error::CANNOT_GET_FILE_PASSWORD)
      return ERROR;
    return TRUNCATED;
  }

  Log_event *ev = nullptr;
  enum_read_gtids_from_binlog_status ret = NO_GTIDS;
  bool done = false;
  bool seen_first_gtid = false;
  while (!done && (ev = binlog_file_reader.read_event_object()) != nullptr) {
#ifndef NDEBUG
    event_counter++;
#endif
    DBUG_PRINT("info", ("Read event of type %s", ev->get_type_str()));
    switch (ev->get_type_code()) {
      case mysql::binlog::event::FORMAT_DESCRIPTION_EVENT:
      case mysql::binlog::event::ROTATE_EVENT:
        // do nothing; just accept this event and go to next
        break;
      case mysql::binlog::event::PREVIOUS_GTIDS_LOG_EVENT: {
        ret = GOT_PREVIOUS_GTIDS;
        // add events to sets
        Previous_gtids_log_event *prev_gtids_ev =
            (Previous_gtids_log_event *)ev;
        if (all_gtids != nullptr && prev_gtids_ev->add_to_set(all_gtids) != 0)
          ret = ERROR, done = true;
        else if (prev_gtids != nullptr &&
                 prev_gtids_ev->add_to_set(prev_gtids) != 0)
          ret = ERROR, done = true;
#ifndef NDEBUG
        char *prev_buffer = prev_gtids_ev->get_str(nullptr, nullptr);
        DBUG_PRINT("info", ("Got Previous_gtids from file '%s': Gtid_set='%s'.",
                            filename, prev_buffer));
        my_free(prev_buffer);
#endif
        /*
          If this is not a relay log, the previous_gtids were asked and no
          all_gtids neither first_gtid were asked, it is fine to consider the
          job as done.
        */
        if (!is_relay_log && prev_gtids != nullptr && all_gtids == nullptr &&
            first_gtid == nullptr)
          done = true;
        DBUG_EXECUTE_IF("inject_fault_bug16502579", {
          DBUG_PRINT("debug", ("PREVIOUS_GTIDS_LOG_EVENT found. "
                               "Injected ret=NO_GTIDS."));
          if (ret == GOT_PREVIOUS_GTIDS) {
            ret = NO_GTIDS;
            done = false;
          }
        });
        break;
      }
      case mysql::binlog::event::GTID_LOG_EVENT:
      case mysql::binlog::event::GTID_TAGGED_LOG_EVENT: {
        if (ret != GOT_GTIDS) {
          if (ret != GOT_PREVIOUS_GTIDS) {
            /*
              Since this routine is run on startup, there may not be a
              THD instance. Therefore, ER(X) cannot be used.
             */
            const char *msg_fmt =
                (current_thd != nullptr)
                    ? ER_THD(current_thd, ER_BINLOG_LOGICAL_CORRUPTION)
                    : ER_DEFAULT(ER_BINLOG_LOGICAL_CORRUPTION);
            my_printf_error(
                ER_BINLOG_LOGICAL_CORRUPTION, msg_fmt, MYF(0), filename,
                "The first global transaction identifier was read, but "
                "no other information regarding identifiers existing "
                "on the previous log files was found.");
            ret = ERROR, done = true;
            break;
          } else
            ret = GOT_GTIDS;
        }
        /*
          When this is a relaylog, we just check if the relay log contains at
          least one Gtid_log_event, so that we can distinguish the return values
          GOT_GTID and GOT_PREVIOUS_GTIDS. We don't need to read anything else
          from the relay log.
          When this is a binary log, if all_gtids is requested (i.e., NOT NULL),
          we should continue to read all gtids. If just first_gtid was
          requested, we will be done after storing this Gtid_log_event info on
          it.
        */
        if (is_relay_log) {
          ret = GOT_GTIDS, done = true;
        } else {
          Gtid_log_event *gtid_ev = (Gtid_log_event *)ev;
          rpl_sidno sidno = gtid_ev->get_sidno(tsid_map);
          if (sidno < 0)
            ret = ERROR, done = true;
          else {
            if (all_gtids) {
              if (all_gtids->ensure_sidno(sidno) != RETURN_STATUS_OK)
                ret = ERROR, done = true;
              all_gtids->_add_gtid(sidno, gtid_ev->get_gno());
              DBUG_PRINT("info",
                         ("Got Gtid from file '%s': Gtid(%d, %" PRId64 ").",
                          filename, sidno, gtid_ev->get_gno()));
            }

            /* If the first GTID was requested, stores it */
            if (first_gtid && !seen_first_gtid) {
              first_gtid->set(sidno, gtid_ev->get_gno());
              seen_first_gtid = true;
              /* If the first_gtid was the only thing requested, we are done */
              if (all_gtids == nullptr) ret = GOT_GTIDS, done = true;
            }
          }
        }
        break;
      }
      case mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT: {
        /*
          When this is a relaylog, we just check if it contains
          at least one Anonymous_gtid_log_event after initialization
          (FDs, Rotates and PREVIOUS_GTIDS), so that we can distinguish the
          return values GOT_GTID and GOT_PREVIOUS_GTIDS.
          We don't need to read anything else from the relay log.
        */
        if (is_relay_log) {
          ret = GOT_GTIDS;
          done = true;
          break;
        }
        assert(prev_gtids == nullptr
                   ? true
                   : all_gtids != nullptr || first_gtid != nullptr);
      }
        [[fallthrough]];
      default:
        // if we found any other event type without finding a
        // previous_gtids_log_event, then the rest of this binlog
        // cannot contain gtids
        if (ret != GOT_GTIDS && ret != GOT_PREVIOUS_GTIDS) done = true;
        /*
          The GTIDs of the relaylog files will be handled later
          because of the possibility of transactions be spanned
          along distinct relaylog files.
          So, if we found an ordinary event without finding the
          GTID but we already found the PREVIOUS_GTIDS, this probably
          means that the event is from a transaction that started on
          previous relaylog file.
        */
        if (ret == GOT_PREVIOUS_GTIDS && is_relay_log) done = true;
        break;
    }
    delete ev;
    DBUG_PRINT("info", ("done=%d", done));
  }

  if (binlog_file_reader.has_fatal_error()) {
    // This is not a fatal error; the log may just be truncated.

    // @todo but what other errors could happen? IO error?
    LogErr(WARNING_LEVEL, ER_BINLOG_ERROR_READING_GTIDS_FROM_BINARY_LOG, -1);
  }

  if (all_gtids)
    all_gtids->dbug_print("all_gtids");
  else
    DBUG_PRINT("info", ("all_gtids==NULL"));
  if (prev_gtids)
    prev_gtids->dbug_print("prev_gtids");
  else
    DBUG_PRINT("info", ("prev_gtids==NULL"));
  if (first_gtid == nullptr)
    DBUG_PRINT("info", ("first_gtid==NULL"));
  else if (first_gtid->sidno == 0)
    DBUG_PRINT("info", ("first_gtid.sidno==0"));
  else
    first_gtid->dbug_print(tsid_map, "first_gtid");

  DBUG_PRINT("info", ("returning %d", ret));
#ifndef NDEBUG
  if (!is_relay_log && prev_gtids != nullptr && all_gtids == nullptr &&
      first_gtid == nullptr)
    LogErr(INFORMATION_LEVEL, ER_BINLOG_EVENTS_READ_FROM_BINLOG_INFO,
           event_counter, filename);
#endif
  return ret;
}

bool MYSQL_BIN_LOG::find_first_log(std::string &binlog_file_name,
                                   std::string &errmsg) {
  auto log_index = this->get_log_index();
  std::list<std::string> filename_list = log_index.second;

  list<string>::iterator fit = filename_list.begin();
  if (fit != filename_list.end()) {
    binlog_file_name.assign(*fit);
  } else {
    errmsg.assign("Could not find the first log file name in the index file");
    return true;
  }
  return false;
}

bool MYSQL_BIN_LOG::find_first_log_not_in_gtid_set(char *binlog_file_name,
                                                   const Gtid_set *gtid_set,
                                                   Gtid *first_gtid,
                                                   std::string &errmsg) {
  DBUG_TRACE;
  Log_info linfo;
  auto log_index = this->get_log_index();
  std::list<std::string> filename_list = log_index.second;
  int error = log_index.first;
  list<string>::reverse_iterator rit;
  Gtid_set binlog_previous_gtid_set{gtid_set->get_tsid_map()};

  if (error != LOG_INFO_EOF) {
    errmsg.assign(
        "Failed to read the binary log index file while "
        "looking for the oldest binary log that contains any GTID "
        "that is not in the given gtid set");
    error = -1;
    goto end;
  }

  if (filename_list.empty()) {
    errmsg.assign(
        "Could not find first log file name in binary log index file "
        "while looking for the oldest binary log that contains any GTID "
        "that is not in the given gtid set");
    error = -2;
    goto end;
  }

  /*
    Iterate over all the binary logs in reverse order, and read only
    the Previous_gtids_log_event, to find the first one, that is the
    subset of the given gtid set. Since every binary log begins with
    a Previous_gtids_log_event, that contains all GTIDs in all
    previous binary logs.
    We also ask for the first GTID in the binary log to know if we
    should send the FD event with the "created" field cleared or not.
  */
  DBUG_PRINT("info", ("Iterating backwards through binary logs, and reading "
                      "only the Previous_gtids_log_event, to find the first "
                      "one, that is the subset of the given gtid set."));
  rit = filename_list.rbegin();
  error = 0;
  while (rit != filename_list.rend()) {
    binlog_previous_gtid_set.clear();
    const char *filename = rit->c_str();
    DBUG_PRINT("info",
               ("Read Previous_gtids_log_event from filename='%s'", filename));
    switch (read_gtids_from_binlog(filename, nullptr, &binlog_previous_gtid_set,
                                   first_gtid,
                                   binlog_previous_gtid_set.get_tsid_map(),
                                   opt_source_verify_checksum, is_relay_log)) {
      case ERROR:
        errmsg.assign(
            "Error reading header of binary log while looking for "
            "the oldest binary log that contains any GTID that is not in "
            "the given gtid set");
        error = -3;
        goto end;
      case NO_GTIDS:
        errmsg.assign(
            "Found old binary log without GTIDs while looking for "
            "the oldest binary log that contains any GTID that is not in "
            "the given gtid set");
        error = -4;
        goto end;
      case GOT_GTIDS:
      case GOT_PREVIOUS_GTIDS:
        if (binlog_previous_gtid_set.is_subset(gtid_set)) {
          strcpy(binlog_file_name, filename);
          /*
            Verify that the selected binlog is not the first binlog,
          */
          DBUG_EXECUTE_IF("replica_reconnect_with_gtid_set_executed",
                          assert(strcmp(filename_list.begin()->c_str(),
                                        binlog_file_name) != 0););
          goto end;
        }
      case TRUNCATED:
        break;
    }

    rit++;
  }

  if (rit == filename_list.rend()) {
    report_missing_gtids(&binlog_previous_gtid_set, gtid_set, errmsg);
    error = -5;
  }

end:
  if (error) DBUG_PRINT("error", ("'%s'", errmsg.c_str()));
  filename_list.clear();
  DBUG_PRINT("info", ("returning %d", error));
  return error != 0 ? true : false;
}

bool MYSQL_BIN_LOG::init_gtid_sets(
    Gtid_set *all_gtids, Gtid_set *lost_gtids, bool verify_checksum,
    bool need_lock,
    mysql::binlog::event::Transaction_boundary_parser *trx_parser,
    Gtid_monitoring_info *partial_trx, bool is_server_starting) {
  DBUG_TRACE;
  DBUG_PRINT(
      "info",
      ("lost_gtids=%p; so we are recovering a %s log; is_relay_log=%d",
       lost_gtids, lost_gtids == nullptr ? "relay" : "binary", is_relay_log));

  Checkable_rwlock *tsid_lock = is_relay_log
                                    ? all_gtids->get_tsid_map()->get_tsid_lock()
                                    : global_tsid_lock;
  /*
    If this is a relay log, we must have the IO thread Master_info trx_parser
    in order to correctly feed it with relay log events.
  */
#ifndef NDEBUG
  if (is_relay_log) {
    assert(trx_parser != nullptr);
    assert(lost_gtids == nullptr);
  }
#endif

  /*
    Acquires the necessary locks to ensure that logs are not either
    removed or updated when we are reading from it.
  */
  if (need_lock) {
    // We don't need LOCK_log if we are only going to read the initial
    // Prevoius_gtids_log_event and ignore the Gtid_log_events.
    if (all_gtids != nullptr) mysql_mutex_lock(&LOCK_log);
    m_binlog_index_monitor.lock();
    tsid_lock->wrlock();
  } else {
    if (all_gtids != nullptr) mysql_mutex_assert_owner(&LOCK_log);
    m_binlog_index_monitor.assert_owner();
    tsid_lock->assert_some_wrlock();
  }

  /* Initialize the tsid_map to be used in read_gtids_from_binlog */
  Tsid_map *tsid_map = nullptr;
  if (all_gtids)
    tsid_map = all_gtids->get_tsid_map();
  else if (lost_gtids)
    tsid_map = lost_gtids->get_tsid_map();

  // Gather the set of files to be accessed.
  auto log_index = this->get_log_index(false);
  std::list<std::string> filename_list = log_index.second;
  int error = log_index.first;
  list<string>::iterator it;
  list<string>::reverse_iterator rit;
  bool reached_first_file = false;

  if (error != LOG_INFO_EOF) {
    DBUG_PRINT("error", ("Error reading %s index",
                         is_relay_log ? "relaylog" : "binlog"));
    goto end;
  }
  /*
    On server starting, one new empty binlog file is created and
    its file name is put into index file before initializing
    GLOBAL.GTID_EXECUTED AND GLOBAL.GTID_PURGED, it is not the
    last binlog file before the server restarts, so we remove
    its file name from filename_list.
  */
  if (is_server_starting && !is_relay_log && !filename_list.empty())
    filename_list.pop_back();

  error = 0;
  if (all_gtids != nullptr) {
    DBUG_PRINT("info", ("Iterating backwards through %s logs, "
                        "looking for the last %s log that contains "
                        "a Previous_gtids_log_event.",
                        is_relay_log ? "relay" : "binary",
                        is_relay_log ? "relay" : "binary"));
    // Iterate over all files in reverse order until we find one that
    // contains a Previous_gtids_log_event.
    rit = filename_list.rbegin();
    bool can_stop_reading = false;
    reached_first_file = (rit == filename_list.rend());
    DBUG_PRINT("info",
               ("filename='%s' reached_first_file=%d",
                reached_first_file ? "" : rit->c_str(), reached_first_file));
    while (!can_stop_reading && !reached_first_file) {
      const char *filename = rit->c_str();
      assert(rit != filename_list.rend());
      rit++;
      reached_first_file = (rit == filename_list.rend());
      DBUG_PRINT("info", ("filename='%s' can_stop_reading=%d "
                          "reached_first_file=%d, ",
                          filename, can_stop_reading, reached_first_file));
      switch (read_gtids_from_binlog(
          filename, all_gtids, reached_first_file ? lost_gtids : nullptr,
          nullptr /* first_gtid */, tsid_map, verify_checksum, is_relay_log)) {
        case ERROR: {
          error = 1;
          goto end;
        }
        case GOT_GTIDS: {
          can_stop_reading = true;
          break;
        }
        case GOT_PREVIOUS_GTIDS: {
          /*
            If this is a binlog file, it is enough to have GOT_PREVIOUS_GTIDS.
            If this is a relaylog file, we need to find at least one GTID to
            start parsing the relay log to add GTID of transactions that might
            have spanned in distinct relaylog files.
          */
          if (!is_relay_log) can_stop_reading = true;
          break;
        }
        case NO_GTIDS: {
          /*
            Mysql server iterates backwards through binary logs, looking for
            the last binary log that contains a Previous_gtids_log_event for
            gathering the set of gtid_executed on server start. This may take
            very long time if it has many binary logs and almost all of them
            are out of filesystem cache. So if the binlog_gtid_simple_recovery
            is enabled, and the last binary log does not contain any GTID
            event, do not read any more binary logs, GLOBAL.GTID_EXECUTED and
            GLOBAL.GTID_PURGED should be empty in the case.
          */
          if (binlog_gtid_simple_recovery && is_server_starting &&
              !is_relay_log) {
            assert(all_gtids->is_empty());
            assert(lost_gtids->is_empty());
            goto end;
          }
          [[fallthrough]];
        }
        case TRUNCATED: {
          break;
        }
      }
    }

    /*
      If we use GTIDs and have partial transactions on the relay log,
      must check if it ends on next relay log files.
      We also need to feed the boundary parser with the rest of the
      relay log to put it in the correct state before receiving new
      events from the master in the case of GTID auto positioning be
      disabled.
    */
    if (is_relay_log && filename_list.size() > 0) {
      /*
        Suppose the following relaylog:

         rl-bin.000001 | rl-bin.000002 | rl-bin.000003 | rl-bin-000004
        ---------------+---------------+---------------+---------------
         PREV_GTIDS    | PREV_GTIDS    | PREV_GTIDS    | PREV_GTIDS
         (empty)       | (UUID:1)      | (UUID:1)      | (UUID:1)
        ---------------+---------------+---------------+---------------
         GTID(UUID:1)  | QUERY(INSERT) | QUERY(INSERT) | XID
        ---------------+---------------+---------------+---------------
         QUERY(CREATE  |
         TABLE t1 ...) |
        ---------------+
         GTID(UUID:2)  |
        ---------------+
         QUERY(BEGIN)  |
        ---------------+

        As it is impossible to determine the current Retrieved_Gtid_Set by only
        looking to the PREVIOUS_GTIDS on the last relay log file, and scanning
        events on it, we tried to find a relay log file that contains at least
        one GTID event during the backwards search.

        In the example, we will find a GTID only in rl-bin.000001, as the
        UUID:2 transaction was spanned across 4 relay log files.

        The transaction spanning can be caused by "FLUSH RELAY LOGS" commands
        on slave while it is queuing the transaction.

        So, in order to correctly add UUID:2 into Retrieved_Gtid_Set, we need
        to parse the relay log starting on the file we found the last GTID
        queued to know if the transaction was fully retrieved or not.
      */

      /*
        Adjust the reverse iterator to point to the relaylog file we
        need to start parsing, as it was incremented after generating
        the relay log file name.
      */
      assert(rit != filename_list.rbegin());
      rit--;
      assert(rit != filename_list.rend());
      /* Reset the transaction parser before feeding it with events */
      trx_parser->reset();
      partial_trx->clear();

      DBUG_PRINT("info", ("Iterating forwards through relay logs, "
                          "updating the Retrieved_Gtid_Set and updating "
                          "IO thread trx parser before start."));
      for (it = find(filename_list.begin(), filename_list.end(), *rit);
           it != filename_list.end(); it++) {
        const char *filename = it->c_str();
        DBUG_PRINT("info", ("filename='%s'", filename));
        if (read_gtids_and_update_trx_parser_from_relaylog(
                filename, all_gtids, true, trx_parser, partial_trx)) {
          error = 1;
          goto end;
        }
      }
    }
  }
  if (lost_gtids != nullptr && !reached_first_file) {
    /*
      This branch is only reachable by a binary log. The relay log
      don't need to get lost_gtids information.

      A 5.6 server sets GTID_PURGED by rotating the binary log.

      A 5.6 server that had recently enabled GTIDs and set GTID_PURGED
      would have a sequence of binary logs like:

      master-bin.N  : No PREVIOUS_GTIDS (GTID wasn't enabled)
      master-bin.N+1: Has an empty PREVIOUS_GTIDS and a ROTATE
                      (GTID was enabled on startup)
      master-bin.N+2: Has a PREVIOUS_GTIDS with the content set by a
                      SET @@GLOBAL.GTID_PURGED + has GTIDs of some
                      transactions.

      If this 5.6 server be upgraded to 5.7 keeping its binary log files,
      this routine will have to find the first binary log that contains a
      PREVIOUS_GTIDS + a GTID event to ensure that the content of the
      GTID_PURGED will be correctly set (assuming binlog_gtid_simple_recovery
      is not enabled).
    */
    DBUG_PRINT("info", ("Iterating forwards through binary logs, looking for "
                        "the first binary log that contains both a "
                        "Previous_gtids_log_event and a Gtid_log_event."));
    assert(!is_relay_log);
    for (it = filename_list.begin(); it != filename_list.end(); it++) {
      /*
        We should pass a first_gtid to read_gtids_from_binlog when
        binlog_gtid_simple_recovery is disabled, or else it will return
        right after reading the PREVIOUS_GTIDS event to avoid stall on
        reading the whole binary log.
      */
      Gtid first_gtid = {0, 0};
      const char *filename = it->c_str();
      DBUG_PRINT("info", ("filename='%s'", filename));
      switch (read_gtids_from_binlog(
          filename, nullptr, lost_gtids,
          binlog_gtid_simple_recovery ? nullptr : &first_gtid, tsid_map,
          verify_checksum, is_relay_log)) {
        case ERROR: {
          error = 1;
          [[fallthrough]];
        }
        case GOT_GTIDS: {
          goto end;
        }
        case NO_GTIDS:
        case GOT_PREVIOUS_GTIDS: {
          /*
            Mysql server iterates forwards through binary logs, looking for
            the first binary log that contains both Previous_gtids_log_event
            and gtid_log_event for gathering the set of gtid_purged on server
            start. It also iterates forwards through binary logs, looking for
            the first binary log that contains both Previous_gtids_log_event
            and gtid_log_event for gathering the set of gtid_purged when
            purging binary logs. This may take very long time if it has many
            binary logs and almost all of them are out of filesystem cache.
            So if the binlog_gtid_simple_recovery is enabled, we just
            initialize GLOBAL.GTID_PURGED from the first binary log, do not
            read any more binary logs.
          */
          if (binlog_gtid_simple_recovery) goto end;
          [[fallthrough]];
        }
        case TRUNCATED: {
          break;
        }
      }
    }
  }
end:
  if (all_gtids) all_gtids->dbug_print("all_gtids");
  if (lost_gtids) lost_gtids->dbug_print("lost_gtids");
  if (need_lock) {
    tsid_lock->unlock();
    m_binlog_index_monitor.unlock();
    if (all_gtids != nullptr) mysql_mutex_unlock(&LOCK_log);
  }
  filename_list.clear();
  DBUG_PRINT("info", ("returning %d", error));
  return error != 0 ? true : false;
}

/**
  Open a (new) binlog file.

  - Open the log file and the index file. Register the new
  file name in it
  - When calling this when the file is in use, you must have a locks
  on LOCK_log and LOCK_index.

  @retval
    0	ok
  @retval
    1	error
*/

bool MYSQL_BIN_LOG::open_binlog(
    const char *log_name, const char *new_name, ulong max_size_arg,
    bool null_created_arg, bool need_lock_index, bool need_tsid_lock,
    Format_description_log_event *extra_description_event,
    uint32 new_index_number) {
  // lock_index must be acquired *before* tsid_lock.
  assert(need_tsid_lock || !need_lock_index);
  DBUG_TRACE;
  DBUG_PRINT("enter", ("base filename: %s", log_name));

  mysql_mutex_assert_owner(get_log_lock());

  if (init_and_set_log_file_name(log_name, new_name, new_index_number)) {
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_GENERATE_NEW_FILE_NAME);
    return true;
  }

  DBUG_PRINT("info", ("generated filename: %s", log_file_name));

  DEBUG_SYNC(current_thd, "after_log_file_name_initialized");

  if (m_binlog_index_monitor.open_purge_index_file(true) ||
      m_binlog_index_monitor.register_create_index_entry(log_file_name) ||
      m_binlog_index_monitor.sync_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_registering_index", 1, 0)) {
    /**
      @todo: although this was introduced to appease valgrind
      when injecting emulated faults using fault_injection_registering_index
      it may be good to consider what actually happens when
      open_purge_index_file succeeds but register or sync fails.

      Perhaps we might need the code below in MYSQL_BIN_LOG::cleanup
      for "real life" purposes as well?
    */
    DBUG_EXECUTE_IF("fault_injection_registering_index",
                    { m_binlog_index_monitor.end_close_purge_index_file(); });

    LogErr(ERROR_LEVEL, ER_BINLOG_FAILED_TO_SYNC_INDEX_FILE_IN_OPEN);
    return true;
  }
  DBUG_EXECUTE_IF("crash_create_non_critical_before_update_index",
                  DBUG_SUICIDE(););

  write_error = false;

  /* open the main log file */
  if (open(m_key_file_log, log_name, new_name, new_index_number)) {
    m_binlog_index_monitor.close_purge_index_file();
    return true; /* all warnings issued */
  }

  max_size = max_size_arg;

  bool write_file_name_to_index_file = false;

  Format_description_log_event s;

  if (m_binlog_file->is_empty()) {
    /*
      The binary log file was empty (probably newly created)
      This is the normal case and happens when the user doesn't specify
      an extension for the binary log files.
      In this case we write a standard header to it.
    */
    if (m_binlog_file->write(pointer_cast<const uchar *>(BINLOG_MAGIC),
                             BIN_LOG_HEADER_SIZE))
      goto err;
    bytes_written += BIN_LOG_HEADER_SIZE;
    write_file_name_to_index_file = true;
  }

  /*
    don't set LOG_EVENT_BINLOG_IN_USE_F for the relay log
  */
  if (!is_relay_log) {
    s.common_header->flags |= LOG_EVENT_BINLOG_IN_USE_F;
  }

  if (is_relay_log) {
    /* relay-log */
    if (relay_log_checksum_alg ==
        mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF) {
      /* inherit master's A descriptor if one has been received */
      if (opt_replica_sql_verify_checksum == 0)
        /* otherwise use slave's local preference of RL events verification */
        relay_log_checksum_alg = mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF;
      else
        relay_log_checksum_alg =
            static_cast<enum_binlog_checksum_alg>(binlog_checksum_options);
    }
  }

  if (!s.is_valid()) goto err;
  s.dont_set_created = null_created_arg;
  /* Set LOG_EVENT_RELAY_LOG_F flag for relay log's FD */
  if (is_relay_log) s.set_relay_log_event();
  if (write_event_to_binlog(&s)) goto err;
  /*
    We need to revisit this code and improve it.
    See further comments in the mysqld.
    /Alfranio
  */
  if (current_thd) {
    Checkable_rwlock *tsid_lock = nullptr;
    Gtid_set logged_gtids_binlog(global_tsid_map, global_tsid_lock);
    Gtid_set *previous_logged_gtids;

    if (is_relay_log) {
      previous_logged_gtids = previous_gtid_set_relaylog;
      tsid_lock = previous_gtid_set_relaylog->get_tsid_map()->get_tsid_lock();
    } else {
      previous_logged_gtids = &logged_gtids_binlog;
      tsid_lock = global_tsid_lock;
    }

    if (need_tsid_lock)
      tsid_lock->wrlock();
    else
      tsid_lock->assert_some_wrlock();

    if (!is_relay_log) {
      const Gtid_set *executed_gtids = gtid_state->get_executed_gtids();
      const Gtid_set *gtids_only_in_table =
          gtid_state->get_gtids_only_in_table();
      /* logged_gtids_binlog= executed_gtids - gtids_only_in_table */
      if (logged_gtids_binlog.add_gtid_set(executed_gtids) !=
          RETURN_STATUS_OK) {
        if (need_tsid_lock) tsid_lock->unlock();
        goto err;
      }
      logged_gtids_binlog.remove_gtid_set(gtids_only_in_table);
    }
    DBUG_PRINT("info", ("Generating PREVIOUS_GTIDS for %s file.",
                        is_relay_log ? "relaylog" : "binlog"));
    Previous_gtids_log_event prev_gtids_ev(previous_logged_gtids);
    if (is_relay_log) prev_gtids_ev.set_relay_log_event();
    if (need_tsid_lock) tsid_lock->unlock();
    if (write_event_to_binlog(&prev_gtids_ev)) goto err;
  } else  // !(current_thd)
  {
    /*
      If the slave was configured before server restart, the server will
      generate a new relay log file without having current_thd, but this
      new relay log file must have a PREVIOUS_GTIDS event as we now
      generate the PREVIOUS_GTIDS event always.

      This is only needed for relay log files because the server will add
      the PREVIOUS_GTIDS of binary logs (when current_thd==NULL) after
      server's GTID initialization.

      During server's startup at mysqld_main(), from the binary/relay log
      initialization point of view, it will:
      1) Call init_server_components() that will generate a new binary log
         file but won't write the PREVIOUS_GTIDS event yet;
      2) Initialize server's GTIDs;
      3) Write the binary log PREVIOUS_GTIDS;
      4) Call init_replica() in where the new relay log file will be created
         after initializing relay log's Retrieved_Gtid_Set;
    */
    if (is_relay_log) {
      Tsid_map *previous_gtid_tsid_map =
          previous_gtid_set_relaylog->get_tsid_map();
      Checkable_rwlock *tsid_lock = previous_gtid_tsid_map->get_tsid_lock();

      if (need_tsid_lock)
        tsid_lock->wrlock();
      else
        tsid_lock->assert_some_wrlock(); /* purecov: inspected */

      DBUG_PRINT("info", ("Generating PREVIOUS_GTIDS for relaylog file."));
      Previous_gtids_log_event prev_gtids_ev(previous_gtid_set_relaylog);
      prev_gtids_ev.set_relay_log_event();

      if (need_tsid_lock) tsid_lock->unlock();

      if (write_event_to_binlog(&prev_gtids_ev)) goto err;
    }
  }
  if (extra_description_event) {
    /*
      This is a relay log written to by the I/O slave thread.
      Write the event so that others can later know the format of this relay
      log.
      Note that this event is very close to the original event from the
      master (it has binlog version of the master, event types of the
      master), so this is suitable to parse the next relay log's event. It
      has been produced by
      Format_description_log_event::Format_description_log_event(char* buf,).
      Why don't we want to write the mi_description_event if this
      event is for format<4 (3.23 or 4.x): this is because in that case, the
      mi_description_event describes the data received from the
      master, but not the data written to the relay log (*conversion*),
      which is in format 4 (slave's).
    */
    /*
      Set 'created' to 0, so that in next relay logs this event does not
      trigger cleaning actions on the slave in
      Format_description_log_event::apply_event_impl().
    */
    extra_description_event->created = 0;
    /* Don't set log_pos in event header */
    extra_description_event->set_artificial_event();

    if (binary_event_serialize(extra_description_event, m_binlog_file))
      goto err;
    bytes_written += extra_description_event->common_header->data_written;
  }
  if (m_binlog_file->flush_and_sync()) goto err;

  if (write_file_name_to_index_file) {
    DBUG_EXECUTE_IF("crash_create_critical_before_update_index",
                    DBUG_SUICIDE(););
    assert(m_binlog_index_monitor.is_inited_index_file() != 0);

    /*
      The new log file name is appended into crash safe index file after
      all the content of index file is copied into the crash safe index
      file. Then move the crash safe index file to index file.
    */
    DBUG_EXECUTE_IF("simulate_disk_full_on_open_binlog",
                    { DBUG_SET("+d,simulate_no_free_space_error"); });
    if (DBUG_EVALUATE_IF("fault_injection_updating_index", 1, 0) ||
        m_binlog_index_monitor.add_log_to_index(
            (uchar *)log_file_name, strlen(log_file_name), need_lock_index)) {
      DBUG_EXECUTE_IF("simulate_disk_full_on_open_binlog", {
        DBUG_SET("-d,simulate_file_write_error");
        DBUG_SET("-d,simulate_no_free_space_error");
        DBUG_SET("-d,simulate_disk_full_on_open_binlog");
      });
      goto err;
    }

    DBUG_EXECUTE_IF("crash_create_after_update_index", DBUG_SUICIDE(););
  }

  atomic_log_state = LOG_OPENED;
  /*
    At every rotate memorize the last transaction counter state to use it as
    offset at logging the transaction logical timestamps.
  */
  m_dependency_tracker.rotate();

  m_binlog_index_monitor.close_purge_index_file();

  update_binlog_end_pos();
  return false;

err:
  if (m_binlog_index_monitor.is_inited_purge_index_file())
    purge_index_entry(nullptr, nullptr, need_lock_index);
  m_binlog_index_monitor.close_purge_index_file();
  if (binlog_error_action == ABORT_SERVER) {
    exec_binlog_error_action_abort(
        "Either disk is full, file system is read only or "
        "there was an encryption error while opening the binlog. "
        "Aborting the server.");
  } else {
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_USE_FOR_LOGGING,
           (new_name) ? new_name : name, errno);
    close(LOG_CLOSE_INDEX, false, need_lock_index);
  }
  return true;
}

int MYSQL_BIN_LOG::get_current_log(Log_info *linfo,
                                   bool need_lock_log /*true*/) {
  if (need_lock_log) mysql_mutex_lock(&LOCK_log);
  int ret = raw_get_current_log(linfo);
  if (need_lock_log) mysql_mutex_unlock(&LOCK_log);
  return ret;
}

int MYSQL_BIN_LOG::raw_get_current_log(Log_info *linfo) {
  strmake(linfo->log_file_name, log_file_name,
          sizeof(linfo->log_file_name) - 1);
  linfo->pos = m_binlog_file->position();
  linfo->encrypted_header_size = m_binlog_file->get_encrypted_header_size();
  return 0;
}

bool MYSQL_BIN_LOG::check_write_error(const THD *thd) {
  DBUG_TRACE;

  bool checked = false;

  if (!thd->is_error()) return checked;

  switch (thd->get_stmt_da()->mysql_errno()) {
    case ER_TRANS_CACHE_FULL:
    case ER_STMT_CACHE_FULL:
    case ER_ERROR_ON_WRITE:
    case ER_BINLOG_LOGGING_IMPOSSIBLE:
      checked = true;
      break;
  }
  DBUG_PRINT("return", ("checked: %s", YESNO(checked)));
  return checked;
}

void MYSQL_BIN_LOG::report_cache_write_error(THD *thd, bool is_transactional) {
  DBUG_TRACE;

  write_error = true;

  if (check_write_error(thd)) return;

  if (my_errno() == EFBIG) {
    if (is_transactional) {
      my_error(ER_TRANS_CACHE_FULL, MYF(MY_WME));
    } else {
      my_error(ER_STMT_CACHE_FULL, MYF(MY_WME));
    }
  } else {
    char errbuf[MYSYS_STRERROR_SIZE];
    my_error(ER_ERROR_ON_WRITE, MYF(MY_WME), name, errno,
             my_strerror(errbuf, sizeof(errbuf), errno));
  }
}

int MYSQL_BIN_LOG::find_log_pos(Log_info *linfo, const char *log_name,
                                bool need_lock_index) {
  return m_binlog_index_monitor.find_log_pos(linfo, log_name, need_lock_index);
}

int MYSQL_BIN_LOG::find_next_log(Log_info *linfo, bool need_lock_index) {
  return m_binlog_index_monitor.find_next_log(linfo, need_lock_index);
}

int MYSQL_BIN_LOG::find_next_relay_log(char log_name[FN_REFLEN + 1]) {
  return m_binlog_index_monitor.find_next_relay_log(log_name);
}

std::pair<int, std::list<std::string>> MYSQL_BIN_LOG::get_log_index(
    bool need_lock_index) {
  return m_binlog_index_monitor.get_log_index(need_lock_index);
}

/**
  Removes files, as part of a RESET BINARY LOGS AND GTIDS or RESET REPLICA
  statement, by deleting all logs referred to in the index file and the index
  file. Then, it creates a new index file and a new log file.

  The new index file will only contain the new log file.

  @param thd Thread
  @param delete_only If true, do not create a new index file and
  a new log file.

  @note
    If not called from slave thread, write start event to new log

  @retval
    0	ok
  @retval
    1   error
*/
bool MYSQL_BIN_LOG::reset_logs(THD *thd, bool delete_only) {
  Log_info linfo;
  bool error = false;
  int err;
  const char *save_name = nullptr;
  Checkable_rwlock *tsid_lock = nullptr;
  DBUG_TRACE;

  /*
    Flush logs for storage engines, so that the last transaction
    is persisted inside storage engines.
  */
  assert(!thd->is_log_reset());
  thd->set_log_reset();
  if (ha_flush_logs()) {
    thd->clear_log_reset();
    return true;
  }
  thd->clear_log_reset();

  ha_reset_logs(thd);

  /*
    We need to get both locks to be sure that no one is trying to
    write to the index log file.
  */
  mysql_mutex_lock(&LOCK_log);
  m_binlog_index_monitor.lock();

  if (is_relay_log)
    tsid_lock = previous_gtid_set_relaylog->get_tsid_map()->get_tsid_lock();
  else
    tsid_lock = global_tsid_lock;
  tsid_lock->wrlock();

  /* Save variables so that we can reopen the log */
  save_name = name;
  name = nullptr;  // Protect against free
  close(LOG_CLOSE_TO_BE_OPENED, false /*need_lock_log=false*/,
        false /*need_lock_index=false*/);

  /*
    First delete all old log files and then update the index file.
    As we first delete the log files and do not use sort of logging,
    a crash may lead to an inconsistent state where the index has
    references to non-existent files.

    We need to invert the steps and use the purge_index_file methods
    in order to make the operation safe.
  */

  if ((err = find_log_pos(&linfo, NullS, false /*need_lock_index=false*/)) !=
      0) {
    uint errcode = purge_log_get_error_code(err);
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_LOCATE_OLD_BINLOG_OR_RELAY_LOG_FILES);
    my_error(errcode, MYF(0));
    error = true;
    goto err;
  }

  for (;;) {
    if ((error = my_delete_allow_opened(linfo.log_file_name, MYF(0))) != 0) {
      if (my_errno() == ENOENT) {
        push_warning_printf(
            current_thd, Sql_condition::SL_WARNING, ER_LOG_PURGE_NO_FILE,
            ER_THD(current_thd, ER_LOG_PURGE_NO_FILE), linfo.log_file_name);
        LogErr(INFORMATION_LEVEL, ER_BINLOG_CANT_DELETE_FILE,
               linfo.log_file_name);
        set_my_errno(0);
        error = false;
      } else {
        push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                            ER_BINLOG_PURGE_FATAL_ERR,
                            "a problem with deleting %s; "
                            "consider examining correspondence "
                            "of your binlog index file "
                            "to the actual binlog files",
                            linfo.log_file_name);
        LogErr(ERROR_LEVEL, ER_BINLOG_CANT_DELETE_FILE, linfo.log_file_name);
        my_error(ER_BINLOG_PURGE_FATAL_ERR, MYF(0));
        error = true;
        goto err;
      }
    }
    if (find_next_log(&linfo, false /*need_lock_index=false*/)) break;
  }

  /* Start logging with a new file */
  close(LOG_CLOSE_INDEX | LOG_CLOSE_TO_BE_OPENED, false /*need_lock_log=false*/,
        false /*need_lock_index=false*/);
  if ((error = my_delete_allow_opened(m_binlog_index_monitor.get_index_fname(),
                                      MYF(0))))  // Reset (open will update)
  {
    if (my_errno() == ENOENT) {
      push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                          ER_LOG_PURGE_NO_FILE,
                          ER_THD(current_thd, ER_LOG_PURGE_NO_FILE),
                          m_binlog_index_monitor.get_index_fname());
      LogErr(INFORMATION_LEVEL, ER_BINLOG_CANT_DELETE_FILE,
             m_binlog_index_monitor.get_index_fname());
      set_my_errno(0);
      error = false;
    } else {
      push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                          ER_BINLOG_PURGE_FATAL_ERR,
                          "a problem with deleting %s; "
                          "consider examining correspondence "
                          "of your binlog index file "
                          "to the actual binlog files",
                          m_binlog_index_monitor.get_index_fname());
      LogErr(ERROR_LEVEL, ER_BINLOG_CANT_DELETE_FILE,
             m_binlog_index_monitor.get_index_fname());
      my_error(ER_BINLOG_PURGE_FATAL_ERR, MYF(0));
      error = true;
      goto err;
    }
  }
  DBUG_EXECUTE_IF("wait_for_kill_gtid_state_clear", {
    const char action[] = "now WAIT_FOR kill_gtid_state_clear";
    assert(!debug_sync_set_action(thd, STRING_WITH_LEN(action)));
  };);

  /*
    For relay logs we clear the gtid state associated per channel(i.e rli)
    in the purge_relay_logs()
  */
  if (!is_relay_log) {
    if (gtid_state->clear(thd)) {
      error = true;
    }
    /*
      Don't clear global_tsid_map because gtid_state->clear() above didn't
      touched owned_gtids GTID set.
    */
    error = error || gtid_state->init();
  }

  if (!delete_only) {
    if (!open_index_file(m_binlog_index_monitor.get_index_fname(), nullptr,
                         false /*need_lock_index=false*/))
      error = open_binlog(save_name, nullptr, max_size, false,
                          false /*need_lock_index=false*/,
                          false /*need_tsid_lock=false*/, nullptr,
                          thd->lex->next_binlog_file_nr) ||
              error;
  }
  /* String has been duplicated, free old file-name */
  if (name != nullptr) {
    my_free(const_cast<char *>(save_name));
    save_name = nullptr;
  }

err:
  if (name == nullptr)
    name = const_cast<char *>(save_name);  // restore old file-name
  tsid_lock->unlock();
  m_binlog_index_monitor.unlock();
  mysql_mutex_unlock(&LOCK_log);
  return error;
}

int MYSQL_BIN_LOG::remove_logs_outside_range_from_index(
    const std::string &first, const std::string &last) {
  return m_binlog_index_monitor.remove_logs_outside_range_from_index(
      first, is_relay_log, last);
}

/**
  Remove all logs before the given log from disk and from the index file.

  @param to_log	      Delete all log file name before this file.
  @param included            If true, to_log is deleted too.
  @param need_lock_index     Set to true, if the lock_index of the binary log
  shall be acquired, false if the called is already the owner of the lock_index.
  @param need_update_threads If we want to update the log coordinates of
                             all threads. False for relay logs, true otherwise.
  @param decrease_log_space  If not null, decrement this variable of
                             the amount of log space freed
  @param auto_purge          True if this is an automatic purge.

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval 0			ok
  @retval LOG_INFO_EOF		to_log not found
  @retval LOG_INFO_EMFILE       too many files opened
  @retval LOG_INFO_FATAL        if any other than ENOENT error from
                                mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs(const char *to_log, bool included,
                              bool need_lock_index, bool need_update_threads,
                              ulonglong *decrease_log_space, bool auto_purge) {
  int error = 0, no_of_log_files_to_purge = 0, no_of_log_files_purged = 0;
  int no_of_threads_locking_log = 0;
  bool exit_loop = false;
  Log_info log_info;
  THD *thd = current_thd;
  DBUG_TRACE;
  DBUG_PRINT("info", ("to_log= %s", to_log));

  if (need_lock_index)
    m_binlog_index_monitor.lock();
  else
    m_binlog_index_monitor.assert_owner();
  if ((error =
           find_log_pos(&log_info, to_log, false /*need_lock_index=false*/))) {
    LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_CALLED_WITH_FILE_NOT_IN_INDEX,
           to_log);
    goto err;
  }

  DEBUG_SYNC(thd, "before_purge_logs");

  no_of_log_files_to_purge = log_info.entry_index;

  if ((error = m_binlog_index_monitor.open_purge_index_file(true))) {
    LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_CANT_SYNC_INDEX_FILE);
    goto err;
  }

  /*
    File name exists in index file; delete until we find this file
    or a file that is used.
  */
  if ((error = find_log_pos(&log_info, NullS, false /*need_lock_index=false*/)))
    goto err;

  while ((compare_log_name(to_log, log_info.log_file_name) ||
          (exit_loop = included))) {
    if (is_active(log_info.log_file_name)) {
      if (!auto_purge)
        push_warning_printf(
            thd, Sql_condition::SL_WARNING, ER_WARN_PURGE_LOG_IS_ACTIVE,
            ER_THD(thd, ER_WARN_PURGE_LOG_IS_ACTIVE), log_info.log_file_name);
      break;
    }

    if ((no_of_threads_locking_log = log_in_use(log_info.log_file_name))) {
      if (!auto_purge)
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_WARN_PURGE_LOG_IN_USE,
                            ER_THD(thd, ER_WARN_PURGE_LOG_IN_USE),
                            log_info.log_file_name, no_of_threads_locking_log,
                            no_of_log_files_purged, no_of_log_files_to_purge);
      break;
    }
    no_of_log_files_purged++;

    if ((error = m_binlog_index_monitor.register_purge_index_entry(
             log_info.log_file_name))) {
      LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_CANT_COPY_TO_REGISTER_FILE,
             log_info.log_file_name);
      goto err;
    }

    if (find_next_log(&log_info, false /*need_lock_index=false*/) || exit_loop)
      break;
  }

  DBUG_EXECUTE_IF("crash_purge_before_update_index", DBUG_SUICIDE(););

  if ((error = m_binlog_index_monitor.sync_purge_index_file())) {
    LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_CANT_FLUSH_REGISTER_FILE);
    goto err;
  }

  /* We know how many files to delete. Update index file. */
  if ((error = m_binlog_index_monitor.remove_logs_outside_range_from_index(
           &log_info, need_update_threads))) {
    LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_CANT_UPDATE_INDEX_FILE);
    goto err;
  }

  // Update gtid_state->lost_gtids
  if (!is_relay_log) {
    global_tsid_lock->wrlock();
    error = init_gtid_sets(
        nullptr, const_cast<Gtid_set *>(gtid_state->get_lost_gtids()),
        opt_source_verify_checksum, false /*false=don't need lock*/,
        nullptr /*trx_parser*/, nullptr /*partial_trx*/);
    global_tsid_lock->unlock();
    if (error) goto err;
  }

  DBUG_EXECUTE_IF("crash_purge_critical_after_update_index", DBUG_SUICIDE(););

err:

  int error_index = 0, close_error_index = 0;
  /* Read each entry from purge_index_file and delete the file. */
  if (!error && m_binlog_index_monitor.is_inited_purge_index_file() &&
      (error_index = purge_index_entry(thd, decrease_log_space,
                                       false /*need_lock_index=false*/)))
    LogErr(ERROR_LEVEL, ER_BINLOG_PURGE_LOGS_FAILED_TO_PURGE_LOG);

  close_error_index = m_binlog_index_monitor.close_purge_index_file();

  DBUG_EXECUTE_IF("crash_purge_non_critical_after_update_index",
                  DBUG_SUICIDE(););

  if (need_lock_index) m_binlog_index_monitor.unlock();

  /*
    Error codes from purge logs take precedence.
    Then error codes from purging the index entry.
    Finally, error codes from closing the purge index file.
  */
  error = error ? error : (error_index ? error_index : close_error_index);

  return error;
}

int MYSQL_BIN_LOG::purge_index_entry(THD *thd, ulonglong *decrease_log_space,
                                     bool need_lock_index) {
  return m_binlog_index_monitor.purge_index_entry(
      thd, decrease_log_space, m_key_file_log, need_lock_index);
}

/**
  Remove all logs before the given file date from disk and from the
  index file.

  @param purge_time	Delete all log files before given date.
  @param auto_purge     True if this is an automatic purge.

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0				ok
  @retval
    LOG_INFO_PURGE_NO_ROTATE	Binary file that can't be rotated
    LOG_INFO_FATAL              if any other than ENOENT error from
                                mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs_before_date(time_t purge_time, bool auto_purge) {
  int error;
  int no_of_threads_locking_log = 0, no_of_log_files_purged = 0;
  bool log_is_active = false, log_is_in_use = false;
  char to_log[FN_REFLEN], copy_log_in_use[FN_REFLEN];
  Log_info log_info;
  MY_STAT stat_area;
  THD *thd = current_thd;

  DBUG_TRACE;

  m_binlog_index_monitor.lock();
  to_log[0] = 0;

  if ((error = find_log_pos(&log_info, NullS, false /*need_lock_index=false*/)))
    goto err;

  while (!(log_is_active = is_active(log_info.log_file_name))) {
    if (!mysql_file_stat(m_key_file_log, log_info.log_file_name, &stat_area,
                         MYF(0))) {
      if (my_errno() == ENOENT) {
        /*
          It's not fatal if we can't stat a log file that does not exist.
        */
        set_my_errno(0);
      } else {
        /*
          Other than ENOENT are fatal
        */
        if (thd) {
          push_warning_printf(thd, Sql_condition::SL_WARNING,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        } else {
          LogErr(INFORMATION_LEVEL, ER_BINLOG_FAILED_TO_DELETE_LOG_FILE,
                 log_info.log_file_name);
        }
        error = LOG_INFO_FATAL;
        goto err;
      }
    }
    /* check if the binary log file is older than the purge_time
       if yes check if it is in use, if not in use then add
       it in the list of binary log files to be purged.
    */
    else if (stat_area.st_mtime < purge_time) {
      if ((no_of_threads_locking_log = log_in_use(log_info.log_file_name))) {
        if (!auto_purge) {
          log_is_in_use = true;
          strcpy(copy_log_in_use, log_info.log_file_name);
        }
        break;
      }
      strmake(to_log, log_info.log_file_name,
              sizeof(log_info.log_file_name) - 1);
      no_of_log_files_purged++;
    } else
      break;
    if (find_next_log(&log_info, false /*need_lock_index=false*/)) break;
  }

  if (log_is_active) {
    if (!auto_purge)
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_WARN_PURGE_LOG_IS_ACTIVE,
          ER_THD(thd, ER_WARN_PURGE_LOG_IS_ACTIVE), log_info.log_file_name);
  }

  if (log_is_in_use) {
    int no_of_log_files_to_purge = no_of_log_files_purged + 1;
    while (strcmp(log_file_name, log_info.log_file_name)) {
      if (mysql_file_stat(m_key_file_log, log_info.log_file_name, &stat_area,
                          MYF(0))) {
        if (stat_area.st_mtime < purge_time)
          no_of_log_files_to_purge++;
        else
          break;
      }
      if (find_next_log(&log_info, false /*need_lock_index=false*/)) {
        no_of_log_files_to_purge++;
        break;
      }
    }

    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_WARN_PURGE_LOG_IN_USE,
                        ER_THD(thd, ER_WARN_PURGE_LOG_IN_USE), copy_log_in_use,
                        no_of_threads_locking_log, no_of_log_files_purged,
                        no_of_log_files_to_purge);
  }

  error = (to_log[0] ? purge_logs(to_log, true, false /*need_lock_index=false*/,
                                  true /*need_update_threads=true*/,
                                  (ulonglong *)nullptr, auto_purge)
                     : 0);

err:
  m_binlog_index_monitor.unlock();
  return error;
}

/**
  Create a new log file name.

  @param[out] buf       Buffer allocated with at least FN_REFLEN bytes where
                        new name is stored.
  @param      log_ident Identity of the binary/relay log.

  @note
    If file name will be longer then FN_REFLEN it will be truncated
*/

void MYSQL_BIN_LOG::make_log_name(char *buf, const char *log_ident) {
  size_t dir_len = dirname_length(log_file_name);
  if (dir_len >= FN_REFLEN) dir_len = FN_REFLEN - 1;
  my_stpnmov(buf, log_file_name, dir_len);
  strmake(buf + dir_len, log_ident, FN_REFLEN - dir_len - 1);
}

/**
  Check if we are writing/reading to the given log file.
*/

bool MYSQL_BIN_LOG::is_active(const char *log_file_name_arg) const {
  return !compare_log_name(log_file_name, log_file_name_arg);
}

void MYSQL_BIN_LOG::inc_prep_xids(THD *thd) {
  DBUG_TRACE;
#ifndef NDEBUG
  int result = ++m_atomic_prep_xids;
  DBUG_PRINT("debug", ("m_atomic_prep_xids: %d", result));
#else
  m_atomic_prep_xids++;
#endif
  thd->get_transaction()->m_flags.xid_written = true;
}

void MYSQL_BIN_LOG::dec_prep_xids(THD *thd) {
  DBUG_TRACE;
  int32 result = --m_atomic_prep_xids;
  DBUG_PRINT("debug", ("m_atomic_prep_xids: %d", result));
  thd->get_transaction()->m_flags.xid_written = false;
  if (result == 0) {
    mysql_mutex_lock(&LOCK_xids);
    mysql_cond_signal(&m_prep_xids_cond);
    mysql_mutex_unlock(&LOCK_xids);
  }
}

void MYSQL_BIN_LOG::wait_for_prep_xids() {
  DBUG_TRACE;
  mysql_mutex_lock(&LOCK_xids);
  while (get_prep_xids() > 0) {
    mysql_cond_wait(&m_prep_xids_cond, &LOCK_xids);
  }
  mysql_mutex_unlock(&LOCK_xids);
}

/*
  Wrappers around new_file_impl to avoid using argument
  to control locking. The argument 1) less readable 2) breaks
  incapsulation 3) allows external access to the class without
  a lock (which is not possible with private new_file_without_locking
  method).

  @retval
    nonzero - error

*/

int MYSQL_BIN_LOG::new_file(
    Format_description_log_event *extra_description_event) {
  return new_file_impl(true /*need_lock_log=true*/, extra_description_event);
}

/*
  @retval
    nonzero - error
*/
int MYSQL_BIN_LOG::new_file_without_locking(
    Format_description_log_event *extra_description_event) {
  return new_file_impl(false /*need_lock_log=false*/, extra_description_event);
}

/**
  Start writing to a new log file or reopen the old file.

  @param need_lock_log If true, this function acquires LOCK_log;
  otherwise the caller should already have acquired it.

  @param extra_description_event The master's FDE to be written by the I/O
  thread while creating a new relay log file. This should be NULL for
  binary log files.

  @retval 0 success
  @retval nonzero - error

  @note The new file name is stored last in the index file
*/
int MYSQL_BIN_LOG::new_file_impl(
    bool need_lock_log, Format_description_log_event *extra_description_event) {
  int error = 0;
  bool close_on_error = false;
  char new_name[FN_REFLEN], *new_name_ptr = nullptr, *old_name;
  const char *file_to_open;
  const size_t ERR_CLOSE_MSG_LEN = 1024;
  char close_on_error_msg[ERR_CLOSE_MSG_LEN];
  memset(close_on_error_msg, 0, sizeof close_on_error_msg);

  DBUG_TRACE;
  if (!is_open()) {
    DBUG_PRINT("info", ("log is closed"));
    return error;
  }

  if (need_lock_log)
    mysql_mutex_lock(&LOCK_log);
  else
    mysql_mutex_assert_owner(&LOCK_log);
  DBUG_EXECUTE_IF("semi_sync_3-way_deadlock",
                  DEBUG_SYNC(current_thd, "before_rotate_binlog"););
  /*
    We need to ensure that the number of prepared XIDs are 0.

    If m_atomic_prep_xids is not zero:
    - We wait for storage engine commit, hence decrease m_atomic_prep_xids
    - We keep the LOCK_log to block new transactions from being
      written to the binary log.
   */
  wait_for_prep_xids();

  m_binlog_index_monitor.lock();

  mysql_mutex_assert_owner(&LOCK_log);
  m_binlog_index_monitor.assert_owner();

  if (DBUG_EVALUATE_IF("expire_logs_always", 0, 1) &&
      (error = ha_flush_logs(true))) {
    goto end;
  }

  if (!is_relay_log) {
    /* Save set of GTIDs of the last binlog into table on binlog rotation */
    if ((error = gtid_state->save_gtids_of_last_binlog_into_table())) {
      if (error == ER_RPL_GTID_TABLE_CANNOT_OPEN) {
        close_on_error =
            m_binlog_file->get_real_file_size() >=
                static_cast<my_off_t>(max_size) ||
            DBUG_EVALUATE_IF("simulate_max_binlog_size", true, false);

        if (!close_on_error) {
          LogErr(ERROR_LEVEL, ER_BINLOG_UNABLE_TO_ROTATE_GTID_TABLE_READONLY,
                 "Current binlog file was flushed to disk and will be kept in "
                 "use.");
        } else {
          snprintf(close_on_error_msg, sizeof close_on_error_msg,
                   ER_THD(current_thd, ER_RPL_GTID_TABLE_CANNOT_OPEN), "mysql",
                   "gtid_executed");

          if (binlog_error_action != ABORT_SERVER)
            LogErr(WARNING_LEVEL,
                   ER_BINLOG_UNABLE_TO_ROTATE_GTID_TABLE_READONLY,
                   "Binary logging going to be disabled.");
        }

        DBUG_EXECUTE_IF("gtid_executed_readonly",
                        { DBUG_SET("-d,gtid_executed_readonly"); });
        DBUG_EXECUTE_IF("simulate_max_binlog_size",
                        { DBUG_SET("-d,simulate_max_binlog_size"); });
      } else {
        close_on_error = true;
        snprintf(close_on_error_msg, sizeof close_on_error_msg, "%s",
                 ER_THD(current_thd, ER_OOM_SAVE_GTIDS));
      }
      goto end;
    }
  }

  /*
    If user hasn't specified an extension, generate a new log name
    We have to do this here and not in open as we want to store the
    new file name in the current binary log file.
  */
  new_name_ptr = new_name;
  if ((error = generate_new_name(new_name, name))) {
    // Use the old name if generation of new name fails.
    strcpy(new_name, name);
    close_on_error = true;
    snprintf(close_on_error_msg, sizeof close_on_error_msg,
             ER_THD(current_thd, ER_NO_UNIQUE_LOGFILE), name);
    if (strlen(close_on_error_msg)) {
      close_on_error_msg[strlen(close_on_error_msg) - 1] = '\0';
    }
    goto end;
  }

  /*
    Make sure that the log_file is initialized before writing
    Rotate_log_event into it.
  */
  if (m_binlog_file->is_open()) {
    /*
      We log the whole file name for log file as the user may decide
      to change base names at some point.
    */
    Rotate_log_event r(new_name + dirname_length(new_name), 0, LOG_EVENT_OFFSET,
                       is_relay_log ? Rotate_log_event::RELAY_LOG : 0);

    if (DBUG_EVALUATE_IF("fault_injection_new_file_rotate_event", (error = 1),
                         false) ||
        (error = write_event_to_binlog(&r))) {
      char errbuf[MYSYS_STRERROR_SIZE];
      DBUG_EXECUTE_IF("fault_injection_new_file_rotate_event", errno = 2;);
      close_on_error = true;
      snprintf(close_on_error_msg, sizeof close_on_error_msg,
               ER_THD(current_thd, ER_ERROR_ON_WRITE), name, errno,
               my_strerror(errbuf, sizeof(errbuf), errno));
      my_printf_error(ER_ERROR_ON_WRITE, ER_THD(current_thd, ER_ERROR_ON_WRITE),
                      MYF(ME_FATALERROR), name, errno,
                      my_strerror(errbuf, sizeof(errbuf), errno));
      goto end;
    }

    if ((error = m_binlog_file->flush())) {
      close_on_error = true;
      snprintf(close_on_error_msg, sizeof close_on_error_msg, "%s",
               "Either disk is full or file system is read only");
      goto end;
    }
  }

  DEBUG_SYNC(current_thd, "after_rotate_event_appended");

  old_name = name;
  name = nullptr;  // Don't free name
  close(LOG_CLOSE_TO_BE_OPENED | LOG_CLOSE_INDEX, false /*need_lock_log=false*/,
        false /*need_lock_index=false*/);

  if (checksum_alg_reset != mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF) {
    assert(!is_relay_log);
    assert(binlog_checksum_options != checksum_alg_reset);
    binlog_checksum_options = checksum_alg_reset;
  }
  /*
    Note that at this point, atomic_log_state != LOG_CLOSED
    (important for is_open()).
  */
  DBUG_EXECUTE_IF("binlog_crash_between_close_and_open", { DBUG_SUICIDE(); });
  DEBUG_SYNC(current_thd, "binlog_rotate_between_close_and_open");
  /*
    new_file() is only used for rotation (in FLUSH LOGS or because size >
    max_binlog_size or max_relay_log_size).
    If this is a binary log, the Format_description_log_event at the beginning
    of the new file should have created=0 (to distinguish with the
    Format_description_log_event written at server startup, which should
    trigger temp tables deletion on slaves.
  */

  /* reopen index binlog file, BUG#34582 */
  file_to_open = m_binlog_index_monitor.get_index_fname();
  error = open_index_file(m_binlog_index_monitor.get_index_fname(), nullptr,
                          false /*need_lock_index=false*/);
  if (!error) {
    /* reopen the binary log file. */
    file_to_open = new_name_ptr;
    error = open_binlog(old_name, new_name_ptr, max_size,
                        true /*null_created_arg=true*/,
                        false /*need_lock_index=false*/,
                        true /*need_tsid_lock=true*/, extra_description_event);
  }

  /* handle reopening errors */
  if (error) {
    char errbuf[MYSYS_STRERROR_SIZE];
    my_printf_error(ER_CANT_OPEN_FILE, ER_THD(current_thd, ER_CANT_OPEN_FILE),
                    MYF(ME_FATALERROR), file_to_open, error,
                    my_strerror(errbuf, sizeof(errbuf), error));
    close_on_error = true;
    snprintf(close_on_error_msg, sizeof close_on_error_msg,
             ER_THD(current_thd, ER_CANT_OPEN_FILE), file_to_open, error,
             my_strerror(errbuf, sizeof(errbuf), error));
  }
  my_free(old_name);

end:

  if (error && close_on_error /* rotate, flush or reopen failed */) {
    /*
      Close whatever was left opened.

      We are keeping the behavior as it exists today, ie,
      we disable logging and move on (see: BUG#51014).

      TODO: as part of WL#1790 consider other approaches:
       - kill mysql (safety);
       - try multiple locations for opening a log file;
       - switch server to protected/readonly mode
       - ...
    */
    if (binlog_error_action == ABORT_SERVER) {
      char abort_msg[ERR_CLOSE_MSG_LEN + 48];
      memset(abort_msg, 0, sizeof abort_msg);
      snprintf(abort_msg, sizeof abort_msg,
               "%s, while rotating the binlog. "
               "Aborting the server",
               close_on_error_msg);
      exec_binlog_error_action_abort(abort_msg);
    } else
      LogErr(ERROR_LEVEL, ER_BINLOG_CANT_OPEN_FOR_LOGGING,
             new_name_ptr != nullptr ? new_name_ptr : "new file", errno);

    close(LOG_CLOSE_INDEX, false /*need_lock_log=false*/,
          false /*need_lock_index=false*/);
  }

  m_binlog_index_monitor.unlock();
  if (need_lock_log) mysql_mutex_unlock(&LOCK_log);

  DEBUG_SYNC(current_thd, "after_disable_binlog");
  return error;
}

/**
  Called after an event has been written to the relay log by the IO
  thread.  This flushes and possibly syncs the file (according to the
  sync options), rotates the file if it has grown over the limit, and
  finally calls signal_update().

  @note The caller must hold LOCK_log before invoking this function.

  @param mi Master_info for the IO thread.

  @retval false success
  @retval true error
*/
bool MYSQL_BIN_LOG::after_write_to_relay_log(Master_info *mi) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("max_size: %lu", max_size));

  // Check pre-conditions
  mysql_mutex_assert_owner(&LOCK_log);
  assert(is_relay_log);

  /*
    We allow the relay log rotation by relay log size
    only if the trx parser is not inside a transaction.
  */
  bool can_rotate = mi->transaction_parser.is_not_inside_transaction();

#ifndef NDEBUG
  if (m_binlog_file->get_real_file_size() >
          DBUG_EVALUATE_IF("rotate_replica_debug_group", 500, max_size) &&
      !can_rotate) {
    DBUG_PRINT("info", ("Postponing the rotation by size waiting for "
                        "the end of the current transaction."));
  }
#endif

  // Flush and sync
  bool error = flush_and_sync(false);
  if (error) {
    mi->report(ERROR_LEVEL, ER_REPLICA_RELAY_LOG_WRITE_FAILURE,
               ER_THD(current_thd, ER_REPLICA_RELAY_LOG_WRITE_FAILURE),
               "failed to flush event to relay log file");
    truncate_relaylog_file(mi, atomic_binlog_end_pos);
  } else {
    if (can_rotate) {
      mysql_mutex_lock(&mi->data_lock);
      /*
        If the last event of the transaction has been flushed, we can add
        the GTID (if it is not empty) to the logged set, or else it will
        not be available in the Previous GTIDs of the next relay log file
        if we are going to rotate the relay log.
      */
      const Gtid *last_gtid_queued = mi->get_queueing_trx_gtid();
      if (!last_gtid_queued->is_empty()) {
        mi->rli->get_tsid_lock()->rdlock();
        DBUG_SIGNAL_WAIT_FOR(current_thd, "updating_received_transaction_set",
                             "reached_updating_received_transaction_set",
                             "continue_updating_received_transaction_set");
        mi->rli->add_logged_gtid(last_gtid_queued->sidno,
                                 last_gtid_queued->gno);
        mi->rli->get_tsid_lock()->unlock();
      }

      if (mi->is_queueing_trx()) {
        mi->finished_queueing();

        Trx_monitoring_info processing;
        Trx_monitoring_info last;
        mi->get_gtid_monitoring_info()->copy_info_to(&processing, &last);

        // update the compression information
        binlog::global_context.monitoring_context()
            .transaction_compression()
            .update(binlog::monitoring::log_type::RELAY, last.compression_type,
                    last.gtid, last.end_time, last.compressed_bytes,
                    last.uncompressed_bytes,
                    mi->rli->get_gtid_set()->get_tsid_map());
      }
      mysql_mutex_unlock(&mi->data_lock);

      /*
        If relay log is too big, rotate. But only if not in the middle of a
        transaction when GTIDs are enabled.

        Also rotate if a deferred flush request has been placed.

        We now try to mimic the following master binlog behavior: "A transaction
        is written in one chunk to the binary log, so it is never split between
        several binary logs. Therefore, if you have big transactions, you might
        see binary log files larger than max_binlog_size."
      */
      if (m_binlog_file->get_real_file_size() >
              DBUG_EVALUATE_IF("rotate_replica_debug_group", 500, max_size) ||
          mi->is_rotate_requested()) {
        error = new_file_without_locking(mi->get_mi_description_event());
        mi->clear_rotate_requests();
      }
    }
  }

  lock_binlog_end_pos();
  mi->rli->ign_master_log_name_end[0] = 0;
  update_binlog_end_pos(false /*need_lock*/);
  harvest_bytes_written(mi->rli, true /*need_log_space_lock=true*/);
  unlock_binlog_end_pos();

  return error;
}

bool MYSQL_BIN_LOG::truncate_update_log_file(const char *log_name,
                                             my_off_t valid_pos,
                                             my_off_t binlog_size,
                                             bool update) {
  std::unique_ptr<Binlog_ofile> ofile(
      Binlog_ofile::open_existing(key_file_binlog, log_name, MYF(MY_WME)));

  if (!ofile) {
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_OPEN_CRASHED_BINLOG);
    return false;
  }

  /* Change binlog file size to valid_pos */
  if (valid_pos < binlog_size) {
    if (ofile->truncate(valid_pos)) {
      LogErr(ERROR_LEVEL, ER_BINLOG_CANT_TRIM_CRASHED_BINLOG);
      return false;
    }
    LogErr(INFORMATION_LEVEL, ER_BINLOG_CRASHED_BINLOG_TRIMMED, log_name,
           binlog_size, valid_pos, valid_pos);
  }

  if (update) {
    /* Clear LOG_EVENT_BINLOG_IN_USE_F */
    uchar flags = 0;
    if (ofile->update(&flags, 1, BIN_LOG_HEADER_SIZE + FLAGS_OFFSET)) {
      LogErr(ERROR_LEVEL, ER_BINLOG_CANT_CLEAR_IN_USE_FLAG_FOR_CRASHED_BINLOG);
      return false;
    }
  }

  return true;
}

bool MYSQL_BIN_LOG::write_event(Log_event *ev, Master_info *mi) {
  DBUG_TRACE;

  DBUG_EXECUTE_IF("fail_to_write_ignored_event_to_relay_log", { return true; });
  // check preconditions
  assert(is_relay_log);

  mysql_mutex_assert_owner(&LOCK_log);

  // write data
  bool error = false;
  if (!binary_event_serialize(ev, m_binlog_file)) {
    bytes_written += ev->common_header->data_written;
    error = after_write_to_relay_log(mi);
  } else {
    mi->report(ERROR_LEVEL, ER_REPLICA_RELAY_LOG_WRITE_FAILURE,
               ER_THD(current_thd, ER_REPLICA_RELAY_LOG_WRITE_FAILURE),
               "failed to write event to the relay log file");
    truncate_relaylog_file(mi, atomic_binlog_end_pos);
    error = true;
  }

  return error;
}

bool MYSQL_BIN_LOG::write_buffer(const char *buf, uint len, Master_info *mi) {
  DBUG_TRACE;

  // check preconditions
  assert(is_relay_log);
  mysql_mutex_assert_owner(&LOCK_log);

  // write data
  bool error = false;
  if (m_binlog_file->write(pointer_cast<const uchar *>(buf), len) == 0) {
    bytes_written += len;
    error = after_write_to_relay_log(mi);
  } else {
    mi->report(ERROR_LEVEL, ER_REPLICA_RELAY_LOG_WRITE_FAILURE,
               ER_THD(current_thd, ER_REPLICA_RELAY_LOG_WRITE_FAILURE),
               "failed to write event to the relay log file");
    truncate_relaylog_file(mi, atomic_binlog_end_pos);
    error = true;
  }

  return error;
}

bool MYSQL_BIN_LOG::flush() {
  return m_binlog_file->is_open() && m_binlog_file->flush();
}

bool MYSQL_BIN_LOG::flush_and_sync(const bool force) {
  mysql_mutex_assert_owner(&LOCK_log);

  if (m_binlog_file->flush()) return true;

  std::pair<bool, bool> result = sync_binlog_file(force);

  return result.first;
}

void MYSQL_BIN_LOG::start_union_events(THD *thd, query_id_t query_id_param) {
  assert(!thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union = true;
  thd->binlog_evt_union.unioned_events = false;
  thd->binlog_evt_union.unioned_events_trans = false;
  thd->binlog_evt_union.first_query_id = query_id_param;
}

void MYSQL_BIN_LOG::stop_union_events(THD *thd) {
  assert(thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union = false;
}

bool MYSQL_BIN_LOG::is_query_in_union(THD *thd, query_id_t query_id_param) {
  return (thd->binlog_evt_union.do_union &&
          query_id_param >= thd->binlog_evt_union.first_query_id);
}

/*
  Updates thd's position-of-next-event variables
  after a *real* write a file.
 */
void MYSQL_BIN_LOG::update_thd_next_event_pos(THD *thd) {
  if (likely(thd != nullptr)) {
    thd->set_next_event_pos(log_file_name, m_binlog_file->position());
  }
}

/*
  Moves the last bunch of rows from the pending Rows event to a cache (either
  transactional cache if is_transaction is @c true, or the non-transactional
  cache otherwise. Sets a new pending event.

  @param thd               a pointer to the user thread.
  @param event             a pointer to the row event.
  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
*/
int MYSQL_BIN_LOG::flush_and_set_pending_rows_event(THD *thd,
                                                    Rows_log_event *event,
                                                    bool is_transactional) {
  DBUG_TRACE;
  assert(mysql_bin_log.is_open());
  DBUG_PRINT("enter", ("event: %p", event));

  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);

  assert(cache_mngr);

  binlog_cache_data *const cache_data =
      cache_mngr->get_binlog_cache_data(is_transactional);

  DBUG_PRINT("info", ("cache_mngr->pending(): %p", cache_data->pending()));

  if (Rows_log_event *pending = cache_data->pending()) {
    /*
      Write pending event to the cache.
    */
    if (cache_data->write_event(pending)) {
      report_cache_write_error(thd, is_transactional);
      if (check_write_error(thd) && stmt_cannot_safely_rollback(thd))
        cache_data->set_incident();
      delete pending;
      cache_data->set_pending(nullptr);
      return 1;
    }

    delete pending;
  }

  cache_data->set_pending(event);

  return 0;
}

/**
  Write an event to the binary log cache.
*/

bool MYSQL_BIN_LOG::write_event(Log_event *event_info) {
  THD *const thd = event_info->thd;
  constexpr bool error = true;
  DBUG_TRACE;

  if (thd->binlog_evt_union.do_union) {
    /*
      In Stored function; Remember that function call caused an update.
      We will log the function call to the binary log on function exit
    */
    thd->binlog_evt_union.unioned_events = true;
    thd->binlog_evt_union.unioned_events_trans |=
        event_info->is_using_trans_cache();
    return false;
  }

  /*
    We only end the statement if we are in a top-level statement.  If
    we are inside a stored function, we do not end the statement since
    this will close all tables on the slave. But there can be a special case
    where we are inside a stored function/trigger and a SAVEPOINT is being
    set in side the stored function/trigger. This SAVEPOINT execution will
    force the pending event to be flushed without an STMT_END_F flag. This
    will result in a case where following DMLs will be considered as part of
    same statement and result in data loss on slave. Hence in this case we
    force the end_stmt to be true.
  */
  bool const end_stmt =
      (thd->in_sub_stmt && thd->lex->sql_command == SQLCOM_SAVEPOINT)
          ? true
          : (thd->locked_tables_mode && thd->lex->requires_prelocking());
  if (thd->binlog_flush_pending_rows_event(end_stmt,
                                           event_info->is_using_trans_cache()))
    return error;

  /*
     In most cases this is only called if 'is_open()' is true; in fact this is
     mostly called if is_open() *was* true a few instructions before, but it
     could have changed since.
  */
  if (likely(is_open())) {
    /*
      In the future we need to add to the following if tests like
      "do the involved tables match (to be implemented)
      binlog_[wild_]{do|ignore}_table?" (WL#1049)"
    */
    const char *local_db = event_info->get_db();
    if (!(thd->variables.option_bits & OPTION_BIN_LOG) ||
        (thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
         thd->lex->sql_command != SQLCOM_SAVEPOINT &&
         (!event_info->is_no_filter_event() &&
          !binlog_filter->db_ok(local_db))))
      return false;

    assert(event_info->is_using_trans_cache() ||
           event_info->is_using_stmt_cache());

    if (binlog_start_trans_and_stmt(thd, event_info)) return error;

    const bool is_trans_cache = event_info->is_using_trans_cache();
    binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(thd);
    binlog_cache_data *const cache_data =
        cache_mngr->get_binlog_cache_data(is_trans_cache);

    DBUG_PRINT("info", ("event type: %d", event_info->get_type_code()));

    /*
       No check for auto events flag here - this write method should
       never be called if auto-events are enabled.

       Write first log events which describe the 'run environment'
       of the SQL command. If row-based binlogging, Insert_id, Rand
       and other kind of "setting context" events are not needed.
    */
    if (!thd->is_current_stmt_binlog_format_row()) {
      if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt) {
        Intvar_log_event e(
            thd,
            (uchar)mysql::binlog::event::Intvar_event::LAST_INSERT_ID_EVENT,
            thd->first_successful_insert_id_in_prev_stmt_for_binlog,
            event_info->event_cache_type, event_info->event_logging_type);
        if (cache_data->write_event(&e)) goto err;
      }
      if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0) {
        DBUG_PRINT(
            "info",
            ("number of auto_inc intervals: %u",
             thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements()));
        Intvar_log_event e(
            thd, (uchar)mysql::binlog::event::Intvar_event::INSERT_ID_EVENT,
            thd->auto_inc_intervals_in_cur_stmt_for_binlog.minimum(),
            event_info->event_cache_type, event_info->event_logging_type);
        if (cache_data->write_event(&e)) goto err;
      }
      if (thd->rand_used) {
        Rand_log_event e(thd, thd->rand_saved_seed1, thd->rand_saved_seed2,
                         event_info->event_cache_type,
                         event_info->event_logging_type);
        if (cache_data->write_event(&e)) goto err;
      }
      if (!thd->user_var_events.empty()) {
        for (size_t i = 0; i < thd->user_var_events.size(); i++) {
          Binlog_user_var_event *user_var_event = thd->user_var_events[i];

          /* setting flags for user var log event */
          uchar flags = User_var_log_event::UNDEF_F;
          if (user_var_event->unsigned_flag)
            flags |= User_var_log_event::UNSIGNED_F;

          User_var_log_event e(
              thd, user_var_event->user_var_event->entry_name.ptr(),
              user_var_event->user_var_event->entry_name.length(),
              user_var_event->value, user_var_event->length,
              user_var_event->type, user_var_event->charset_number, flags,
              event_info->event_cache_type, event_info->event_logging_type);
          if (cache_data->write_event(&e)) goto err;
        }
      }
    }

    /*
      Write the event.
    */
    if (cache_data->write_event(event_info)) goto err;

    if (DBUG_EVALUATE_IF("injecting_fault_writing", 1, 0)) goto err;

    /*
      After writing the event, if the trx-cache was used and any unsafe
      change was written into it, the cache is marked as cannot safely
      roll back.
    */
    if (is_trans_cache && stmt_cannot_safely_rollback(thd))
      cache_mngr->trx_cache.set_cannot_rollback();

    return false;

  err:
    report_cache_write_error(thd, is_trans_cache);
    if (check_write_error(thd) && stmt_cannot_safely_rollback(thd))
      cache_data->set_incident();
  }

  return error;
}

/**
  The method executes rotation when LOCK_log is already acquired
  by the caller.

  @param force_rotate  caller can request the log rotation
  @param check_purge   is set to true if rotation took place

  @note
    If rotation fails, for instance the server was unable
    to create a new log file, we still try to write an
    incident event to the current log.

  @note The caller must hold LOCK_log when invoking this function.

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate(bool force_rotate, bool *check_purge) {
  int error = 0;
  DBUG_TRACE;

  assert(!is_relay_log);
  mysql_mutex_assert_owner(&LOCK_log);

  *check_purge = false;

  if (DBUG_EVALUATE_IF("force_rotate", 1, 0) || force_rotate ||
      (m_binlog_file->get_real_file_size() >= (my_off_t)max_size) ||
      DBUG_EVALUATE_IF("simulate_max_binlog_size", true, false)) {
    error = new_file_without_locking(nullptr);
    *check_purge = true;
  }
  return error;
}

void MYSQL_BIN_LOG::auto_purge_at_server_startup() {
  // first run the auto purge validations
  if (check_auto_purge_conditions()) return;

  auto purge_time = calculate_auto_purge_lower_time_bound();
  constexpr auto auto_purge{true};
  purge_logs_before_date(purge_time, auto_purge);
}

/**
  The method executes logs purging routine.
*/
void MYSQL_BIN_LOG::auto_purge() {
  // first run the auto purge validations
  if (check_auto_purge_conditions()) return;

  // then we run the purge validations
  // if we run into out of memory, execute binlog_error_action_abort
  const auto [is_invalid_purge, purge_error] = check_purge_conditions(*this);
  if (is_invalid_purge) {
    if (purge_error == LOG_INFO_MEM) {
      /* purecov: begin inspected */
      // OOM
      exec_binlog_error_action_abort(
          "Out of memory happened while checking if "
          "instance was locked for backup");
      /* purecov: end */
    } else if (purge_error == LOG_INFO_BACKUP_LOCK) {
      LogErr(WARNING_LEVEL, ER_LOG_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK);
    }
    return;
  }

  // lock BACKUP lock for the duration of PURGE operation
  Shared_backup_lock_guard backup_lock{current_thd};
  switch (backup_lock) {
    case Shared_backup_lock_guard::Lock_result::locked:
      break;
    case Shared_backup_lock_guard::Lock_result::not_locked: {
      LogErr(WARNING_LEVEL, ER_LOG_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK);
      return;
    }
    case Shared_backup_lock_guard::Lock_result::oom: {
      exec_binlog_error_action_abort(ER_OUT_OF_RESOURCES_MSG);
      return;
    }
  }

  DEBUG_SYNC(current_thd, "at_purge_logs_before_date");

  auto purge_time = calculate_auto_purge_lower_time_bound();
  constexpr auto auto_purge{true};
  /*
    Flush logs for storage engines, so that the last transaction
    is persisted inside storage engines.
  */
  ha_flush_logs();
  purge_logs_before_date(purge_time, auto_purge);
}

/**
  Execute a FLUSH LOGS statement.

  The method is a shortcut of @c rotate() and @c purge().
  LOCK_log is acquired prior to rotate and is released after it.

  @param thd           Current session.
  @param force_rotate  caller can request the log rotation

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate_and_purge(THD *thd, bool force_rotate) {
  int error = 0;
  DBUG_TRACE;
  bool check_purge = false;

  /*
    FLUSH BINARY LOGS command should ignore 'read-only', 'super_read_only' and
    the transaction READ ONLY mode options so that it can update
    'mysql.gtid_executed' replication repository table.
  */
  thd->set_skip_readonly_check();
  /*
    Wait for handlerton to insert any pending information into the binlog.
    For e.g. ha_ndbcluster which updates the binlog asynchronously this is
    needed so that the user see its own commands in the binlog.
  */
  ha_binlog_wait(thd);

  assert(!is_relay_log);
  mysql_mutex_lock(&LOCK_log);
  error = rotate(force_rotate, &check_purge);
  /*
    NOTE: Run purge_logs wo/ holding LOCK_log because it does not need
          the mutex. Otherwise causes various deadlocks.
  */
  mysql_mutex_unlock(&LOCK_log);

  if (!error && check_purge) auto_purge();

  return error;
}

uint MYSQL_BIN_LOG::next_file_id() {
  uint res;
  mysql_mutex_lock(&LOCK_log);
  res = file_id++;
  mysql_mutex_unlock(&LOCK_log);
  return res;
}

int MYSQL_BIN_LOG::get_gtid_executed(Tsid_map *tsid_map, Gtid_set *gtid_set) {
  DBUG_TRACE;
  int error = 0;

  mysql_mutex_lock(&mysql_bin_log.LOCK_commit);
  global_tsid_lock->wrlock();

  enum_return_status return_status = global_tsid_map->copy(tsid_map);
  if (return_status != RETURN_STATUS_OK) {
    error = 1;
    goto end;
  }

  return_status = gtid_set->add_gtid_set(gtid_state->get_executed_gtids());
  if (return_status != RETURN_STATUS_OK) error = 1;

end:
  global_tsid_lock->unlock();
  mysql_mutex_unlock(&mysql_bin_log.LOCK_commit);

  return error;
}

void MYSQL_BIN_LOG::register_log_info(Log_info *log_info) {
  DBUG_TRACE;
  MUTEX_LOCK(lock, &LOCK_log_info);
  m_binlog_index_monitor.register_log_info(log_info);
}

void MYSQL_BIN_LOG::unregister_log_info(Log_info *log_info) {
  DBUG_TRACE;
  MUTEX_LOCK(lock, &LOCK_log_info);
  m_binlog_index_monitor.unregister_log_info(log_info);
}

int MYSQL_BIN_LOG::log_in_use(const char *log_name) {
  DBUG_TRACE;
#ifndef NDEBUG
  if (current_thd)
    DEBUG_SYNC(current_thd, "purge_logs_after_lock_index_before_thread_count");
#endif
  MUTEX_LOCK(lock_log_info, &LOCK_log_info);
  return m_binlog_index_monitor.log_in_use(log_name);
}

void MYSQL_BIN_LOG::adjust_linfo_offsets(my_off_t purge_offset) {
  DBUG_TRACE;
  MUTEX_LOCK(lock_log_info, &LOCK_log_info);
  m_binlog_index_monitor.adjust_linfo_offsets(purge_offset);
}

/**
  Write the contents of the given IO_CACHE to the binary log.

  The cache will be reset as a READ_CACHE to be able to read the
  contents from it.

  The data will be post-processed: see class Binlog_event_writer for
  details.

  @param cache Events will be read from this IO_CACHE.
  @param writer Events will be written to this Binlog_event_writer.

  @retval true IO error.
  @retval false Success.

  @see MYSQL_BIN_LOG::write_cache
*/
bool MYSQL_BIN_LOG::do_write_cache(Binlog_cache_storage *cache,
                                   Binlog_event_writer *writer) {
  DBUG_TRACE;

  DBUG_EXECUTE_IF("simulate_do_write_cache_failure", {
    /*
       see binlog_cache_data::write_event() that reacts on
       @c simulate_disk_full_at_flush_pending.
    */
    DBUG_SET("-d,simulate_do_write_cache_failure");
    return true;
  });

#ifndef NDEBUG
  uint64 expected_total_len = cache->length();
  DBUG_PRINT("info", ("bytes in cache= %" PRIu64, expected_total_len));
#endif

  bool error = false;
  if (cache->copy_to(writer, &error)) {
    if (error) report_binlog_write_error();
    return true;
  }
  return false;
}

bool MYSQL_BIN_LOG::write_stmt_directly(THD *thd, const char *stmt,
                                        size_t stmt_len,
                                        enum_sql_command sql_command) {
  bool ret = false;
  /* backup the original command */
  enum_sql_command save_sql_command = thd->lex->sql_command;
  thd->lex->sql_command = sql_command;

  if (thd->binlog_query(THD::STMT_QUERY_TYPE, stmt, stmt_len, false, false,
                        false, 0) ||
      commit(thd, false) != TC_LOG::RESULT_SUCCESS) {
    ret = true;
  }

  thd->lex->sql_command = save_sql_command;
  return ret;
}

/*
  Write the event into current binlog directly without going though a session
  binlog cache. It will update the event's log_pos and set checksum accordingly.
  binary_event_serialize can be called directly if log_pos should not be
  updated.
*/
inline bool MYSQL_BIN_LOG::write_event_to_binlog(Log_event *ev) {
  ev->common_footer->checksum_alg =
      is_relay_log
          ? relay_log_checksum_alg
          : static_cast<enum_binlog_checksum_alg>(binlog_checksum_options);
  assert(ev->common_footer->checksum_alg !=
         mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF);

  /*
    Stores current position into log_pos, it is used to calculate correctly
    end_log_pos by adding data_written in Log_event::write_header().
  */
  ev->common_header->log_pos = m_binlog_file->position();

  if (binary_event_serialize(ev, m_binlog_file)) return true;

  add_bytes_written(ev->common_header->data_written);
  return false;
}

/* Write the event into current binlog and flush and sync */
bool MYSQL_BIN_LOG::write_event_to_binlog_and_sync(Log_event *ev) {
  if (write_event_to_binlog(ev) || m_binlog_file->flush() ||
      m_binlog_file->sync())
    return true;

  update_binlog_end_pos();
  return false;
}

/**
  Write the contents of the statement or transaction cache to the binary log.

  Comparison with do_write_cache:

  - do_write_cache is a lower-level function that only performs the
    actual write.

  - write_cache is a higher-level function that calls do_write_cache
    and additionally performs some maintenance tasks, including:
    - report any errors that occurred
    - update gtid_state
    - update thd.binlog_next_event_pos

  @param thd Thread variable

  @param cache_data Events will be read from the IO_CACHE of this
  cache_data object.

  @param writer Events will be written to this Binlog_event_writer.

  @retval true IO error.
  @retval false Success.

  @note We only come here if there is something in the cache.
  @note Whatever is in the cache is always a complete transaction.
  @note 'cache' needs to be reinitialized after this functions returns.
*/
bool MYSQL_BIN_LOG::write_cache(THD *thd, binlog_cache_data *cache_data,
                                Binlog_event_writer *writer) {
  DBUG_TRACE;

  Binlog_cache_storage *const cache = cache_data->get_cache();

  mysql_mutex_assert_owner(&LOCK_log);

  assert(is_open());
  if (likely(is_open()))  // Should always be true
  {
    /*
      We only bother to write to the binary log if there is anything
      to write.

      @todo Is this check redundant? Probably this is only called if
      there is anything in the cache (see @note in comment above this
      function). Check if we can replace this by an assertion. /Sven
    */
    if (!cache->is_empty()) {
      DBUG_EXECUTE_IF("crash_before_writing_xid", {
        if (do_write_cache(cache, writer))
          DBUG_PRINT("info", ("error writing binlog cache: %d", write_error));
        flush_and_sync(true);
        DBUG_PRINT("info", ("crashing before writing xid"));
        DBUG_SUICIDE();
      });
      if (do_write_cache(cache, writer)) goto err;

      DBUG_EXECUTE_IF("half_binlogged_transaction", DBUG_SUICIDE(););
    }
    update_thd_next_event_pos(thd);
  }

  return false;

err:
  thd->commit_error = THD::CE_FLUSH_ERROR;

  return true;
}

void MYSQL_BIN_LOG::report_binlog_write_error() {
  char errbuf[MYSYS_STRERROR_SIZE];

  write_error = true;
  LogErr(ERROR_LEVEL, ER_FAILED_TO_WRITE_TO_FILE, name, errno,
         my_strerror(errbuf, sizeof(errbuf), errno));
}

int MYSQL_BIN_LOG::wait_for_update() {
  DBUG_TRACE;
  mysql_mutex_assert_owner(&LOCK_binlog_end_pos);
  mysql_cond_wait(&update_cond, &LOCK_binlog_end_pos);
  return 0;
}

int MYSQL_BIN_LOG::wait_for_update(const std::chrono::nanoseconds &timeout) {
  DBUG_TRACE;

  struct timespec ts;
  set_timespec_nsec(&ts, timeout.count());
  mysql_mutex_assert_owner(&LOCK_binlog_end_pos);
  return mysql_cond_timedwait(&update_cond, &LOCK_binlog_end_pos, &ts);
}

/**
  Close the log file.

  @param exiting     Bitmask for one or more of the following bits:
          - LOG_CLOSE_INDEX : if we should close the index file
          - LOG_CLOSE_TO_BE_OPENED : if we intend to call open
                                     at once after close.
          - LOG_CLOSE_STOP_EVENT : write a 'stop' event to the log

  @param need_lock_log If true, this function acquires LOCK_log;
  otherwise the caller should already have acquired it.

  @param need_lock_index If true, this function acquires LOCK_index;
  otherwise the caller should already have acquired it.

  @note
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_BIN_LOG::close(
    uint exiting, bool need_lock_log,
    bool need_lock_index) {  // One can't set log_type here!
  DBUG_TRACE;
  DBUG_PRINT("enter", ("exiting: %d", (int)exiting));
  if (need_lock_log)
    mysql_mutex_lock(&LOCK_log);
  else
    mysql_mutex_assert_owner(&LOCK_log);

  if (atomic_log_state == LOG_OPENED) {
    if ((exiting & LOG_CLOSE_STOP_EVENT) != 0) {
      /**
        TODO(WL#7546): Change the implementation to Stop_event after write() is
        moved into libbinlogevents
      */
      Stop_log_event s;
      // the checksumming rule for relay-log case is similar to Rotate
      s.common_footer->checksum_alg =
          is_relay_log
              ? relay_log_checksum_alg
              : static_cast<enum_binlog_checksum_alg>(binlog_checksum_options);
      assert(!is_relay_log ||
             relay_log_checksum_alg !=
                 mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF);
      if (!write_event_to_binlog(&s) && !m_binlog_file->flush())
        update_binlog_end_pos();
    }

    /* The following update should not be done in relay log files */
    if (!is_relay_log) {
      my_off_t offset = BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
      uchar flags = 0;  // clearing LOG_EVENT_BINLOG_IN_USE_F
      (void)m_binlog_file->update(&flags, 1, offset);
    }

    if (m_binlog_file->flush_and_sync() && !write_error) {
      report_binlog_write_error();
    }

    /*
      LOCK_sync to guarantee that no thread is calling m_binlog_file
      to sync data to disk when another thread is closing m_binlog_file.
    */
    if (!is_relay_log) mysql_mutex_lock(&LOCK_sync);
    m_binlog_file->close();
    if (!is_relay_log) mysql_mutex_unlock(&LOCK_sync);

    atomic_log_state =
        (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
    my_free(name);
    name = nullptr;
  }

  /*
    The following test is needed even if is_open() is not set, as we may have
    called a not complete close earlier and the index file is still open.
  */
  if (exiting & LOG_CLOSE_INDEX) {
    if (m_binlog_index_monitor.close_index_file(need_lock_index) < 0 &&
        !write_error) {
      report_binlog_write_error();
    }
  }

  atomic_log_state =
      (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  my_free(name);
  name = nullptr;

  if (need_lock_log) mysql_mutex_unlock(&LOCK_log);
}

void MYSQL_BIN_LOG::harvest_bytes_written(Relay_log_info *rli,
                                          bool need_log_space_lock) {
#ifndef NDEBUG
  char buf1[22], buf2[22];
#endif

  DBUG_TRACE;
  if (need_log_space_lock)
    mysql_mutex_lock(&rli->log_space_lock);
  else
    mysql_mutex_assert_owner(&rli->log_space_lock);
  rli->log_space_total += bytes_written;
  DBUG_PRINT("info",
             ("relay_log_space: %s  bytes_written: %s",
              llstr(rli->log_space_total, buf1), llstr(bytes_written, buf2)));
  bytes_written = 0;
  if (need_log_space_lock) mysql_mutex_unlock(&rli->log_space_lock);
}

void MYSQL_BIN_LOG::set_max_size(ulong max_size_arg) {
  /*
    We need to take locks, otherwise this may happen:
    new_file() is called, calls open(old_max_size), then before open() starts,
    set_max_size() sets max_size to max_size_arg, then open() starts and
    uses the old_max_size argument, so max_size_arg has been overwritten and
    it's like if the SET command was never run.
  */
  DBUG_TRACE;
  mysql_mutex_lock(&LOCK_log);
  if (is_open()) max_size = max_size_arg;
  mysql_mutex_unlock(&LOCK_log);
}

/****** transaction coordinator log for 2pc - binlog() based solution *******/

bool MYSQL_BIN_LOG::read_binlog_in_use_flag(
    Binlog_file_reader &binlog_file_reader) {
  std::unique_ptr<Log_event> ev(binlog_file_reader.read_event_object());
  if (!ev) {
    my_off_t binlog_size = binlog_file_reader.ifile()->length();
    LogErr(ERROR_LEVEL, ER_READ_LOG_EVENT_FAILED,
           binlog_file_reader.get_error_str(), binlog_size,
           mysql::binlog::event::UNKNOWN_EVENT);
    return false;
  }

  if (ev->get_type_code() != mysql::binlog::event::FORMAT_DESCRIPTION_EVENT) {
    my_off_t valid_pos = 0;
    const char *binlog_file_name =
        binlog_file_reader.ifile()->file_name().c_str();
    LogErr(ERROR_LEVEL, ER_BINLOG_CRASH_RECOVERY_MALFORMED_LOG,
           binlog_file_name, valid_pos, binlog_file_reader.position(),
           Log_event::get_type_str(ev->get_type_code()));
    return false;
  }

  if (!(ev->common_header->flags & LOG_EVENT_BINLOG_IN_USE_F ||
        DBUG_EVALUATE_IF("eval_force_bin_log_recovery", true, false))) {
    LogErr(INFORMATION_LEVEL, ER_BINLOG_CANT_OPEN_CRASHED_BINLOG);
    return false;
  }

  return true;
}

int MYSQL_BIN_LOG::open_binlog(const char *opt_name) {
  Log_info log_info;
  int error = 1;

  /*
    This function is used for 2pc transaction coordination.  Hence, it
    is never used for relay logs.
  */
  assert(!is_relay_log);
  assert(total_ha_2pc > 1 || (1 == total_ha_2pc && opt_bin_log));
  assert(opt_name && opt_name[0]);

  if (!m_binlog_index_monitor.is_inited_index_file()) {
    /* There was a failure to open the index file, can't open the binlog */
    cleanup();
    return 1;
  }

  if (using_heuristic_recover()) {
    /* generate a new binlog to mask a corrupted one */
    mysql_mutex_lock(&LOCK_log);
    open_binlog(opt_name, nullptr, max_binlog_size, false,
                true /*need_lock_index=true*/, true /*need_tsid_lock=true*/,
                nullptr);
    mysql_mutex_unlock(&LOCK_log);
    cleanup();
    return 1;
  }

  if ((error = find_log_pos(&log_info, NullS, true /*need_lock_index=true*/))) {
    if (error != LOG_INFO_EOF)
      LogErr(ERROR_LEVEL, ER_BINLOG_CANT_FIND_LOG_IN_INDEX, error);
    else {
      /* should execute ha_recover */
      error = ha_recover();
      if (error)
        LogErr(ERROR_LEVEL, ER_BINLOG_CRASH_RECOVERY_ERROR_RETURNED_SE);
    }
    return error;
  }

  char log_name[FN_REFLEN];

  do {
    strmake(log_name, log_info.log_file_name, sizeof(log_name) - 1);
  } while (!(error = find_next_log(&log_info, true /*need_lock_index=true*/)));

  if (error != LOG_INFO_EOF) {
    LogErr(ERROR_LEVEL, ER_BINLOG_CANT_FIND_LOG_IN_INDEX, error);
    return error;
  }

  Binlog_file_reader binlog_file_reader(opt_source_verify_checksum);
  if (binlog_file_reader.open(log_name)) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FILE_OPEN_FAILED,
           binlog_file_reader.get_error_str());
    return 1;
  }

  /*
    If the binary log was not properly closed it means that the server
    may have crashed. In that case, we need to call
    binlog::Binlog_recovery::recover()
    to:
      a) collect logged XIDs;
      b) complete the 2PC of the pending XIDs;
      c) collect the last valid position.

    Therefore, we do need to iterate over the binary log, even if
    total_ha_2pc == 1, to find the last valid group of events written.
    Later we will take this value and truncate the log if need be.
  */
  if (!read_binlog_in_use_flag(binlog_file_reader)) {
    /* should execute ha_recover */
    error = ha_recover();
    if (error) LogErr(ERROR_LEVEL, ER_BINLOG_CRASH_RECOVERY_ERROR_RETURNED_SE);
    return error;
  }

  LogErr(INFORMATION_LEVEL, ER_BINLOG_RECOVERING_AFTER_CRASH_USING, opt_name);

  binlog::Binlog_recovery bl_recovery{binlog_file_reader};
  bl_recovery.recover();

  my_off_t valid_pos = bl_recovery.get_valid_pos();
  my_off_t binlog_size = binlog_file_reader.ifile()->length();

  if (bl_recovery.is_binlog_malformed()) {
    LogErr(ERROR_LEVEL, ER_BINLOG_CRASH_RECOVERY_MALFORMED_LOG, log_name,
           valid_pos, binlog_file_reader.position(),
           bl_recovery.get_failure_message().data());
    return 1;
  }
  if (bl_recovery.has_engine_recovery_failed()) {
    /* truncate log file but do not clear LOG_EVENT_IN_USE_F flag */
    LogErr(ERROR_LEVEL, ER_BINLOG_CRASH_RECOVERY_ERROR_RETURNED_SE);
    if (!truncate_update_log_file(log_name, valid_pos, binlog_size, false)) {
      /* log error has been written */
    }
    return 1;
  }

  /* Trim the crashed binlog file to last valid transaction
     or event (non-transaction) base on valid_pos. */
  if (valid_pos > 0) {
    /* truncate log file and clear LOG_EVENT_IN_USE_F flag */
    if (!truncate_update_log_file(log_name, valid_pos, binlog_size, true)) {
      /* log error has been written */
      return 1;
    }
  }  // end if (valid_pos > 0)

  return 0;
}

/**
 Truncate the active relay log file in the specified position.

  @param mi Master_info of the channel going to truncate the relay log file.
  @param truncate_pos The position to truncate the active relay log file.
  @return False on success and true on failure.
*/
bool MYSQL_BIN_LOG::truncate_relaylog_file(Master_info *mi,
                                           my_off_t truncate_pos) {
  DBUG_TRACE;
  assert(is_relay_log);
  mysql_mutex_assert_owner(&LOCK_log);
  Relay_log_info *rli = mi->rli;
  bool error = false;

  /*
    If the relay log was closed by an error (binlog_error_action=IGNORE_ERROR)
    this truncate function should produce no result as the relay log is already
    in really bad shape.
  */
  if (!is_open()) {
    return false;
  }

  my_off_t relaylog_file_size = m_binlog_file->position();

  if (truncate_pos > 0 && truncate_pos < relaylog_file_size) {
    if (m_binlog_file->truncate(truncate_pos)) {
      mi->report(ERROR_LEVEL, ER_REPLICA_RELAY_LOG_WRITE_FAILURE,
                 ER_THD(current_thd, ER_REPLICA_RELAY_LOG_WRITE_FAILURE),
                 "failed to truncate relay log file");
      error = true;
    } else {
      LogErr(INFORMATION_LEVEL, ER_REPLICA_RELAY_LOG_TRUNCATE_INFO,
             log_file_name, relaylog_file_size, truncate_pos);

      // Re-init the SQL thread IO_CACHE
      assert(strcmp(rli->get_event_relay_log_name(), log_file_name) ||
             rli->get_event_relay_log_pos() <= truncate_pos);
      rli->notify_relay_log_truncated();
    }
  }
  return error;
}

/** This is called on shutdown, after ha_panic. */
void MYSQL_BIN_LOG::close() {}

/*
  Prepare the transaction in the transaction coordinator.

  This function will prepare the transaction in the storage engines
  (by calling @c ha_prepare_low) what will write a prepare record
  to the log buffers.

  @retval 0    success
  @retval 1    error
*/
int MYSQL_BIN_LOG::prepare(THD *thd, bool all) {
  DBUG_TRACE;

  assert(opt_bin_log);

  /*
    Set HA_IGNORE_DURABILITY to not flush the prepared record of the
    transaction to the log of storage engine (for example, InnoDB
    redo log) during the prepare phase. So that we can flush prepared
    records of transactions to the log of storage engine in a group
    right before flushing them to binary log during binlog group
    commit flush stage. Reset to HA_REGULAR_DURABILITY at the
    beginning of parsing next command.
  */
  thd->durability_property = HA_IGNORE_DURABILITY;

  CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_prepare_in_engines");
  int error = ha_prepare_low(thd, all);

  CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("after_ha_prepare_low");
  // Invoke `commit` if we're dealing with `XA PREPARE` in order to use BCG
  // to write the event to file.
  if (!error && all && is_xa_prepare(thd)) return this->commit(thd, true);

  return error;
}

/**
  Commit the transaction in the transaction coordinator.

  This function will commit the sessions transaction in the binary log
  and in the storage engines (by calling @c ha_commit_low). If the
  transaction was successfully logged (or not successfully unlogged)
  but the commit in the engines did not succeed, there is a risk of
  inconsistency between the engines and the binary log.

  For binary log group commit, the commit is separated into three
  parts:

  1. First part consists of filling the necessary caches and
     finalizing them (if they need to be finalized). After this,
     nothing is added to any of the caches.

  2. Second part execute an ordered flush and commit. This will be
     done using the group commit functionality in ordered_commit.

  3. Third part checks any errors resulting from the ordered commit
     and handles them appropriately.

  @retval RESULT_SUCCESS   success
  @retval RESULT_ABORTED   error, transaction was neither logged nor committed
  @retval RESULT_INCONSISTENT  error, transaction was logged but not committed
*/
TC_LOG::enum_result MYSQL_BIN_LOG::commit(THD *thd, bool all) {
  DBUG_TRACE;
  DBUG_PRINT("info",
             ("query='%s'", thd == current_thd ? thd->query().str : nullptr));
  Transaction_ctx *trn_ctx = thd->get_transaction();
  my_xid xid = trn_ctx->xid_state()->get_xid()->get_my_xid();
  bool stmt_stuff_logged = false;
  bool trx_stuff_logged = false;
  bool skip_commit = is_loggable_xa_prepare(thd);
  bool is_atomic_ddl = false;
  auto xs = thd->get_transaction()->xid_state();
  raii::Sentry<> reset_detached_guard{[&]() -> void {
    // XID_STATE may have been used to hold metadata for a detached transaction.
    // In that case, we need to reset it.
    if (xs->is_detached()) xs->reset();
  }};

  if (thd->lex->sql_command ==
      SQLCOM_XA_COMMIT) {  // XA commit must be written to the binary log prior
                           // to retrieving cache manager
    DBUG_EXECUTE_IF("simulate_xa_commit_log_abort", { return RESULT_ABORTED; });
    if (this->write_xa_to_cache(thd)) return RESULT_ABORTED;
  }

  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
  DBUG_PRINT("enter", ("thd: 0x%llx, all: %s, xid: %llu, cache_mngr: 0x%llx",
                       (ulonglong)thd, YESNO(all), (ulonglong)xid,
                       (ulonglong)cache_mngr));

  Scope_guard guard_applier_wait_enabled(
      [&thd]() { thd->disable_low_level_commit_ordering(); });

  if (is_current_stmt_binlog_enabled_and_caches_empty(thd)) {
    thd->enable_low_level_commit_ordering();
  }
  /*
    No cache manager means nothing to log, but we still have to commit
    the transaction.
   */
  if (cache_mngr == nullptr) {
    if (!skip_commit && trx_coordinator::commit_in_engines(thd, all))
      return RESULT_ABORTED;
    return RESULT_SUCCESS;
  }

  Transaction_ctx::enum_trx_scope trx_scope =
      all ? Transaction_ctx::SESSION : Transaction_ctx::STMT;

  DBUG_PRINT("debug", ("in_transaction: %s, no_2pc: %s, rw_ha_count: %d",
                       YESNO(thd->in_multi_stmt_transaction_mode()),
                       YESNO(trn_ctx->no_2pc(trx_scope)),
                       trn_ctx->rw_ha_count(trx_scope)));
  DBUG_PRINT("debug",
             ("all.cannot_safely_rollback(): %s, trx_cache_empty: %s",
              YESNO(trn_ctx->cannot_safely_rollback(Transaction_ctx::SESSION)),
              YESNO(cache_mngr->trx_cache.is_binlog_empty())));
  DBUG_PRINT("debug",
             ("stmt.cannot_safely_rollback(): %s, stmt_cache_empty: %s",
              YESNO(trn_ctx->cannot_safely_rollback(Transaction_ctx::STMT)),
              YESNO(cache_mngr->stmt_cache.is_binlog_empty())));

  /*
    If there are no handlertons registered, there is nothing to
    commit. Note that DDLs are written earlier in this case (inside
    binlog_query).

    TODO: This can be a problem in those cases that there are no
    handlertons registered. DDLs are one example, but the other case
    is MyISAM. In this case, we could register a dummy handlerton to
    trigger the commit.

    Any statement that requires logging will call binlog_query before
    trans_commit_stmt, so an alternative is to use the condition
    "binlog_query called or stmt.ha_list != 0".
   */
  if (!all && !trn_ctx->is_active(trx_scope) &&
      cache_mngr->stmt_cache.is_binlog_empty())
    return RESULT_SUCCESS;

  if (!cache_mngr->stmt_cache.is_binlog_empty()) {
    /*
      Commit parent identification of non-transactional query has
      been deferred until now, except for the mixed transaction case.
    */
    trn_ctx->store_commit_parent(
        m_dependency_tracker.get_max_committed_timestamp());
    if (cache_mngr->stmt_cache.finalize(thd)) return RESULT_ABORTED;
    stmt_stuff_logged = true;
  }

  bool one_phase = get_xa_opt(thd) == XA_ONE_PHASE;

  /*
    We commit the transaction if:
     - We are not in a transaction and committing a statement, or
     - We are in a transaction and a full transaction is committed.
    Otherwise, we accumulate the changes.
  */
  if (!cache_mngr->trx_cache.is_binlog_empty() && ending_trans(thd, all) &&
      !trx_stuff_logged) {
    const bool real_trans =
        (all || !trn_ctx->is_active(Transaction_ctx::SESSION));

    bool is_loggable_xa = is_loggable_xa_prepare(thd);

    /*
      Log and finalize transaction cache regarding XA PREPARE/XA COMMIT ONE
      PHASE if one of the following statements is true:
      - If it is a loggable XA transaction in prepare state;
      - If it is a transaction being committed with 'XA COMMIT ONE PHASE',
      statement and is not an empty transaction when GTID_NEXT is set to a
      manual GTID.

      For other XA COMMIT ONE PHASE statements that already have been finalized
      or are finalizing empty transactions when GTID_NEXT is set to a manual
      GTID, just let the execution flow get into the final 'else' branch and log
      a final 'COMMIT;' statement.
    */
    if (is_loggable_xa ||  // XA transaction in prepare state
        (thd->lex->sql_command == SQLCOM_XA_COMMIT &&  // Is a 'XA COMMIT
         one_phase &&                                  // ONE PHASE'
         xs != nullptr &&                              // and it has not yet
         !xs->is_binlogged() &&                        // been logged
         (thd->owned_gtid.sidno <= 0 ||  // and GTID_NEXT is NOT set to a
                                         // manual GTID
          !xs->has_state(XID_STATE::XA_NOTR))))  // and the transaction is NOT
                                                 // empty and NOT finalized in
                                                 // 'trans_xa_commit'
    {
      /* The prepare phase of XA transaction two phase logging. */
      int err = 0;

      assert(thd->lex->sql_command != SQLCOM_XA_COMMIT || one_phase);

      XA_prepare_log_event end_evt(thd, xs->get_xid(), one_phase);

      assert(!is_loggable_xa || skip_commit);

      err = cache_mngr->trx_cache.finalize(thd, &end_evt, xs);
      if (err) return RESULT_ABORTED;
      if (is_loggable_xa)
        if (DBUG_EVALUATE_IF("simulate_xa_prepare_failure_in_cache_finalize",
                             true, false))
          return RESULT_ABORTED;
    }
    /*
      If is atomic DDL, finalize cache for DDL and no further logging is needed.
    */
    else if ((is_atomic_ddl = cache_mngr->trx_cache.has_xid())) {
      if (cache_mngr->trx_cache.finalize(thd, nullptr)) return RESULT_ABORTED;
    }
    /*
      We are committing a 2PC transaction if it is a "real" transaction
      and has an XID assigned (because some handlerton registered). A
      transaction is "real" if either 'all' is true or
      'trn_ctx->is_active(Transaction_ctx::SESSION)' is not true.

      Note: This is kind of strange since registering the binlog
      handlerton will then make the transaction 2PC, which is not really
      true. This occurs for example if a MyISAM statement is executed
      with row-based replication on.
    */
    else if (real_trans && xid && trn_ctx->rw_ha_count(trx_scope) > 1 &&
             !trn_ctx->no_2pc(trx_scope)) {
      Xid_log_event end_evt(thd, xid);
      if (cache_mngr->trx_cache.finalize(thd, &end_evt)) return RESULT_ABORTED;
    }
    /*
      No further action needed and no special case applies, log a final
      'COMMIT' statement and finalize the transaction cache.

      Empty transactions finalized with 'XA COMMIT ONE PHASE' will be covered
      by this branch.
     */
    else {
      Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"), true, false, true,
                              0, true);
      if (cache_mngr->trx_cache.finalize(thd, &end_evt)) return RESULT_ABORTED;
    }
    trx_stuff_logged = true;
  }

  /*
    This is part of the stmt rollback.
  */
  if (!all) cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);

  /*
    Now all the events are written to the caches, so we will commit
    the transaction in the engines. This is done using the group
    commit logic in ordered_commit, which will return when the
    transaction is committed.

    If the commit in the engines fail, we still have something logged
    to the binary log so we have to report this as a "bad" failure
    (failed to commit, but logged something).
  */
  if (stmt_stuff_logged || trx_stuff_logged) {
    CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_invoke_before_commit_hook");
    if (RUN_HOOK(
            transaction, before_commit,
            (thd, all, thd_get_cache_mngr(thd)->get_trx_cache(),
             thd_get_cache_mngr(thd)->get_stmt_cache(),
             max<my_off_t>(max_binlog_cache_size, max_binlog_stmt_cache_size),
             is_atomic_ddl)) ||
        DBUG_EVALUATE_IF("simulate_failure_in_before_commit_hook", true,
                         false)) {
      if (!(thd->lex->sql_command == SQLCOM_XA_COMMIT && !one_phase)) {
        trx_coordinator::rollback_in_engines(thd, all);
      }
      gtid_state->update_on_rollback(thd);
      thd_get_cache_mngr(thd)->reset();
      // Reset the thread OK status before changing the outcome.
      if (thd->get_stmt_da()->is_ok())
        thd->get_stmt_da()->reset_diagnostics_area();
      my_error(ER_RUN_HOOK_ERROR, MYF(0), "before_commit");
      return RESULT_ABORTED;
    }
    /*
      Check whether the transaction should commit or abort given the
      plugin feedback.
    */
    if (thd->get_transaction()
            ->get_rpl_transaction_ctx()
            ->is_transaction_rollback() ||
        (DBUG_EVALUATE_IF("simulate_transaction_rollback_request", true,
                          false))) {
      if (!(thd->lex->sql_command == SQLCOM_XA_COMMIT && !one_phase)) {
        trx_coordinator::rollback_in_engines(thd, all);
      }
      gtid_state->update_on_rollback(thd);
      thd_get_cache_mngr(thd)->reset();
      if (thd->get_stmt_da()->is_ok())
        thd->get_stmt_da()->reset_diagnostics_area();
      my_error(ER_TRANSACTION_ROLLBACK_DURING_COMMIT, MYF(0));
      return RESULT_ABORTED;
    }

    if (DBUG_EVALUATE_IF("simulate_xa_commit_log_inconsistency", true, false) ||
        ordered_commit(thd, all, skip_commit)) {
      thd_get_cache_mngr(thd)->reset();
      if (thd->get_stmt_da()->is_ok())
        thd->get_stmt_da()->reset_diagnostics_area();
      return RESULT_INCONSISTENT;
    }

    DBUG_EXECUTE_IF("ensure_binlog_cache_is_reset", {
      /* Assert that binlog cache is reset at commit time. */
      assert(binlog_cache_is_reset);
      binlog_cache_is_reset = false;
    };);

    /*
      Mark the flag m_is_binlogged to true only after we are done
      with checking all the error cases.
    */
    if (is_loggable_xa_prepare(thd)) {
      thd->get_transaction()->xid_state()->set_binlogged();
      /*
        Inform hook listeners that a XA PREPARE did commit, that
        is, did log a transaction to the binary log.
      */
      (void)RUN_HOOK(transaction, after_commit, (thd, all));
    }
  } else if (!skip_commit) {
    if (trx_coordinator::commit_in_engines(thd, all))
      return RESULT_INCONSISTENT;
  }

  return RESULT_SUCCESS;
}

/**
   Flush caches for session.

   @note @c set_trans_pos is called with a pointer to the file name
   that the binary log currently use and a rotation will change the
   contents of the variable.

   The position is used when calling the after_flush, after_commit,
   and after_rollback hooks, but these have been placed so that they
   occur before a rotation is executed.

   It is the responsibility of any plugin that use this position to
   copy it if they need it after the hook has returned.

   The current "global" transaction_counter is stepped and its new value
   is assigned to the transaction.
 */
std::pair<int, my_off_t> MYSQL_BIN_LOG::flush_thread_caches(THD *thd) {
  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
  my_off_t bytes = 0;
  bool wrote_xid = false;
  int error = cache_mngr->flush(thd, &bytes, &wrote_xid);
  if (!error && bytes > 0) {
    /*
      Note that set_trans_pos does not copy the file name. See
      this function documentation for more info.
    */
    thd->set_trans_pos(log_file_name, m_binlog_file->position());
    if (wrote_xid) inc_prep_xids(thd);
  }
  DBUG_PRINT("debug", ("bytes: %llu", bytes));
  return std::make_pair(error, bytes);
}

void MYSQL_BIN_LOG::init_thd_variables(THD *thd, bool all, bool skip_commit) {
  /*
    These values are used while committing a transaction, so clear
    everything.

    Notes:

    - It would be good if we could keep transaction coordinator
      log-specific data out of the THD structure, but that is not the
      case right now.

    - Everything in the transaction structure is reset when calling
      ha_commit_low since that calls Transaction_ctx::cleanup.
  */
  thd->tx_commit_pending = true;
  thd->commit_error = THD::CE_NONE;
  thd->next_to_commit = nullptr;
  thd->durability_property = HA_IGNORE_DURABILITY;
  thd->get_transaction()->m_flags.real_commit = all;
  thd->get_transaction()->m_flags.xid_written = false;
  thd->get_transaction()->m_flags.commit_low = !skip_commit;
  thd->get_transaction()->m_flags.run_hooks = !skip_commit;
#ifndef NDEBUG
  /*
     The group commit Leader may have to wait for follower whose transaction
     is not ready to be preempted. Initially the status is pessimistic.
     Preemption guarding logics is necessary only when !NDEBUG is set.
     It won't be required for the dbug-off case as long as the follower won't
     execute any thread-specific write access code in this method, which is
     the case as of current.
  */
  thd->get_transaction()->m_flags.ready_preempt = false;
#endif
}

THD *MYSQL_BIN_LOG::fetch_and_process_flush_stage_queue(
    const bool check_and_skip_flush_logs) {
  /*
    Fetch the entire flush queue and empty it, so that the next batch
    has a leader. We must do this before invoking ha_flush_logs(...)
    for guaranteeing to flush prepared records of transactions before
    flushing them to binary log, which is required by crash recovery.
  */
  Commit_stage_manager::get_instance().lock_queue(
      Commit_stage_manager::BINLOG_FLUSH_STAGE);

  THD *first_seen =
      Commit_stage_manager::get_instance().fetch_queue_skip_acquire_lock(
          Commit_stage_manager::BINLOG_FLUSH_STAGE);
  assert(first_seen != nullptr);

  THD *commit_order_thd =
      Commit_stage_manager::get_instance().fetch_queue_skip_acquire_lock(
          Commit_stage_manager::COMMIT_ORDER_FLUSH_STAGE);

  Commit_stage_manager::get_instance().unlock_queue(
      Commit_stage_manager::BINLOG_FLUSH_STAGE);

  if (!check_and_skip_flush_logs ||
      (check_and_skip_flush_logs && commit_order_thd != nullptr)) {
    /*
      We flush prepared records of transactions to the log of storage
      engine (for example, InnoDB redo log) in a group right before
      flushing them to binary log.
    */
    ha_flush_logs(true);
  }

  /*
    The transactions are flushed to the disk and so threads
    executing slave preserve commit order can be unblocked.
  */
  Commit_stage_manager::get_instance()
      .process_final_stage_for_ordered_commit_group(commit_order_thd);
  return first_seen;
}

int MYSQL_BIN_LOG::process_flush_stage_queue(my_off_t *total_bytes_var,
                                             THD **out_queue_var) {
  DBUG_TRACE;
#ifndef NDEBUG
  // number of flushes per group.
  int no_flushes = 0;
#endif
  assert(total_bytes_var && out_queue_var);
  my_off_t total_bytes = 0;
  int flush_error = 1;
  mysql_mutex_assert_owner(&LOCK_log);

  THD *first_seen = fetch_and_process_flush_stage_queue();
  DBUG_EXECUTE_IF("crash_after_flush_engine_log", DBUG_SUICIDE(););
  CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_write_binlog");
  assign_automatic_gtids_to_flush_group(first_seen);
  /* Flush thread caches to binary log. */
  for (THD *head = first_seen; head; head = head->next_to_commit) {
    Thd_backup_and_restore switch_thd(current_thd, head);
    const auto [error, flushed_bytes] = flush_thread_caches(head);
    total_bytes += flushed_bytes;
    if (flush_error == 1) flush_error = error;
#ifndef NDEBUG
    no_flushes++;
#endif
  }

  *out_queue_var = first_seen;
  *total_bytes_var = total_bytes;

  first_seen->rpl_thd_ctx.binlog_group_commit_ctx().set_max_size_exceeded(
      total_bytes > 0 &&
      (m_binlog_file->get_real_file_size() >= (my_off_t)max_size ||
       DBUG_EVALUATE_IF("simulate_max_binlog_size", true, false)));
#ifndef NDEBUG
  DBUG_PRINT("info", ("no_flushes:= %d", no_flushes));
#endif
  return flush_error;
}

/**
  Commit a sequence of sessions.

  This function commit an entire queue of sessions starting with the
  session in @c first. If there were an error in the flushing part of
  the ordered commit, the error code is passed in and all the threads
  are marked accordingly (but not committed).

  It will also add the GTIDs of the transactions to gtid_executed.

  @see MYSQL_BIN_LOG::ordered_commit

  @param thd The "master" thread
  @param first First thread in the queue of threads to commit
 */

void MYSQL_BIN_LOG::process_commit_stage_queue(THD *thd, THD *first) {
  mysql_mutex_assert_owner(&LOCK_commit);
#ifndef NDEBUG
  thd->get_transaction()->m_flags.ready_preempt =
      true;  // formality by the leader
#endif
  for (THD *head = first; head; head = head->next_to_commit) {
    DBUG_PRINT("debug", ("Thread ID: %u, commit_error: %d, commit_pending: %s",
                         head->thread_id(), head->commit_error,
                         YESNO(head->tx_commit_pending)));
    DBUG_EXECUTE_IF(
        "block_leader_after_delete",
        if (thd != head) { DBUG_SET("+d,after_delete_wait"); };);
    /*
      If flushing failed, set commit_error for the session, skip the
      transaction and proceed with the next transaction instead. This
      will mark all threads as failed, since the flush failed.

      If flush succeeded, attach to the session and commit it in the
      engines.
    */
#ifndef NDEBUG
    Commit_stage_manager::get_instance().clear_preempt_status(head);
#endif
    if (head->get_transaction()->sequence_number != SEQ_UNINIT) {
      mysql_mutex_lock(&LOCK_replica_trans_dep_tracker);
      m_dependency_tracker.update_max_committed(head);
      mysql_mutex_unlock(&LOCK_replica_trans_dep_tracker);
    }
    /*
      Flush/Sync error should be ignored and continue
      to commit phase. And thd->commit_error cannot be
      COMMIT_ERROR at this moment.
    */
    assert(head->commit_error != THD::CE_COMMIT_ERROR);
    Thd_backup_and_restore switch_thd(thd, head);
    bool all = head->get_transaction()->m_flags.real_commit;
    assert(!head->get_transaction()->m_flags.commit_low ||
           head->get_transaction()->m_flags.ready_preempt);
    ::finish_transaction_in_engines(head, all, false);
    DBUG_PRINT("debug", ("commit_error: %d, commit_pending: %s",
                         head->commit_error, YESNO(head->tx_commit_pending)));
  }

  /*
    Handle the GTID of the threads.
    gtid_executed table is kept updated even though transactions fail to be
    logged. That's required by slave auto positioning.
  */
  gtid_state->update_commit_group(first);

  for (THD *head = first; head; head = head->next_to_commit) {
    Thd_backup_and_restore switch_thd(thd, head);
    auto all = head->get_transaction()->m_flags.real_commit;
    // Mark transaction as prepared in TC, if applicable
    trx_coordinator::set_prepared_in_tc_in_engines(head, all);
    /*
      Decrement the prepared XID counter after storage engine commit.
      We also need decrement the prepared XID when encountering a
      flush error or session attach error for avoiding 3-way deadlock
      among user thread, rotate thread and dump thread.
    */
    if (head->get_transaction()->m_flags.xid_written) dec_prep_xids(head);
  }
}

/**
  Process after commit for a sequence of sessions.

  @param thd The "master" thread
  @param first First thread in the queue of threads to commit
 */

void MYSQL_BIN_LOG::process_after_commit_stage_queue(THD *thd, THD *first) {
  for (THD *head = first; head; head = head->next_to_commit) {
    if (head->get_transaction()->m_flags.run_hooks &&
        head->commit_error != THD::CE_COMMIT_ERROR) {
      /*
        TODO: This hook here should probably move outside/below this
              if and be the only after_commit invocation left in the
              code.
      */
      Thd_backup_and_restore switch_thd(thd, head);
      bool all = head->get_transaction()->m_flags.real_commit;
      (void)RUN_HOOK(transaction, after_commit, (head, all));
      /*
        When after_commit finished for the transaction, clear the run_hooks
        flag. This allow other parts of the system to check if after_commit was
        called.
      */
      head->get_transaction()->m_flags.run_hooks = false;
    }
  }
}

#ifndef NDEBUG
/** Names for the stages. */
static const char *g_stage_name[] = {"FLUSH", "SYNC", "COMMIT", "AFTER_COMMIT",
                                     "COMMIT_ORDER_FLUSH"};
#endif

bool MYSQL_BIN_LOG::change_stage(THD *thd [[maybe_unused]],
                                 Commit_stage_manager::StageID stage,
                                 THD *queue, mysql_mutex_t *leave_mutex,
                                 mysql_mutex_t *enter_mutex) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("thd: 0x%llx, stage: %s, queue: 0x%llx", (ulonglong)thd,
                       g_stage_name[stage], (ulonglong)queue));
  assert(0 <= stage && stage < Commit_stage_manager::STAGE_COUNTER);
  assert(enter_mutex);
  assert(queue);
  /*
    enroll_for will release the leave_mutex once the sessions are
    queued.
  */
  if (!Commit_stage_manager::get_instance().enroll_for(
          stage, queue, leave_mutex, enter_mutex)) {
    assert(!thd_get_cache_mngr(thd)->dbug_any_finalized());
    return true;
  }

  return false;
}

/**
  Flush the I/O cache to file.

  Flush the binary log to the binlog file if any byte where written
  and signal that the binary log file has been updated if the flush
  succeeds.
*/

int MYSQL_BIN_LOG::flush_cache_to_file(my_off_t *end_pos_var) {
  if (m_binlog_file->flush()) {
    THD *thd = current_thd;
    thd->commit_error = THD::CE_FLUSH_ERROR;
    return ER_ERROR_ON_WRITE;
  }
  *end_pos_var = m_binlog_file->position();
  return 0;
}

/**
  Call fsync() to sync the file to disk.
*/
std::pair<bool, bool> MYSQL_BIN_LOG::sync_binlog_file(bool force) {
  bool synced = false;
  unsigned int sync_period = get_sync_period();
  if (force || (sync_period && ++sync_counter >= sync_period)) {
    sync_counter = 0;

    /*
      There is a chance that binlog file could be closed by 'RESET BINARY LOGS
      AND GTIDS' or or 'FLUSH LOGS' just after the leader releases LOCK_log and
      before it acquires LOCK_sync log. So it should check if m_binlog_file is
      opened.
    */
    if (DBUG_EVALUATE_IF("simulate_error_during_sync_binlog_file", 1,
                         m_binlog_file->is_open() && m_binlog_file->sync())) {
      THD *thd = current_thd;
      thd->commit_error = THD::CE_SYNC_ERROR;
      return std::make_pair(true, synced);
    }
    synced = true;
  }
  return std::make_pair(false, synced);
}

/**
   Helper function executed when leaving @c ordered_commit.

   This function contain the necessary code for fetching the error
   code, doing post-commit checks, and wrapping up the commit if
   necessary.

   It is typically called when enter_stage indicates that the thread
   should bail out, and also when the ultimate leader thread finishes
   executing @c ordered_commit.

   It is typically used in this manner:
   @code
   if (enter_stage(thd, Thread_queue::BINLOG_FLUSH_STAGE, thd, &LOCK_log))
     return finish_commit(thd);
   @endcode

   @return Error code if the session commit failed, or zero on
   success.
 */
int MYSQL_BIN_LOG::finish_commit(THD *thd) {
  DBUG_TRACE;
  DEBUG_SYNC(thd, "reached_finish_commit");
  /*
    In some unlikely situations, it can happen that binary
    log is closed before the thread flushes it's cache.
    In that case, clear the caches before doing commit.
  */
  if (unlikely(!is_open())) {
    binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
    if (cache_mngr) cache_mngr->reset();
  }

  if (thd->get_transaction()->sequence_number != SEQ_UNINIT) {
    mysql_mutex_lock(&LOCK_replica_trans_dep_tracker);
    m_dependency_tracker.update_max_committed(thd);
    mysql_mutex_unlock(&LOCK_replica_trans_dep_tracker);
  }

  auto all = thd->get_transaction()->m_flags.real_commit;
  auto committed_low = thd->get_transaction()->m_flags.commit_low;

  assert(thd->commit_error != THD::CE_COMMIT_ERROR);
  ::finish_transaction_in_engines(thd, all, false);

  // If the ordered commit didn't updated the GTIDs for this thd yet
  // at process_commit_stage_queue (i.e. --binlog-order-commits=0)
  // the thd still has the ownership of a GTID and we must handle it.
  if (!thd->owned_gtid_is_empty()) {
    if (thd->commit_error == THD::CE_NONE) {
      gtid_state->update_on_commit(thd);
    } else
      gtid_state->update_on_rollback(thd);
  }

  // If not yet done, mark transaction as prepared in TC, if applicable and
  // unfence the rotation of the binary log
  if (thd->get_transaction()->m_flags.xid_written) {
    trx_coordinator::set_prepared_in_tc_in_engines(thd, all);
    dec_prep_xids(thd);
  }

  // If the transaction was committed successfully, run the after_commit
  if (committed_low && (thd->commit_error != THD::CE_COMMIT_ERROR) &&
      thd->get_transaction()->m_flags.run_hooks) {
    (void)RUN_HOOK(transaction, after_commit, (thd, all));
    thd->get_transaction()->m_flags.run_hooks = false;
  }

  DBUG_EXECUTE_IF("leaving_finish_commit", {
    const char act[] = "now SIGNAL signal_leaving_finish_commit";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  };);

  assert(thd->commit_error || !thd->get_transaction()->m_flags.run_hooks);
  assert(!thd_get_cache_mngr(thd)->dbug_any_finalized());
  DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d", thd->thread_id(),
                        thd->commit_error));
  /*
    flush or sync errors are handled by the leader of the group
    (using binlog_error_action). Hence treat only COMMIT_ERRORs as errors.
  */
  return thd->commit_error == THD::CE_COMMIT_ERROR;
}

/**
   Auxiliary function used in ordered_commit.
*/
static inline int call_after_sync_hook(THD *queue_head) {
  const char *log_file = nullptr;
  my_off_t pos = 0;

  if (NO_HOOK(binlog_storage)) return 0;

  assert(queue_head != nullptr);
  for (THD *thd = queue_head; thd != nullptr; thd = thd->next_to_commit)
    if (likely(thd->commit_error == THD::CE_NONE))
      thd->get_trans_fixed_pos(&log_file, &pos);

  if (DBUG_EVALUATE_IF("simulate_after_sync_hook_error", 1, 0) ||
      RUN_HOOK(binlog_storage, after_sync, (queue_head, log_file, pos))) {
    LogErr(ERROR_LEVEL, ER_BINLOG_FAILED_TO_RUN_AFTER_SYNC_HOOK);
    return ER_ERROR_ON_WRITE;
  }
  return 0;
}

/**
  Helper function to handle flush or sync stage errors.
  If binlog_error_action= ABORT_SERVER, server will be aborted
  after reporting the error to the client.
  If binlog_error_action= IGNORE_ERROR, binlog will be closed
  for the reset of the life time of the server. close() call is protected
  with LOCK_log to avoid any parallel operations on binary log.

  @param thd Thread object that faced flush/sync error
  @param need_lock_log
                       > Indicates true if LOCk_log is needed before closing
                         binlog (happens when we are handling sync error)
                       > Indicates false if LOCK_log is already acquired
                         by the thread (happens when we are handling flush
                         error)
  @param message Message stating the reason of the failure
*/
void MYSQL_BIN_LOG::handle_binlog_flush_or_sync_error(THD *thd,
                                                      bool need_lock_log,
                                                      const char *message) {
  char errmsg[MYSQL_ERRMSG_SIZE] = {0};
  if (message == nullptr)
    sprintf(
        errmsg,
        "An error occurred during %s stage of the commit. "
        "'binlog_error_action' is set to '%s'.",
        thd->commit_error == THD::CE_FLUSH_ERROR ? "flush" : "sync",
        binlog_error_action == ABORT_SERVER ? "ABORT_SERVER" : "IGNORE_ERROR");
  else
    strncpy(errmsg, message, MYSQL_ERRMSG_SIZE - 1);
  if (binlog_error_action == ABORT_SERVER) {
    char err_buff[MYSQL_ERRMSG_SIZE + 25];
    sprintf(err_buff, "%s Server is being stopped.", errmsg);
    exec_binlog_error_action_abort(err_buff);
  } else {
    DEBUG_SYNC(thd, "before_binlog_closed_due_to_error");
    if (need_lock_log)
      mysql_mutex_lock(&LOCK_log);
    else
      mysql_mutex_assert_owner(&LOCK_log);
    /*
      It can happen that other group leader encountered
      error and already closed the binary log. So print
      error only if it is in open state. But we should
      call close() always just in case if the previous
      close did not close index file.
    */
    if (is_open()) {
      binlog::services::iterator::FileStorage::unregister_service();
      LogErr(ERROR_LEVEL, ER_TURNING_LOGGING_OFF_FOR_THE_DURATION, errmsg);
    }
    close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT, false /*need_lock_log=false*/,
          true /*need_lock_index=true*/);
    /*
      If there is a write error (flush/sync stage) and if
      binlog_error_action=IGNORE_ERROR, clear the error
      and allow the commit to happen in storage engine.
    */
    if (check_write_error(thd)) thd->clear_error();

    if (need_lock_log) mysql_mutex_unlock(&LOCK_log);
    DEBUG_SYNC(thd, "after_binlog_closed_due_to_error");
  }
}

int MYSQL_BIN_LOG::ordered_commit(THD *thd, bool all, bool skip_commit) {
  DBUG_TRACE;
  int flush_error = 0, sync_error = 0;
  my_off_t total_bytes = 0;

  CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_assign_session_to_bgc_ticket");
  thd->rpl_thd_ctx.binlog_group_commit_ctx().assign_ticket();

  DBUG_EXECUTE_IF("syncpoint_before_wait_on_ticket_3",
                  binlog::Bgc_ticket_manager::instance().push_new_ticket(););
  DBUG_EXECUTE_IF("begin_new_bgc_ticket",
                  binlog::Bgc_ticket_manager::instance().push_new_ticket(););

  DBUG_EXECUTE_IF("crash_commit_before_log", DBUG_SUICIDE(););
  init_thd_variables(thd, all, skip_commit);
  DBUG_PRINT("enter", ("commit_pending: %s, commit_error: %d, thread_id: %u",
                       YESNO(thd->tx_commit_pending), thd->commit_error,
                       thd->thread_id()));

  DEBUG_SYNC(thd, "bgc_before_flush_stage");
  DBUG_EXECUTE_IF("ordered_commit_blocked", {
    const char act[] =
        "now signal signal.ordered_commit_waiting wait_for "
        "signal.ordered_commit_continue";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  /*
    Stage #0: ensure slave threads commit order as they appear in the slave's
              relay log for transactions flushing to binary log.

    This will make thread wait until its turn to commit.
    Commit_order_manager maintains it own queue and its own order for the
    commit. So Stage#0 doesn't maintain separate StageID.
  */
  if (Commit_order_manager::wait_for_its_turn_before_flush_stage(thd) ||
      ending_trans(thd, all) ||
      Commit_order_manager::get_rollback_status(thd)) {
    if (Commit_order_manager::wait(thd)) {
      return thd->commit_error;
    }
  }

  /*
    Stage #1: flushing transactions to binary log

    While flushing, we allow new threads to enter and will process
    them in due time. Once the queue was empty, we cannot reap
    anything more since it is possible that a thread entered and
    appointed itself leader for the flush phase.
  */

  if (change_stage(thd, Commit_stage_manager::BINLOG_FLUSH_STAGE, thd, nullptr,
                   &LOCK_log)) {
    DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d", thd->thread_id(),
                          thd->commit_error));
    return finish_commit(thd);
  }

  THD *wait_queue = nullptr, *final_queue = nullptr;
  mysql_mutex_t *leave_mutex_before_commit_stage = nullptr;
  my_off_t flush_end_pos = 0;
  bool update_binlog_end_pos_after_sync;
  if (unlikely(!is_open())) {
    final_queue = fetch_and_process_flush_stage_queue(true);
    leave_mutex_before_commit_stage = &LOCK_log;
    /*
      binary log is closed, flush stage and sync stage should be
      ignored. Binlog cache should be cleared, but instead of doing
      it here, do that work in 'finish_commit' function so that
      leader and followers thread caches will be cleared.
    */
    goto commit_stage;
  }
  DEBUG_SYNC(thd, "waiting_in_the_middle_of_flush_stage");
  flush_error = process_flush_stage_queue(&total_bytes, &wait_queue);

  if (flush_error == 0 && total_bytes > 0)
    flush_error = flush_cache_to_file(&flush_end_pos);
  DBUG_EXECUTE_IF("crash_after_flush_binlog", DBUG_SUICIDE(););

  update_binlog_end_pos_after_sync = (get_sync_period() == 1);

  /*
    If the flush finished successfully, we can call the after_flush
    hook. Being invoked here, we have the guarantee that the hook is
    executed before the before/after_send_hooks on the dump thread
    preventing race conditions among these plug-ins.
  */
  if (flush_error == 0) {
    const char *file_name_ptr = log_file_name + dirname_length(log_file_name);
    assert(flush_end_pos != 0);
    if (RUN_HOOK(binlog_storage, after_flush,
                 (thd, file_name_ptr, flush_end_pos))) {
      LogErr(ERROR_LEVEL, ER_BINLOG_FAILED_TO_RUN_AFTER_FLUSH_HOOK);
      flush_error = ER_ERROR_ON_WRITE;
    }

    if (!update_binlog_end_pos_after_sync) update_binlog_end_pos();

    DBUG_EXECUTE_IF("crash_commit_after_log", DBUG_SUICIDE(););
  }

  if (flush_error) {
    /*
      Handle flush error (if any) after leader finishes it's flush stage.
    */
    handle_binlog_flush_or_sync_error(
        thd, false /* need_lock_log */,
        (thd->commit_error == THD::CE_FLUSH_GNO_EXHAUSTED_ERROR)
            ? ER_THD(thd, ER_GNO_EXHAUSTED)
            : nullptr);
  }

  DEBUG_SYNC(thd, "bgc_after_flush_stage_before_sync_stage");

  /*
    Stage #2: Syncing binary log file to disk
  */

  if (change_stage(thd, Commit_stage_manager::SYNC_STAGE, wait_queue, &LOCK_log,
                   &LOCK_sync)) {
    DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d", thd->thread_id(),
                          thd->commit_error));
    return finish_commit(thd);
  }

  /*
    Shall introduce a delay only if it is going to do sync
    in this ongoing SYNC stage. The "+1" used below in the
    if condition is to count the ongoing sync stage.
    When sync_binlog=0 (where we never do sync in BGC group),
    it is considered as a special case and delay will be executed
    for every group just like how it is done when sync_binlog= 1.
  */
  if (!flush_error && (sync_counter + 1 >= get_sync_period()))
    Commit_stage_manager::get_instance().wait_count_or_timeout(
        opt_binlog_group_commit_sync_no_delay_count,
        opt_binlog_group_commit_sync_delay, Commit_stage_manager::SYNC_STAGE);

  final_queue = Commit_stage_manager::get_instance().fetch_queue_acquire_lock(
      Commit_stage_manager::SYNC_STAGE);

  if (flush_error == 0 && total_bytes > 0) {
    DEBUG_SYNC(thd, "before_sync_binlog_file");
    std::pair<bool, bool> result = sync_binlog_file(false);
    sync_error = result.first;
  }

  if (update_binlog_end_pos_after_sync && flush_error == 0 && sync_error == 0) {
    THD *tmp_thd = final_queue;
    const char *binlog_file = nullptr;
    my_off_t pos = 0;

    while (tmp_thd != nullptr) {
      if (tmp_thd->commit_error == THD::CE_NONE) {
        tmp_thd->get_trans_fixed_pos(&binlog_file, &pos);
      }
      tmp_thd = tmp_thd->next_to_commit;
    }

    if (binlog_file != nullptr && pos > 0) {
      update_binlog_end_pos(binlog_file, pos);
    }
  }

  DEBUG_SYNC(thd, "bgc_after_sync_stage_before_commit_stage");

  leave_mutex_before_commit_stage = &LOCK_sync;
  /*
    Stage #3: Commit all transactions in order.

    This stage is skipped if we do not need to order the commits and
    each thread have to execute the handlerton commit instead.

    However, since we are keeping the lock from the previous stage, we
    need to unlock it if we skip the stage.

    We must also step commit_clock before the ha_commit_low() is called
    either in ordered fashion (by the leader of this stage) or by the thread
    themselves.

    We are delaying the handling of sync error until
    all locks are released but we should not enter into
    commit stage if binlog_error_action is ABORT_SERVER.
  */
commit_stage:
  /* Clone needs binlog commit order. */
  if ((opt_binlog_order_commits || Clone_handler::need_commit_order()) &&
      (sync_error == 0 || binlog_error_action != ABORT_SERVER)) {
    if (change_stage(thd, Commit_stage_manager::COMMIT_STAGE, final_queue,
                     leave_mutex_before_commit_stage, &LOCK_commit)) {
      DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d", thd->thread_id(),
                            thd->commit_error));
      return finish_commit(thd);
    }
    THD *commit_queue =
        Commit_stage_manager::get_instance().fetch_queue_acquire_lock(
            Commit_stage_manager::COMMIT_STAGE);
    DBUG_EXECUTE_IF("semi_sync_3-way_deadlock",
                    DEBUG_SYNC(thd, "before_process_commit_stage_queue"););

    if (flush_error == 0 && sync_error == 0)
      sync_error = call_after_sync_hook(commit_queue);

    /*
      process_commit_stage_queue will call update_on_commit or
      update_on_rollback for the GTID owned by each thd in the queue.

      This will be done this way to guarantee that GTIDs are added to
      gtid_executed in order, to avoid creating unnecessary temporary
      gaps and keep gtid_executed as a single interval at all times.

      If we allow each thread to call update_on_commit only when they
      are at finish_commit, the GTID order cannot be guaranteed and
      temporary gaps may appear in gtid_executed. When this happen,
      the server would have to add and remove intervals from the
      Gtid_set, and adding and removing intervals requires a mutex,
      which would reduce performance.
    */
    process_commit_stage_queue(thd, commit_queue);

    /**
     * After commit stage
     */
    if (change_stage(thd, Commit_stage_manager::AFTER_COMMIT_STAGE,
                     commit_queue, &LOCK_commit, &LOCK_after_commit)) {
      DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d", thd->thread_id(),
                            thd->commit_error));
      return finish_commit(thd);
    }

    THD *after_commit_queue =
        Commit_stage_manager::get_instance().fetch_queue_acquire_lock(
            Commit_stage_manager::AFTER_COMMIT_STAGE);

    process_after_commit_stage_queue(thd, after_commit_queue);

    final_queue = after_commit_queue;
    mysql_mutex_unlock(&LOCK_after_commit);
  } else {
    if (leave_mutex_before_commit_stage)
      mysql_mutex_unlock(leave_mutex_before_commit_stage);
    if (flush_error == 0 && sync_error == 0)
      sync_error = call_after_sync_hook(final_queue);
  }

  /*
    Handle sync error after we release all locks in order to avoid deadlocks
  */
  if (sync_error)
    handle_binlog_flush_or_sync_error(thd, true /* need_lock_log */, nullptr);

  /* Extract the rotate settings of all thread before signal done */
  const auto [check_rotate, force_rotate] =
      Binlog_group_commit_ctx::aggregate_rotate_settings(final_queue);

  DEBUG_SYNC(thd, "before_signal_done");
  /* Commit done so signal all waiting threads */
  Commit_stage_manager::get_instance().signal_done(final_queue);
  DBUG_EXECUTE_IF("block_leader_after_delete", {
    const char action[] = "now SIGNAL leader_proceed";
    assert(!debug_sync_set_action(thd, STRING_WITH_LEN(action)));
  };);

  /*
    Finish the commit before executing a rotate, or run the risk of a
    deadlock. We don't need the return value here since it is in
    thd->commit_error, which is returned below.
  */
  (void)finish_commit(thd);
  DEBUG_SYNC(thd, "bgc_after_commit_stage_before_rotation");

  /*
    If we need to rotate, we do it without commit error.
    Otherwise the thd->commit_error will be possibly reset.
   */
  if (DBUG_EVALUATE_IF("force_rotate", 1, 0) || force_rotate ||
      (check_rotate && thd->commit_error == THD::CE_NONE)) {
    /*
      Normally the rotate is not forced as several consecutive groups may
      request unnecessary rotations. Without force, rotate() will double check
      whether max size has been exceed and skip if not, this is an
      optimization to avoid taking LOCK_log.

      NOTE: Run purge_logs wo/ holding LOCK_log because it does not
      need the mutex. Otherwise causes various deadlocks.
    */

    DEBUG_SYNC(thd, "ready_to_do_rotation");
    bool check_purge = false;
    mysql_mutex_lock(&LOCK_log);
    /*
      If rotate fails then depends on binlog_error_action variable
      appropriate action will be taken inside rotate call.
    */
    int error = rotate(force_rotate, &check_purge);
    mysql_mutex_unlock(&LOCK_log);

    if (error)
      thd->commit_error = THD::CE_COMMIT_ERROR;
    else if (check_purge)
      auto_purge();
  }
  /*
    flush or sync errors are handled above (using binlog_error_action).
    Hence treat only COMMIT_ERRORs as errors.
  */
  return thd->commit_error == THD::CE_COMMIT_ERROR;
}

void MYSQL_BIN_LOG::report_missing_purged_gtids(
    const Gtid_set *slave_executed_gtid_set, std::string &errmsg) {
  DBUG_TRACE;
  THD *thd = current_thd;
  Gtid_set gtid_missing(gtid_state->get_lost_gtids()->get_tsid_map());
  gtid_missing.add_gtid_set(gtid_state->get_lost_gtids());
  gtid_missing.remove_gtid_set(slave_executed_gtid_set);

  String tmp_uuid;

  /* Protects thd->user_vars. */
  mysql_mutex_lock(&thd->LOCK_thd_data);
  const auto it = thd->user_vars.find("replica_uuid");
  if (it != thd->user_vars.end() && it->second->length() > 0) {
    tmp_uuid.copy(it->second->ptr(), it->second->length(), nullptr);
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  char *missing_gtids = nullptr;
  char *slave_executed_gtids = nullptr;
  gtid_missing.to_string(&missing_gtids);
  slave_executed_gtid_set->to_string(&slave_executed_gtids);

  /*
     Log the information about the missing purged GTIDs to the error log.
  */
  std::ostringstream log_info;
  log_info << "The missing transactions are '" << missing_gtids << "'";

  LogErr(WARNING_LEVEL, ER_FOUND_MISSING_GTIDS, tmp_uuid.ptr(),
         log_info.str().c_str());

  /*
     Send the information about the slave executed GTIDs and missing
     purged GTIDs to slave if the message is less than MYSQL_ERRMSG_SIZE.
  */
  std::ostringstream gtid_info;
  gtid_info << "The GTID set sent by the replica is '" << slave_executed_gtids
            << "', and the missing transactions are '" << missing_gtids << "'";
  errmsg.assign(ER_THD(thd, ER_SOURCE_HAS_PURGED_REQUIRED_GTIDS));

  /* Don't consider the "%s" in the format string. Subtract 2 from the
     total length */
  int total_length = (errmsg.length() - 2 + gtid_info.str().length());

  DBUG_EXECUTE_IF("simulate_long_missing_gtids",
                  { total_length = MYSQL_ERRMSG_SIZE + 1; });

  if (total_length > MYSQL_ERRMSG_SIZE)
    gtid_info.str(
        "The GTID sets and the missing purged transactions are too"
        " long to print in this message. For more information,"
        " please see the source's error log or the manual for"
        " GTID_SUBTRACT");

  /* Buffer for formatting the message about the missing GTIDs. */
  char buff[MYSQL_ERRMSG_SIZE] = {0};
  snprintf(buff, MYSQL_ERRMSG_SIZE, errmsg.c_str(), gtid_info.str().c_str());
  errmsg.assign(buff);

  my_free(missing_gtids);
  my_free(slave_executed_gtids);
}

void MYSQL_BIN_LOG::report_missing_gtids(
    const Gtid_set *previous_gtid_set, const Gtid_set *slave_executed_gtid_set,
    std::string &errmsg) {
  DBUG_TRACE;
  THD *thd = current_thd;
  char *missing_gtids = nullptr;
  char *slave_executed_gtids = nullptr;
  Gtid_set gtid_missing(slave_executed_gtid_set->get_tsid_map());
  gtid_missing.add_gtid_set(slave_executed_gtid_set);
  gtid_missing.remove_gtid_set(previous_gtid_set);
  gtid_missing.to_string(&missing_gtids);
  slave_executed_gtid_set->to_string(&slave_executed_gtids);

  String tmp_uuid;

  /* Protects thd->user_vars. */
  mysql_mutex_lock(&thd->LOCK_thd_data);
  const auto it = thd->user_vars.find("replica_uuid");
  if (it != thd->user_vars.end() && it->second->length() > 0) {
    tmp_uuid.copy(it->second->ptr(), it->second->length(), nullptr);
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  /*
     Log the information about the missing purged GTIDs to the error log.
  */
  std::ostringstream log_info;
  log_info << "If the binary log files have been deleted from disk,"
              " check the consistency of 'GTID_PURGED' variable."
              " The missing transactions are '"
           << missing_gtids << "'";
  LogErr(WARNING_LEVEL, ER_FOUND_MISSING_GTIDS, tmp_uuid.ptr(),
         log_info.str().c_str());
  /*
     Send the information about the slave executed GTIDs and missing
     purged GTIDs to slave if the message is less than MYSQL_ERRMSG_SIZE.
  */
  std::ostringstream gtid_info;
  gtid_info << "The GTID set sent by the replica is '" << slave_executed_gtids
            << "', and the missing transactions are '" << missing_gtids << "'";
  errmsg.assign(ER_THD(thd, ER_SOURCE_HAS_PURGED_REQUIRED_GTIDS));

  /* Don't consider the "%s" in the format string. Subtract 2 from the
     total length */
  if ((errmsg.length() - 2 + gtid_info.str().length()) > MYSQL_ERRMSG_SIZE)
    gtid_info.str(
        "The GTID sets and the missing purged transactions are too"
        " long to print in this message. For more information,"
        " please see the source's error log or the manual for"
        " GTID_SUBTRACT");
  /* Buffer for formatting the message about the missing GTIDs. */
  char buff[MYSQL_ERRMSG_SIZE] = {0};
  snprintf(buff, MYSQL_ERRMSG_SIZE, errmsg.c_str(), gtid_info.str().c_str());
  errmsg.assign(buff);

  my_free(missing_gtids);
  my_free(slave_executed_gtids);
}

void MYSQL_BIN_LOG::signal_update() {
  DBUG_TRACE;
  DBUG_EXECUTE_IF("simulate_delay_in_binlog_signal_update",
                  std::this_thread::sleep_for(std::chrono::milliseconds(120)););
  mysql_cond_broadcast(&update_cond);
  return;
}

void MYSQL_BIN_LOG::update_binlog_end_pos(bool need_lock) {
  if (need_lock)
    lock_binlog_end_pos();
  else
    mysql_mutex_assert_owner(&LOCK_binlog_end_pos);
  atomic_binlog_end_pos = m_binlog_file->position();
  signal_update();
  if (need_lock) unlock_binlog_end_pos();
}

inline void MYSQL_BIN_LOG::update_binlog_end_pos(const char *file,
                                                 my_off_t pos) {
  lock_binlog_end_pos();
  if (is_active(file) && (pos > atomic_binlog_end_pos))
    atomic_binlog_end_pos = pos;
  signal_update();
  unlock_binlog_end_pos();
}

bool THD::is_binlog_cache_empty(bool is_transactional) const {
  DBUG_TRACE;

  // If opt_bin_log==0, it is not safe to call thd_get_cache_mngr
  // because binlog_hton has not been completely set up.
  assert(opt_bin_log);

  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(this);
  if (cache_mngr == nullptr) {
    // The cache has not been setup and is thus empty
    return true;
  }

  binlog_cache_data *const cache_data =
      cache_mngr->get_binlog_cache_data(is_transactional);
  assert(cache_data != nullptr);

  return cache_data->is_binlog_empty();
}

/*
  These functions are placed in this file since they need access to
  binlog_hton, which has internal linkage.
*/

int THD::binlog_setup_trx_data() {
  DBUG_TRACE;
  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(this);

  if (cache_mngr) /* Already set up */ {
    if (Rpl_thd_context::TX_RPL_STAGE_BEGIN ==
        rpl_thd_ctx.get_tx_rpl_delegate_stage_status())
      rpl_thd_ctx.set_tx_rpl_delegate_stage_status(
          Rpl_thd_context::TX_RPL_STAGE_CACHE_CREATED);
    return 0;
  }

  cache_mngr = (binlog_cache_mngr *)my_malloc(key_memory_binlog_cache_mngr,
                                              sizeof(binlog_cache_mngr),
                                              MYF(MY_ZEROFILL));
  if (!cache_mngr) {
    return 1;  // Didn't manage to set it up
  }

  cache_mngr = new (cache_mngr)
      binlog_cache_mngr(&binlog_stmt_cache_use, &binlog_stmt_cache_disk_use,
                        &binlog_cache_use, &binlog_cache_disk_use);
  if (cache_mngr->init()) {
    cache_mngr->~binlog_cache_mngr();
    my_free(cache_mngr);
    return 1;
  }
  if (Rpl_thd_context::TX_RPL_STAGE_BEGIN ==
      rpl_thd_ctx.get_tx_rpl_delegate_stage_status())
    rpl_thd_ctx.set_tx_rpl_delegate_stage_status(
        Rpl_thd_context::TX_RPL_STAGE_CACHE_CREATED);

  DBUG_PRINT("debug", ("Set ha_data slot %d to 0x%llx", binlog_hton->slot,
                       (ulonglong)cache_mngr));
  thd_set_ha_data(this, binlog_hton, cache_mngr);

  return 0;
}

bool THD::binlog_configure_trx_cache_size(ulong new_size) {
  // Check expected block size.
  assert((new_size % IO_SIZE) == 0);

  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(this);
  if (cache_mngr == nullptr || !cache_mngr->is_binlog_empty()) {
    // Must exist and be empty
    return true;
  }

  // Close and reopen with new value
  Binlog_cache_storage *const cache = cache_mngr->get_trx_cache();
  cache->close();
  return cache->open(new_size, max_binlog_cache_size);
}

/**

*/
static void register_binlog_handler(THD *thd, bool trx) {
  DBUG_TRACE;
  /*
    If this is the first call to this function while processing a statement,
    the transactional cache does not have a savepoint defined. So, in what
    follows:
      . an implicit savepoint is defined;
      . callbacks are registered;
      . binary log is set as read/write.

    The savepoint allows for truncating the trx-cache transactional changes
    fail. Callbacks are necessary to flush caches upon committing or rolling
    back a statement or a transaction. However, notifications do not happen
    if the binary log is set as read/write.
  */
  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
  if (cache_mngr->trx_cache.get_prev_position() == MY_OFF_T_UNDEF) {
    /*
      Set an implicit savepoint in order to be able to truncate a trx-cache.
    */
    my_off_t pos = 0;
    binlog_trans_log_savepos(thd, &pos);
    cache_mngr->trx_cache.set_prev_position(pos);

    /*
      Set callbacks in order to be able to call commit or rollback.
    */
    if (trx) trans_register_ha(thd, true, binlog_hton, nullptr);
    trans_register_ha(thd, false, binlog_hton, nullptr);

    /*
      Set the binary log as read/write otherwise callbacks are not called.
    */
    thd->get_ha_data(binlog_hton->slot)->ha_info[0].set_trx_read_write();
  }
}

/**
  Function to start a statement and optionally a transaction for the
  binary log.

  This function does three things:
    - Starts a transaction if not in autocommit mode or if a BEGIN
      statement has been seen.

    - Start a statement transaction to allow us to truncate the cache.

    - Save the current binlog position so that we can roll back the
      statement by truncating the cache.

      We only update the saved position if the old one was undefined,
      the reason is that there are some cases (e.g., for CREATE-SELECT)
      where the position is saved twice (e.g., both in
      Query_result_create::prepare() and THD::binlog_write_table_map()), but
      we should use the first. This means that calls to this function
      can be used to start the statement before the first table map
      event, to include some extra events.

  Note however that IMMEDIATE_LOGGING implies that the statement is
  written without BEGIN/COMMIT.

  @param thd         Thread variable
  @param start_event The first event requested to be written into the
                     binary log
 */
static int binlog_start_trans_and_stmt(THD *thd, Log_event *start_event) {
  DBUG_TRACE;

  /*
    Initialize the cache manager if this was not done yet.
  */
  if (thd->binlog_setup_trx_data()) return 1;

  /*
    Retrieve the appropriated cache.
  */
  bool is_transactional = start_event->is_using_trans_cache();
  binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd);
  binlog_cache_data *cache_data =
      cache_mngr->get_binlog_cache_data(is_transactional);

  /*
    If the event is requesting immediate logging, there is no need to go
    further down and set savepoint and register callbacks.
  */
  if (start_event->is_using_immediate_logging()) return 0;

  register_binlog_handler(thd, thd->in_multi_stmt_transaction_mode());

  /* Transactional DDL is logged traditionally without BEGIN. */
  if (is_atomic_ddl_event(start_event)) return 0;

  /*
    If the cache is empty log "BEGIN" at the beginning of every transaction.
    Here, a transaction is either a BEGIN..COMMIT/ROLLBACK block or a single
    statement in autocommit mode.
  */
  if (cache_data->is_binlog_empty()) {
    static const char begin[] = "BEGIN";
    const char *query = nullptr;
    char buf[XID::ser_buf_size];
    char xa_start[sizeof("XA START") + 1 + sizeof(buf)];
    XID_STATE *xs = thd->get_transaction()->xid_state();
    int qlen = sizeof(begin) - 1;

    if (is_transactional && xs->has_state(XID_STATE::XA_ACTIVE)) {
      /*
        XA-prepare logging case.
      */
      qlen = sprintf(xa_start, "XA START %s", xs->get_xid()->serialize(buf));
      query = xa_start;
    } else {
      /*
        Regular transaction case.
      */
      query = begin;
    }

    Query_log_event qinfo(thd, query, qlen, is_transactional, false, true, 0,
                          true);
    if (cache_data->write_event(&qinfo)) return 1;
  }

  return 0;
}

/**
  This function writes a table map to the binary log.
  Note that in order to keep the signature uniform with related methods,
  we use a redundant parameter to indicate whether a transactional table
  was changed or not.
  Sometimes it will write a Rows_query_log_event into binary log before
  the table map too.

  @param table             a pointer to the table.
  @param is_transactional  @c true indicates a transactional table,
                           otherwise @c false a non-transactional.
  @param binlog_rows_query @c true indicates a Rows_query log event
                           will be binlogged before table map,
                           otherwise @c false indicates it will not
                           be binlogged.
  @return
    nonzero if an error pops up when writing the table map event
    or the Rows_query log event.
*/
int THD::binlog_write_table_map(TABLE *table, bool is_transactional,
                                bool binlog_rows_query) {
  int error;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("table: %p (%s: #%llu)", table, table->s->table_name.str,
                       table->s->table_map_id.id()));

  /* Pre-conditions */
  assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  assert(table->s->table_map_id.is_valid());

  Table_map_log_event the_event(this, table, table->s->table_map_id,
                                is_transactional);

  binlog_start_trans_and_stmt(this, &the_event);

  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(this);

  binlog_cache_data *cache_data =
      cache_mngr->get_binlog_cache_data(is_transactional);

  if (binlog_rows_query && this->query().str) {
    /* Write the Rows_query_log_event into binlog before the table map */
    Rows_query_log_event rows_query_ev(this, this->query().str,
                                       this->query().length);
    if ((error = cache_data->write_event(&rows_query_ev))) return error;
  }

  if ((error = cache_data->write_event(&the_event))) return error;

  binlog_table_maps++;
  return 0;
}

/**
  This function retrieves a pending row event from a cache which is
  specified through the parameter @c is_transactional. Respectively, when it
  is @c true, the pending event is returned from the transactional cache.
  Otherwise from the non-transactional cache.

  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
  @return
    The row event if any.
*/
Rows_log_event *THD::binlog_get_pending_rows_event(
    bool is_transactional) const {
  Rows_log_event *rows = nullptr;
  binlog_cache_mngr *const cache_mngr = thd_get_cache_mngr(this);

  /*
    This is less than ideal, but here's the story: If there is no cache_mngr,
    prepare_pending_rows_event() has never been called (since the cache_mngr
    is set up there). In that case, we just return NULL.
   */
  if (cache_mngr) {
    binlog_cache_data *cache_data =
        cache_mngr->get_binlog_cache_data(is_transactional);

    rows = cache_data->pending();
  }
  return (rows);
}

/**
   @param db_param    db name c-string to be inserted into alphabetically sorted
                THD::binlog_accessed_db_names list.

                Note, that space for both the data and the node
                struct are allocated in THD::main_mem_root.
                The list lasts for the top-level query time and is reset
                in @c THD::cleanup_after_query().
*/
void THD::add_to_binlog_accessed_dbs(const char *db_param) {
  char *after_db;
  /*
    binlog_accessed_db_names list is to maintain the database
    names which are referenced in a given command.
    Prior to bug 17806014 fix, 'main_mem_root' memory root used
    to store this list. The 'main_mem_root' scope is till the end
    of the query. Hence it caused increasing memory consumption
    problem in big procedures like the ones mentioned below.
    Eg: CALL p1() where p1 is having 1,00,000 create and drop tables.
    'main_mem_root' is freed only at the end of the command CALL p1()'s
    execution. But binlog_accessed_db_names list scope is only till the
    individual statements specified the procedure(create/drop statements).
    Hence the memory allocated in 'main_mem_root' was left uncleared
    until the p1's completion, even though it is not required after
    completion of individual statements.

    Instead of using 'main_mem_root' whose scope is complete query execution,
    now the memroot is changed to use 'thd->mem_root' whose scope is until the
    individual statement in CALL p1(). 'thd->mem_root' is set to
    'execute_mem_root' in the context of procedure and it's scope is till the
    individual statement in CALL p1() and thd->memroot is equal to
    'main_mem_root' in the context of a normal 'top level query'.

    Eg: a) create table t1(i int); => If this function is called while
           processing this statement, thd->memroot is equal to &main_mem_root
           which will be freed immediately after executing this statement.
        b) CALL p1() -> p1 contains create table t1(i int); => If this function
           is called while processing create table statement which is inside
           a stored procedure, then thd->memroot is equal to 'execute_mem_root'
           which will be freed immediately after executing this statement.
    In both a and b case, thd->memroot will be freed immediately and will not
    increase memory consumption.

    A special case(stored functions/triggers):
    Consider the following example:
    create function f1(i int) returns int
    begin
      insert into db1.t1 values (1);
      insert into db2.t1 values (2);
    end;
    When we are processing SELECT f1(), the list should contain db1, db2 names.
    Since thd->mem_root contains 'execute_mem_root' in the context of
    stored function, the mem root will be freed after adding db1 in
    the list and when we are processing the second statement and when we try
    to add 'db2' in the db1's list, it will lead to crash as db1's memory
    is already freed. To handle this special case, if in_sub_stmt is set
    (which is true in case of stored functions/triggers), we use &main_mem_root,
    if not set we will use thd->memroot which changes it's value to
    'execute_mem_root' or '&main_mem_root' depends on the context.
   */
  MEM_ROOT *db_mem_root = in_sub_stmt ? &main_mem_root : mem_root;

  if (!binlog_accessed_db_names)
    binlog_accessed_db_names = new (db_mem_root) List<char>;

  if (binlog_accessed_db_names->elements > MAX_DBS_IN_EVENT_MTS) {
    push_warning_printf(
        this, Sql_condition::SL_WARNING, ER_MTA_UPDATED_DBS_GREATER_MAX,
        ER_THD(this, ER_MTA_UPDATED_DBS_GREATER_MAX), MAX_DBS_IN_EVENT_MTS);
    return;
  }

  after_db = strdup_root(db_mem_root, db_param);

  /*
     sorted insertion is implemented with first rearranging data
     (pointer to char*) of the links and final appending of the least
     ordered data to create a new link in the list.
  */
  if (binlog_accessed_db_names->elements != 0) {
    List_iterator<char> it(*get_binlog_accessed_db_names());

    while (it++) {
      char *swap = nullptr;
      char **ref_cur_db = it.ref();
      int cmp = strcmp(after_db, *ref_cur_db);

      assert(!swap || cmp < 0);

      if (cmp == 0) {
        after_db = nullptr; /* dup to ignore */
        break;
      } else if (swap || cmp > 0) {
        swap = *ref_cur_db;
        *ref_cur_db = after_db;
        after_db = swap;
      }
    }
  }
  if (after_db) binlog_accessed_db_names->push_back(after_db, db_mem_root);
}

/*
  Tells if two (or more) tables have auto_increment columns and we want to
  lock those tables with a write lock.

  SYNOPSIS
    has_two_write_locked_tables_with_auto_increment
      tables        Table list

  NOTES:
    Call this function only when you have established the list of all tables
    which you'll want to update (including stored functions, triggers, views
    inside your statement).
*/

static bool has_write_table_with_auto_increment(Table_ref *tables) {
  for (Table_ref *table = tables; table; table = table->next_global) {
    /* we must do preliminary checks as table->table may be NULL */
    if (!table->is_placeholder() && table->table->found_next_number_field &&
        (table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE))
      return true;
  }

  return false;
}

/*
   checks if we have select tables in the table list and write tables
   with auto-increment column.

  SYNOPSIS
   has_two_write_locked_tables_with_auto_increment_and_query_block
      tables        Table list

  RETURN VALUES

   -true if the table list has at least one table with auto-increment column
         and at least one table to select from.
   -false otherwise
*/

static bool has_write_table_with_auto_increment_and_query_block(
    Table_ref *tables) {
  bool has_query_block = false;
  bool has_auto_increment_tables = has_write_table_with_auto_increment(tables);
  for (Table_ref *table = tables; table; table = table->next_global) {
    if (!table->is_placeholder() &&
        (table->lock_descriptor().type <= TL_READ_NO_INSERT)) {
      has_query_block = true;
      break;
    }
  }
  return (has_query_block && has_auto_increment_tables);
}

/*
  Tells if there is a table whose auto_increment column is a part
  of a compound primary key while is not the first column in
  the table definition.

  @param tables Table list

  @return true if the table exists, fais if does not.
*/

static bool has_write_table_auto_increment_not_first_in_pk(Table_ref *tables) {
  for (Table_ref *table = tables; table; table = table->next_global) {
    /* we must do preliminary checks as table->table may be NULL */
    if (!table->is_placeholder() && table->table->found_next_number_field &&
        (table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE) &&
        table->table->s->next_number_keypart != 0)
      return true;
  }

  return false;
}

/**
  Checks if a table has a column with a non-deterministic DEFAULT expression.
*/
static bool has_nondeterministic_default(const TABLE *table) {
  return std::any_of(
      table->field, table->field + table->s->fields, [](const Field *field) {
        return field->m_default_val_expr != nullptr &&
               field->m_default_val_expr->get_stmt_unsafe_flags() != 0;
      });
}

/**
  Checks if a Table_ref contains a table that has been opened for writing,
  and that has a column with a non-deterministic DEFAULT expression.
*/
static bool has_write_table_with_nondeterministic_default(
    const Table_ref *tables) {
  for (const Table_ref *table = tables; table != nullptr;
       table = table->next_global) {
    /* we must do preliminary checks as table->table may be NULL */
    if (!table->is_placeholder() &&
        table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE &&
        has_nondeterministic_default(table->table))
      return true;
  }
  return false;
}

/**
  Checks if we have reads from ACL tables in table list.

  @param  thd       Current thread
  @param  tl_list   Table_ref used by current command.

  @returns true, if we statement is unsafe, otherwise false.
*/
static bool has_acl_table_read(THD *thd, const Table_ref *tl_list) {
  for (const Table_ref *tl = tl_list; tl != nullptr; tl = tl->next_global) {
    if (is_acl_table_in_non_LTM(tl, thd->locked_tables_mode) &&
        (tl->lock_descriptor().type == TL_READ_DEFAULT ||
         tl->lock_descriptor().type == TL_READ_HIGH_PRIORITY))
      return true;
  }
  return false;
}

/*
  Function to check whether the table in query uses a fulltext parser
  plugin or not.

  @param s - table share pointer.

  @retval true - The table uses fulltext parser plugin.
  @retval false - Otherwise.
*/
static bool inline fulltext_unsafe_set(TABLE_SHARE *s) {
  for (unsigned int i = 0; i < s->keys; i++) {
    if ((s->key_info[i].flags & HA_USES_PARSER) && s->keys_in_use.is_set(i))
      return true;
  }
  return false;
}
#ifndef NDEBUG
const char *get_locked_tables_mode_name(
    enum_locked_tables_mode locked_tables_mode) {
  switch (locked_tables_mode) {
    case LTM_NONE:
      return "LTM_NONE";
    case LTM_LOCK_TABLES:
      return "LTM_LOCK_TABLES";
    case LTM_PRELOCKED:
      return "LTM_PRELOCKED";
    case LTM_PRELOCKED_UNDER_LOCK_TABLES:
      return "LTM_PRELOCKED_UNDER_LOCK_TABLES";
    default:
      return "Unknown table lock mode";
  }
}
#endif

/// Emit a deprecation warning when a transaction or statement writes to tables
/// of multiple engines, if at least one of those engines is non-transactional
/// or non-composable (see below for definition of composable).
///
/// InnoDB and Blackhole are composable storage engines, and the others are not.
///
/// We use the term *composable storage engine*. This refers to engines that are
/// not just transactional (when used alone), but even transactional when used
/// together with other composable storage engines. While all of InnoDB,
/// Blackhole, and NDB are  transactional, only InnoDB and Blackhole are
/// composable; NDB is non-composable due to the way it writes the binary log.
/// All non-transactional engines are non-composable.
void THD ::check_and_emit_warning_for_non_composable_engines(
    Table_ref *table_ref) {
  for (Table_ref *table = table_ref; table; table = table->next_global) {
    // is not a "real" table
    if (table->is_placeholder()) return;
    // We have already thrown a warning
    if (get_transaction()->tracker.get_warning_already_emitted()) return;
    // Table is not opened for writes
    if (table->lock_descriptor().type < TL_WRITE_ALLOW_WRITE) return;
    // the performance schema and temp tables are compatible with all engines
    if (table->table->file->ht->db_type == DB_TYPE_PERFORMANCE_SCHEMA ||
        table->table->file->ht->db_type == DB_TYPE_TEMPTABLE)
      continue;
    // Skip secondary engines like RAPID
    if (!table->table->s->is_primary_engine()) continue;
    handlerton *engine = table->table->file->ht;
    std::string engine_name(ha_resolve_storage_engine_name(engine));
    std::string database_name(table->db);
    std::string table_name(table->table_name);
    std::string prev_engine_name, prev_database_name, prev_table_name;
    if (get_transaction()->tracker.check_engine(
            engine_name, database_name, table_name, prev_engine_name,
            prev_database_name, prev_table_name)) {
      if (!slave_thread) {
        // send a warning to the client
        push_warning_printf(
            this, Sql_condition::SL_WARNING,
            ER_DEPRECATE_NON_COMPOSABLE_MULTIPLE_ENGINE,
            ER_THD(this, ER_DEPRECATE_NON_COMPOSABLE_MULTIPLE_ENGINE),
            engine_name.c_str(), prev_engine_name.c_str(), engine_name.c_str(),
            database_name.c_str(), table_name.c_str(), prev_engine_name.c_str(),
            prev_database_name.c_str(), prev_table_name.c_str());
      }
      // Logging the warning to the error log after a certain period of time
      // to avoid polluting the error log
      time_t now_time = time(nullptr);
      if ((now_time - last_mixed_non_transactional_engine_warning) >=
          mixed_non_transactional_engine_warning_period) {
        // log the warning to the error log
        LogErr(WARNING_LEVEL, ER_LOG_DEPRECATE_NON_COMPOSABLE_MULTIPLE_ENGINE,
               engine_name.c_str(), prev_engine_name.c_str(),
               engine_name.c_str(), database_name.c_str(), table_name.c_str(),
               prev_engine_name.c_str(), prev_database_name.c_str(),
               prev_table_name.c_str());
        last_mixed_non_transactional_engine_warning = now_time;
      }
      get_transaction()->tracker.set_warning_already_emitted(true);
    }
  }
}

/**
  Decide on logging format to use for the statement and issue errors
  or warnings as needed.  The decision depends on the following
  parameters:

  - The logging mode, i.e., the value of binlog_format.  Can be
    statement, mixed, or row.

  - The type of statement.  There are three types of statements:
    "normal" safe statements; unsafe statements; and row injections.
    An unsafe statement is one that, if logged in statement format,
    might produce different results when replayed on the slave (e.g.,
    queries with a LIMIT clause).  A row injection is either a BINLOG
    statement, or a row event executed by the slave's SQL thread.

  - The capabilities of tables modified by the statement.  The
    *capabilities vector* for a table is a set of flags associated
    with the table.  Currently, it only includes two flags: *row
    capability flag* and *statement capability flag*.

    The row capability flag is set if and only if the engine can
    handle row-based logging. The statement capability flag is set if
    and only if the table can handle statement-based logging.

  Decision table for logging format
  ---------------------------------

  The following table summarizes how the format and generated
  warning/error depends on the tables' capabilities, the statement
  type, and the current binlog_format.

     Row capable        N NNNNNNNNN YYYYYYYYY YYYYYYYYY
     Statement capable  N YYYYYYYYY NNNNNNNNN YYYYYYYYY

     Statement type     * SSSUUUIII SSSUUUIII SSSUUUIII

     binlog_format      * SMRSMRSMR SMRSMRSMR SMRSMRSMR

     Logged format      - SS-S----- -RR-RR-RR SRRSRR-RR
     Warning/Error      1 --2732444 5--5--6-- ---7--6--

  Legend
  ------

  Row capable:    N - Some table not row-capable, Y - All tables row-capable
  Stmt capable:   N - Some table not stmt-capable, Y - All tables stmt-capable
  Statement type: (S)afe, (U)nsafe, or Row (I)njection
  binlog_format:  (S)TATEMENT, (M)IXED, or (R)OW
  Logged format:  (S)tatement or (R)ow
  Warning/Error:  Warnings and error messages are as follows:

  1. Error: Cannot execute statement: binlogging impossible since both
     row-incapable engines and statement-incapable engines are
     involved.

  2. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = ROW and at least one table uses a storage engine
     limited to statement-logging.

  3. Error: Cannot execute statement: binlogging of unsafe statement
     is impossible when storage engine is limited to statement-logging
     and BINLOG_FORMAT = MIXED.

  4. Error: Cannot execute row injection: binlogging impossible since
     at least one table uses a storage engine limited to
     statement-logging.

  5. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = STATEMENT and at least one table uses a storage
     engine limited to row-logging.

  6. Error: Cannot execute row injection: binlogging impossible since
     BINLOG_FORMAT = STATEMENT.

  7. Warning: Unsafe statement binlogged in statement format since
     BINLOG_FORMAT = STATEMENT.

  In addition, we can produce the following error (not depending on
  the variables of the decision diagram):

  8. Error: Cannot execute statement: binlogging impossible since more
     than one engine is involved and at least one engine is
     self-logging.

  9. Error: Do not allow users to modify a gtid_executed table
     explicitly by a XA transaction.

  For each error case above, the statement is prevented from being
  logged, we report an error, and roll back the statement.  For
  warnings, we set the thd->binlog_flags variable: the warning will be
  printed only if the statement is successfully logged.

  @see THD::binlog_query

  @param[in] tables Tables involved in the query

  @retval 0 No error; statement can be logged.
  @retval -1 One of the error conditions above applies (1, 2, 4, 5, 6 or 9).
*/

int THD::decide_logging_format(Table_ref *tables) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("query: %s", query().str));
  DBUG_PRINT("info", ("variables.binlog_format: %lu", variables.binlog_format));
  DBUG_PRINT("info", ("lex->get_stmt_unsafe_flags(): 0x%x",
                      lex->get_stmt_unsafe_flags()));

#if defined(ENABLED_DEBUG_SYNC)
  if (!is_attachable_ro_transaction_active())
    DEBUG_SYNC(this, "begin_decide_logging_format");
#endif

  reset_binlog_local_stmt_filter();

  // emit warning for non-composable engines if need
  check_and_emit_warning_for_non_composable_engines(tables);
  /*
    We should not decide logging format if the binlog is closed or
    binlogging is off, or if the statement is filtered out from the
    binlog by filtering rules.
  */
  if (mysql_bin_log.is_open() && (variables.option_bits & OPTION_BIN_LOG) &&
      !(variables.binlog_format == BINLOG_FORMAT_STMT &&
        !binlog_filter->db_ok(m_db.str))) {
    /*
      Compute one bit field with the union of all the engine
      capabilities, and one with the intersection of all the engine
      capabilities.
    */
    handler::Table_flags flags_write_some_set = 0;
    handler::Table_flags flags_access_some_set = 0;
    handler::Table_flags flags_write_all_set =
        HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE;

    /*
       If different types of engines are about to be updated.
       For example: Innodb and Falcon; Innodb and MyIsam.
    */
    bool multi_write_engine = false;
    /*
       If different types of engines are about to be accessed
       and any of them is about to be updated. For example:
       Innodb and Falcon; Innodb and MyIsam.
    */
    bool multi_access_engine = false;
    /*
      Track if statement creates or drops a temporary table
      and log in ROW if it does.
*/
    bool is_create_drop_temp_table = false;
    /*
       Identifies if a table is changed.
    */
    bool is_write = false;
    /*
       A pointer to a previous table that was changed.
    */
    TABLE *prev_write_table = nullptr;
    /*
       A pointer to a previous table that was accessed.
    */
    TABLE *prev_access_table = nullptr;
    /*
      True if at least one table is transactional.
    */
    bool write_to_some_transactional_table = false;
    /*
      True if at least one table is non-transactional.
    */
    bool write_to_some_non_transactional_table = false;
    /*
       True if all non-transactional tables that has been updated
       are temporary.
    */
    bool write_all_non_transactional_are_tmp_tables = true;
    /**
      The number of tables used in the current statement,
      that should be replicated.
    */
    uint replicated_tables_count = 0;
    /**
      The number of tables written to in the current statement,
      that should not be replicated.
      A table should not be replicated when it is considered
      'local' to a MySQL instance.
      Currently, these tables are:
      - mysql.slow_log
      - mysql.general_log
      - mysql.slave_relay_log_info
      - mysql.slave_master_info
      - mysql.slave_worker_info
      - performance_schema.*
      - TODO: information_schema.*
      In practice, from this list, only performance_schema.* tables
      are written to by user queries.
    */
    uint non_replicated_tables_count = 0;
    /**
      Indicate whether we already reported a warning
      on modifying gtid_executed table.
    */
    int warned_gtid_executed_table = 0;
#ifndef NDEBUG
    {
      DBUG_PRINT("debug", ("prelocked_mode: %s",
                           get_locked_tables_mode_name(locked_tables_mode)));
    }
#endif

    if (variables.binlog_format != BINLOG_FORMAT_ROW && tables) {
      /*
        DML statements that modify a table with an auto_increment column based
        on rows selected from a table are unsafe as the order in which the rows
        are fetched from the select tables cannot be determined and may differ
        on master and slave.
       */
      if (has_write_table_with_auto_increment_and_query_block(tables))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_WRITE_AUTOINC_SELECT);

      if (has_write_table_auto_increment_not_first_in_pk(tables))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_NOT_FIRST);

      /*
        A query that modifies autoinc column in sub-statement can make the
        master and slave inconsistent.
        We can solve these problems in mixed mode by switching to binlogging
        if at least one updated table is used by sub-statement
       */
      if (lex->requires_prelocking() &&
          has_write_table_with_auto_increment(lex->first_not_own_table()))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_COLUMNS);

      /*
        A query that modifies a table with a non-deterministic column default
        expression in a substatement, can make the master and the slave
        inconsistent. Switch to row logging in mixed mode, and raise a warning
        in statement mode.
      */
      if (lex->requires_prelocking() &&
          has_write_table_with_nondeterministic_default(
              lex->first_not_own_table()))
        lex->set_stmt_unsafe(
            LEX::BINLOG_STMT_UNSAFE_DEFAULT_EXPRESSION_IN_SUBSTATEMENT);

      /*
        A DML or DDL statement is unsafe if it reads a ACL table while
        modifying the table, because SE skips acquiring row locks.
        Therefore rows seen by DML or DDL may not have same effect on slave.

        We skip checking the same under lock tables mode, because we do
        not skip row locks on ACL table in this mode.
      */
      if (has_acl_table_read(this, tables)) {
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_ACL_TABLE_READ_IN_DML_DDL);
      }
    }

    /*
      Get the capabilities vector for all involved storage engines and
      mask out the flags for the binary log.
    */
    for (Table_ref *table = tables; table; table = table->next_global) {
      if (table->is_placeholder()) {
        /*
          Detect if this is a CREATE TEMPORARY or DROP of a
          temporary table. This will be used later in determining whether to
          log in ROW or STMT if MIXED replication is being used.
        */
        if (!is_create_drop_temp_table && !table->table &&
            ((lex->sql_command == SQLCOM_CREATE_TABLE &&
              (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)) ||
             ((lex->sql_command == SQLCOM_DROP_TABLE ||
               lex->sql_command == SQLCOM_TRUNCATE) &&
              find_temporary_table(this, table)))) {
          is_create_drop_temp_table = true;
        }
        continue;
      }
      handler::Table_flags const flags = table->table->file->ha_table_flags();

      DBUG_PRINT("info", ("table: %s; ha_table_flags: 0x%llx",
                          table->table_name, flags));

      if (table->table->no_replicate) {
        if (!warned_gtid_executed_table) {
          warned_gtid_executed_table =
              gtid_state->warn_or_err_on_modify_gtid_table(this, table);
          /*
            Do not allow users to modify the gtid_executed table
            explicitly by a XA transaction.
          */
          if (warned_gtid_executed_table == 2) return -1;
        }
        /*
          The statement uses a table that is not replicated.
          The following properties about the table:
          - persistent / transient
          - transactional / non transactional
          - temporary / permanent
          - read or write
          - multiple engines involved because of this table
          are not relevant, as this table is completely ignored.
          Because the statement uses a non replicated table,
          using STATEMENT format in the binlog is impossible.
          Either this statement will be discarded entirely,
          or it will be logged (possibly partially) in ROW format.
        */
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_TABLE);

        if (table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE) {
          non_replicated_tables_count++;
          continue;
        }
      }

      replicated_tables_count++;

      bool trans = table->table->file->has_transactions();

      if (table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE) {
        write_to_some_transactional_table =
            write_to_some_transactional_table || trans;

        write_to_some_non_transactional_table =
            write_to_some_non_transactional_table || !trans;

        if (prev_write_table &&
            prev_write_table->file->ht != table->table->file->ht)
          multi_write_engine = true;

        if (table->table->s->tmp_table)
          lex->set_stmt_accessed_table(
              trans ? LEX::STMT_WRITES_TEMP_TRANS_TABLE
                    : LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans
                                           ? LEX::STMT_WRITES_TRANS_TABLE
                                           : LEX::STMT_WRITES_NON_TRANS_TABLE);

        /*
         Non-transactional updates are allowed when row binlog format is
         used and all non-transactional tables are temporary.
         Binlog format is checked on THD::is_dml_gtid_compatible() method.
        */
        if (!trans)
          write_all_non_transactional_are_tmp_tables =
              write_all_non_transactional_are_tmp_tables &&
              table->table->s->tmp_table;

        flags_write_all_set &= flags;
        flags_write_some_set |= flags;
        is_write = true;

        prev_write_table = table->table;

        /*
          It should be marked unsafe if a table which uses a fulltext parser
          plugin is modified. See also bug#48183.
        */
        if (!lex->is_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_FULLTEXT_PLUGIN)) {
          if (fulltext_unsafe_set(table->table->s))
            lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_FULLTEXT_PLUGIN);
        }
        /*
          INSERT...ON DUPLICATE KEY UPDATE on a table with more than one unique
          keys can be unsafe. Check for it if the flag is already not marked for
          the given statement.
        */
        if (!lex->is_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS) &&
            lex->sql_command == SQLCOM_INSERT &&
            lex->duplicates == DUP_UPDATE) {
          uint keys = table->table->s->keys, i = 0, unique_keys = 0;
          for (KEY *keyinfo = table->table->s->key_info;
               i < keys && unique_keys <= 1; i++, keyinfo++) {
            if (keyinfo->flags & HA_NOSAME) unique_keys++;
          }
          if (unique_keys > 1)
            lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS);
        }
      }
      if (lex->get_using_match()) {
        if (fulltext_unsafe_set(table->table->s))
          lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_FULLTEXT_PLUGIN);
      }

      flags_access_some_set |= flags;

      if (lex->sql_command != SQLCOM_CREATE_TABLE ||
          (lex->sql_command == SQLCOM_CREATE_TABLE &&
           ((lex->create_info->options & HA_LEX_CREATE_TMP_TABLE) ||
            (table->lock_descriptor().type < TL_WRITE_ALLOW_WRITE)))) {
        if (table->table->s->tmp_table)
          lex->set_stmt_accessed_table(
              trans ? LEX::STMT_READS_TEMP_TRANS_TABLE
                    : LEX::STMT_READS_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans ? LEX::STMT_READS_TRANS_TABLE
                                             : LEX::STMT_READS_NON_TRANS_TABLE);
      }

      if (prev_access_table &&
          prev_access_table->file->ht != table->table->file->ht)
        multi_access_engine = true;

      prev_access_table = table->table;
    }
    assert(!is_write || write_to_some_transactional_table ||
           write_to_some_non_transactional_table);
    /*
      write_all_non_transactional_are_tmp_tables may be true if any
      non-transactional table was not updated, so we fix its value here.
    */
    write_all_non_transactional_are_tmp_tables =
        write_all_non_transactional_are_tmp_tables &&
        write_to_some_non_transactional_table;

    DBUG_PRINT("info", ("flags_write_all_set: 0x%llx", flags_write_all_set));
    DBUG_PRINT("info", ("flags_write_some_set: 0x%llx", flags_write_some_set));
    DBUG_PRINT("info",
               ("flags_access_some_set: 0x%llx", flags_access_some_set));
    DBUG_PRINT("info", ("multi_write_engine: %d", multi_write_engine));
    DBUG_PRINT("info", ("multi_access_engine: %d", multi_access_engine));

    int error = 0;
    int unsafe_flags;

    /*
      With transactional data dictionary, CREATE TABLE runs as one statement
      in a multi-statement transaction internally. Revert this for the
      purposes of determining mixed statement safety.
    */
    const bool multi_stmt_trans = lex->sql_command != SQLCOM_CREATE_TABLE &&
                                  in_multi_stmt_transaction_mode();
    bool trans_table = trans_has_updated_trans_table(this);
    bool binlog_direct = variables.binlog_direct_non_trans_update;

    if (lex->is_mixed_stmt_unsafe(multi_stmt_trans, binlog_direct, trans_table,
                                  tx_isolation))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_MIXED_STATEMENT);
    else if (multi_stmt_trans && trans_table && !binlog_direct &&
             lex->stmt_accessed_table(LEX::STMT_WRITES_NON_TRANS_TABLE))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_NONTRANS_AFTER_TRANS);

    /*
      If more than one engine is involved in the statement and at
      least one is doing it's own logging (is *self-logging*), the
      statement cannot be logged atomically, so we generate an error
      rather than allowing the binlog to become corrupt.
    */
    if (multi_write_engine && (flags_write_some_set & HA_HAS_OWN_BINLOGGING))
      my_error((error = ER_BINLOG_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE),
               MYF(0));
    else if (multi_access_engine &&
             flags_access_some_set & HA_HAS_OWN_BINLOGGING)
      lex->set_stmt_unsafe(
          LEX::BINLOG_STMT_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE);

    /* XA is unsafe for statements */
    if (is_write &&
        !get_transaction()->xid_state()->has_state(XID_STATE::XA_NOTR))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_XA);

    DBUG_EXECUTE_IF("make_stmt_only_engines",
                    { flags_write_all_set = HA_BINLOG_STMT_CAPABLE; };);

    /* both statement-only and row-only engines involved */
    if ((flags_write_all_set &
         (HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE)) == 0) {
      /*
        1. Error: Binary logging impossible since both row-incapable
           engines and statement-incapable engines are involved
      */
      my_error((error = ER_BINLOG_ROW_ENGINE_AND_STMT_ENGINE), MYF(0));
    }
    /* statement-only engines involved */
    else if ((flags_write_all_set & HA_BINLOG_ROW_CAPABLE) == 0) {
      if (lex->is_stmt_row_injection()) {
        /*
          4. Error: Cannot execute row injection since table uses
             storage engine limited to statement-logging
        */
        my_error((error = ER_BINLOG_ROW_INJECTION_AND_STMT_ENGINE), MYF(0));
      } else if (variables.binlog_format == BINLOG_FORMAT_ROW &&
                 sqlcom_can_generate_row_events(this->lex->sql_command)) {
        /*
          2. Error: Cannot modify table that uses a storage engine
             limited to statement-logging when BINLOG_FORMAT = ROW
        */
        my_error((error = ER_BINLOG_ROW_MODE_AND_STMT_ENGINE), MYF(0));
      } else if (variables.binlog_format == BINLOG_FORMAT_MIXED &&
                 ((unsafe_flags = lex->get_stmt_unsafe_flags()) != 0)) {
        /*
          3. Error: Cannot execute statement: binlogging of unsafe
             statement is impossible when storage engine is limited to
             statement-logging and BINLOG_FORMAT = MIXED.
        */
        for (int unsafe_type = 0; unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
             unsafe_type++)
          if (unsafe_flags & (1 << unsafe_type))
            my_error(
                (error = ER_BINLOG_UNSAFE_AND_STMT_ENGINE), MYF(0),
                ER_THD_NONCONST(current_thd,
                                LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      } else if (is_write &&
                 ((unsafe_flags = lex->get_stmt_unsafe_flags()) != 0)) {
        /*
          7. Warning: Unsafe statement logged as statement due to
             binlog_format = STATEMENT
        */
        binlog_unsafe_warning_flags |= unsafe_flags;
        DBUG_PRINT("info", ("Scheduling warning to be issued by "
                            "binlog_query: '%s'",
                            ER_THD(current_thd, ER_BINLOG_UNSAFE_STATEMENT)));
        DBUG_PRINT("info", ("binlog_unsafe_warning_flags: 0x%x",
                            binlog_unsafe_warning_flags));
      }
      /* log in statement format! */
    }
    /* no statement-only engines */
    else {
      /* binlog_format = STATEMENT */
      if (variables.binlog_format == BINLOG_FORMAT_STMT) {
        if (lex->is_stmt_row_injection()) {
          /*
            6. Error: Cannot execute row injection since
               BINLOG_FORMAT = STATEMENT
          */
          my_error((error = ER_BINLOG_ROW_INJECTION_AND_STMT_MODE), MYF(0));
        } else if ((flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0 &&
                   sqlcom_can_generate_row_events(this->lex->sql_command)) {
          /*
            5. Error: Cannot modify table that uses a storage engine
               limited to row-logging when binlog_format = STATEMENT
          */
          my_error((error = ER_BINLOG_STMT_MODE_AND_ROW_ENGINE), MYF(0), "");
        } else if (is_write &&
                   (unsafe_flags = lex->get_stmt_unsafe_flags()) != 0) {
          /*
            7. Warning: Unsafe statement logged as statement due to
               binlog_format = STATEMENT
          */
          binlog_unsafe_warning_flags |= unsafe_flags;
          DBUG_PRINT("info", ("Scheduling warning to be issued by "
                              "binlog_query: '%s'",
                              ER_THD(current_thd, ER_BINLOG_UNSAFE_STATEMENT)));
          DBUG_PRINT("info", ("binlog_unsafe_warning_flags: 0x%x",
                              binlog_unsafe_warning_flags));
        }
        /* log in statement format! */
      }
      /* No statement-only engines and binlog_format != STATEMENT.
         I.e., nothing prevents us from row logging if needed. */
      else {
        if (lex->is_stmt_unsafe() || lex->is_stmt_row_injection() ||
            lex->is_stmt_unsafe_with_mixed_mode() ||
            (flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0 ||
            lex->stmt_accessed_table(LEX::STMT_READS_TEMP_TRANS_TABLE) ||
            lex->stmt_accessed_table(LEX::STMT_READS_TEMP_NON_TRANS_TABLE) ||
            is_create_drop_temp_table) {
#ifndef NDEBUG
          int flags = lex->get_stmt_unsafe_flags();
          DBUG_PRINT("info", ("setting row format for unsafe statement"));
          for (int i = 0; i < Query_tables_list::BINLOG_STMT_UNSAFE_COUNT;
               i++) {
            if (flags & (1 << i))
              DBUG_PRINT(
                  "info",
                  ("unsafe reason: %s",
                   ER_THD_NONCONST(
                       current_thd,
                       Query_tables_list::binlog_stmt_unsafe_errcode[i])));
          }
          DBUG_PRINT("info",
                     ("is_row_injection=%d", lex->is_stmt_row_injection()));
          DBUG_PRINT("info", ("stmt_capable=%llu",
                              (flags_write_all_set & HA_BINLOG_STMT_CAPABLE)));
          DBUG_PRINT("info", ("lex->is_stmt_unsafe_with_mixed_mode = %d",
                              lex->is_stmt_unsafe_with_mixed_mode()));
#endif
          /* log in row format! */
          set_current_stmt_binlog_format_row_if_mixed();
        }
      }
    }

    if (non_replicated_tables_count > 0) {
      if ((replicated_tables_count == 0) || !is_write) {
        DBUG_PRINT("info",
                   ("decision: no logging, no replicated table affected"));
        set_binlog_local_stmt_filter();
      } else {
        if (!is_current_stmt_binlog_format_row()) {
          my_error((error = ER_BINLOG_STMT_MODE_AND_NO_REPL_TABLES), MYF(0));
        } else {
          clear_binlog_local_stmt_filter();
        }
      }
    } else {
      clear_binlog_local_stmt_filter();
    }

    if (!error &&
        !is_dml_gtid_compatible(write_to_some_transactional_table,
                                write_to_some_non_transactional_table,
                                write_all_non_transactional_are_tmp_tables))
      error = 1;

    if (error) {
      DBUG_PRINT("info", ("decision: no logging since an error was generated"));
      return -1;
    }

    if (is_write &&
        lex->sql_command != SQLCOM_END /* rows-event applying by slave */) {
      /*
        Master side of DML in the STMT format events parallelization.
        All involving table db:s are stored in a abc-ordered name list.
        In case the number of databases exceeds MAX_DBS_IN_EVENT_MTS maximum
        the list gathering breaks since it won't be sent to the slave.
      */
      for (Table_ref *table = tables; table; table = table->next_global) {
        if (table->is_placeholder()) continue;

        assert(table->table);

        if (table->table->s->is_referenced_by_foreign_key()) {
          /*
             FK-referenced dbs can't be gathered currently. The following
             event will be marked for sequential execution on slave.
          */
          binlog_accessed_db_names = nullptr;
          add_to_binlog_accessed_dbs("");
          break;
        }
        if (!is_current_stmt_binlog_format_row())
          add_to_binlog_accessed_dbs(table->db);
      }
    }
    DBUG_PRINT("info",
               ("decision: logging in %s format",
                is_current_stmt_binlog_format_row() ? "ROW" : "STATEMENT"));

    if (variables.binlog_format == BINLOG_FORMAT_ROW &&
        (lex->sql_command == SQLCOM_UPDATE ||
         lex->sql_command == SQLCOM_UPDATE_MULTI ||
         lex->sql_command == SQLCOM_DELETE ||
         lex->sql_command == SQLCOM_DELETE_MULTI)) {
      String table_names;
      /*
        Generate a warning for UPDATE/DELETE statements that modify a
        BLACKHOLE table, as row events are not logged in row format.
      */
      for (Table_ref *table = tables; table; table = table->next_global) {
        if (table->is_placeholder()) continue;
        if (table->table->file->ht->db_type == DB_TYPE_BLACKHOLE_DB &&
            table->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE) {
          table_names.append(table->table_name);
          table_names.append(",");
        }
      }
      if (!table_names.is_empty()) {
        bool is_update = (lex->sql_command == SQLCOM_UPDATE ||
                          lex->sql_command == SQLCOM_UPDATE_MULTI);
        /*
          Replace the last ',' with '.' for table_names
        */
        table_names.replace(table_names.length() - 1, 1, ".", 1);
        push_warning_printf(
            this, Sql_condition::SL_WARNING, WARN_ON_BLOCKHOLE_IN_RBR,
            ER_THD(this, WARN_ON_BLOCKHOLE_IN_RBR),
            is_update ? "UPDATE" : "DELETE", table_names.c_ptr());
      }
    }
  } else {
    DBUG_PRINT(
        "info",
        ("decision: no logging since "
         "mysql_bin_log.is_open() = %d "
         "and (options & OPTION_BIN_LOG) = 0x%llx "
         "and binlog_format = %lu "
         "and binlog_filter->db_ok(db) = %d",
         mysql_bin_log.is_open(), (variables.option_bits & OPTION_BIN_LOG),
         variables.binlog_format, binlog_filter->db_ok(m_db.str)));

    for (Table_ref *table = tables; table; table = table->next_global) {
      if (!table->is_placeholder() && table->table->no_replicate &&
          gtid_state->warn_or_err_on_modify_gtid_table(this, table))
        break;
    }
  }

#if defined(ENABLED_DEBUG_SYNC)
  if (!is_attachable_ro_transaction_active())
    DEBUG_SYNC(this, "end_decide_logging_format");
#endif

  return 0;
}

/**
  Given that a possible violation of gtid consistency has happened,
  checks if gtid-inconsistencies are forbidden by the current value of
  ENFORCE_GTID_CONSISTENCY and GTID_MODE. If forbidden, generates
  error or warning accordingly.

  @param thd The thread that has issued the GTID-violating statement.

  @param error_code The error code to use, if error or warning is to
  be generated.

  @param log_error_code The error code to use, if error message is to
  be logged.

  @retval false Error was generated.
  @retval true No error was generated (possibly a warning was generated).
*/
static bool handle_gtid_consistency_violation(THD *thd, int error_code,
                                              int log_error_code) {
  DBUG_TRACE;

  enum_gtid_type gtid_next_type = thd->variables.gtid_next.type;
  global_tsid_lock->rdlock();
  enum_gtid_consistency_mode gtid_consistency_mode =
      get_gtid_consistency_mode();
  auto gtid_mode = global_gtid_mode.get();

  DBUG_PRINT("info", ("gtid_next.type=%d gtid_mode=%s "
                      "gtid_consistency_mode=%d error=%d query=%s",
                      gtid_next_type, Gtid_mode::to_string(gtid_mode),
                      gtid_consistency_mode, error_code, thd->query().str));

  /*
    GTID violations should generate error if:
    - GTID_MODE=ON or ON_PERMISSIVE and GTID_NEXT='AUTOMATIC' (since the
      transaction is expected to commit using a GTID), or
    - GTID_NEXT='UUID:NUMBER' (since the transaction is expected to
      commit usinga GTID), or
    - ENFORCE_GTID_CONSISTENCY=ON.
  */
  if ((thd->variables.gtid_next.is_automatic() &&
       gtid_mode >= Gtid_mode::ON_PERMISSIVE) ||
      gtid_next_type == ASSIGNED_GTID ||
      gtid_consistency_mode == GTID_CONSISTENCY_MODE_ON) {
    global_tsid_lock->unlock();
    my_error(error_code, MYF(0));
    return false;
  } else {
    /*
      If we are not generating an error, we must increase the counter
      of GTID-violating transactions.  This will prevent a concurrent
      client from executing a SET GTID_MODE or SET
      ENFORCE_GTID_CONSISTENCY statement that would be incompatible
      with this transaction.

      If the transaction had already been accounted as a gtid violating
      transaction, then don't increment the counters, just issue the
      warning below. This prevents calling
      begin_automatic_gtid_violating_transaction or
      begin_anonymous_gtid_violating_transaction multiple times for the
      same transaction, which would make the counter go out of sync.
    */
    if (!thd->has_gtid_consistency_violation) {
      if (Gtid_specification::is_automatic(gtid_next_type))
        gtid_state->begin_automatic_gtid_violating_transaction();
      else {
        assert(gtid_next_type == ANONYMOUS_GTID);
        gtid_state->begin_anonymous_gtid_violating_transaction();
      }

      /*
        If a transaction generates multiple GTID violation conditions,
        it must still only update the counters once.  Hence we use
        this per-thread flag to keep track of whether the thread has a
        consistency or not.  This function must only be called if the
        transaction does not already have a GTID violation.
      */
      thd->has_gtid_consistency_violation = true;
    }

    global_tsid_lock->unlock();

    // Generate warning if ENFORCE_GTID_CONSISTENCY = WARN.
    if (gtid_consistency_mode == GTID_CONSISTENCY_MODE_WARN) {
      // Need to print to log so that replication admin knows when users
      // have adjusted their workloads.
      LogErr(WARNING_LEVEL, log_error_code);
      // Need to print to client so that users can adjust their workload.
      push_warning(thd, Sql_condition::SL_WARNING, error_code,
                   ER_THD_NONCONST(thd, error_code));
    }
    return true;
  }
}

bool THD::is_ddl_gtid_compatible() {
  DBUG_TRACE;

  bool is_binlog_open{mysql_bin_log.is_open()};
  bool is_binlog_enabled_for_session{(variables.option_bits & OPTION_BIN_LOG) !=
                                     0};
  DBUG_PRINT("info", ("is_binlog_open:%d is_binlog_enabled_for_session:%d",
                      is_binlog_open, is_binlog_enabled_for_session));
  // If we are not going to log, then no problem, we can execute any
  // statement.
  if (!is_binlog_open || !is_binlog_enabled_for_session) return true;

  bool is_create_table{lex->sql_command == SQLCOM_CREATE_TABLE};
  bool is_create_temporary_table{false};
  bool is_create_table_select{false};
  bool is_create_table_atomic{false};
  if (is_create_table) {
    // Check this conditionally, since create_info and/or query_block
    // may be uninitialized if sql_command!=SQLCOM_CREATE_TABLE.
    is_create_temporary_table =
        (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE);
    if (!is_create_temporary_table)
      is_create_table_select = !lex->query_block->field_list_is_empty();
    is_create_table_atomic =
        get_default_handlerton(this, lex->create_info->db_type)->flags &
        HTON_SUPPORTS_ATOMIC_DDL;
  }

  bool is_drop_table{lex->sql_command == SQLCOM_DROP_TABLE};
  bool is_drop_temporary_table{false};
  if (is_drop_table) is_drop_temporary_table = lex->drop_temporary;

  bool is_in_transaction{in_multi_stmt_transaction_mode()};

  bool is_in_sub_statement{in_sub_stmt != 0};

  bool is_binlog_format_statement{variables.binlog_format ==
                                  BINLOG_FORMAT_STMT};

  DBUG_PRINT("info", ("is_create_table:%d is_create_temporary_table:%d "
                      "is_create_table_select:%d is_create_table_atomic:%d "
                      "is_drop_table:%d is_drop_temporary_table:%d "
                      "is_in_transaction:%d is_in_sub_statement:%d "
                      "is_binlog_format_statement:%d",
                      is_create_table, is_create_temporary_table,
                      is_create_table_select, is_create_table_atomic,
                      is_drop_table, is_drop_temporary_table, is_in_transaction,
                      is_in_sub_statement, is_binlog_format_statement));

  if (is_create_table_select && !is_create_temporary_table &&
      !is_create_table_atomic) {
    /*
      CREATE ... SELECT (without TEMPORARY) for engines not supporting
      atomic DDL is unsafe because if binlog_format=row it will be
      logged as a CREATE TABLE followed by row events, re-executed
      non-atomically as two transactions, and then written to the
      slave's binary log as two separate transactions with the same
      GTID.
    */
    return handle_gtid_consistency_violation(
        this, ER_GTID_UNSAFE_CREATE_SELECT,
        ER_RPL_GTID_UNSAFE_STMT_CREATE_SELECT);
  }

  if ((is_create_temporary_table || is_drop_temporary_table) &&
      is_binlog_format_statement &&
      (is_in_transaction || is_in_sub_statement)) {
    /*
      When @@session.binlog_format=statement,
      [CREATE|DROP] TEMPORARY TABLE is unsafe to execute inside a
      transaction or Procedure, because the [CREATE|DROP] statement on
      the temporary table will be executed and written into binary log
      with a GTID even if the transaction or Procedure is rolled back.
    */
    return handle_gtid_consistency_violation(
        this, ER_CLIENT_GTID_UNSAFE_CREATE_DROP_TEMP_TABLE_IN_TRX_IN_SBR,
        ER_SERVER_GTID_UNSAFE_CREATE_DROP_TEMP_TABLE_IN_TRX_IN_SBR);
  }

  return true;
}

bool THD::is_dml_gtid_compatible(bool some_transactional_table,
                                 bool some_non_transactional_table,
                                 bool non_transactional_tables_are_tmp) {
  DBUG_TRACE;

  // If @@session.sql_log_bin has been manually turned off (only
  // doable by SUPER), then no problem, we can execute any statement.
  if ((variables.option_bits & OPTION_BIN_LOG) == 0 ||
      mysql_bin_log.is_open() == false)
    return true;

  /*
    Single non-transactional updates are allowed when not mixed
    together with transactional statements within a transaction.
    Furthermore, writing to transactional and non-transactional
    engines in a single statement is also disallowed.
    Multi-statement transactions on non-transactional tables are
    split into single-statement transactions when
    GTID_NEXT = "AUTOMATIC".

    Non-transactional updates are allowed when row binlog format is
    used and all non-transactional tables are temporary.

    The debug symbol "allow_gtid_unsafe_non_transactional_updates"
    disables the error.  This is useful because it allows us to run
    old tests that were not written with the restrictions of GTIDs in
    mind.
  */
  DBUG_PRINT("info", ("some_non_transactional_table=%d "
                      "some_transactional_table=%d "
                      "trans_has_updated_trans_table=%d "
                      "non_transactional_tables_are_tmp=%d "
                      "is_current_stmt_binlog_format_row=%d",
                      some_non_transactional_table, some_transactional_table,
                      trans_has_updated_trans_table(this),
                      non_transactional_tables_are_tmp,
                      is_current_stmt_binlog_format_row()));
  if (some_non_transactional_table &&
      (some_transactional_table || trans_has_updated_trans_table(this)) &&
      !(non_transactional_tables_are_tmp &&
        is_current_stmt_binlog_format_row()) &&
      !DBUG_EVALUATE_IF("allow_gtid_unsafe_non_transactional_updates", 1, 0)) {
    return handle_gtid_consistency_violation(
        this, ER_GTID_UNSAFE_NON_TRANSACTIONAL_TABLE,
        ER_RPL_GTID_UNSAFE_STMT_ON_NON_TRANS_TABLE);
  }

  return true;
}

/*
  Implementation of interface to write rows to the binary log through the
  thread.  The thread is responsible for writing the rows it has
  inserted/updated/deleted.
*/

/*
  Template member function for ensuring that there is an rows log
  event of the appropriate type before proceeding.

  PRE CONDITION:
    - Events of type 'RowEventT' have the type code 'type_code'.

  POST CONDITION:
    If a non-NULL pointer is returned, the pending event for thread 'thd' will
    be an event of type 'RowEventT' (which have the type code 'type_code')
    will either empty or have enough space to hold 'needed' bytes.  In
    addition, the columns bitmap will be correct for the row, meaning that
    the pending event will be flushed if the columns in the event differ from
    the columns suppled to the function.

  RETURNS
    If no error, a non-NULL pending event (either one which already existed or
    the newly created one).
    If error, NULL.
 */

template <class RowsEventT>
Rows_log_event *THD::binlog_prepare_pending_rows_event(
    TABLE *table, uint32 serv_id, size_t needed, bool is_transactional,
    const unsigned char *extra_row_info, uint32 source_part_id) {
  DBUG_TRACE;

  DBUG_EXECUTE_IF("simulate_null_pending_rows_event", { return nullptr; });

  /* Fetch the type code for the RowsEventT template parameter */
  int const general_type_code = RowsEventT::TYPE_CODE;

  partition_info *part_info = table->part_info;
  auto part_id = get_rpl_part_id(part_info);

  Rows_log_event *pending = binlog_get_pending_rows_event(is_transactional);

  if (unlikely(pending && !pending->is_valid())) return nullptr;

  /*
    Check if the current event is non-NULL and a write-rows
    event. Also check if the table provided is mapped: if it is not,
    then we have switched to writing to a new table.
    If there is no pending event, we need to create one. If there is a pending
    event, but it's not about the same table id, or not of the same type
    (between Write, Update and Delete), or not the same affected columns, or
    going to be too big, flush this event to disk and create a new pending
    event.

    We do not need to check that the pending event and the new event
    have the same setting for partial json updates, because
    partialness of json can only be changed outside transactions.
  */
  if (!pending || pending->server_id != serv_id ||
      pending->get_table_id() != table->s->table_map_id ||
      pending->get_general_type_code() != general_type_code ||
      pending->get_data_size() + needed > binlog_row_event_max_size ||
      pending->read_write_bitmaps_cmp(table) == false ||
      !(pending->m_extra_row_info.compare_extra_row_info(
          extra_row_info, part_id, source_part_id))) {
    /* Create a new RowsEventT... */
    Rows_log_event *const ev = new RowsEventT(
        this, table, table->s->table_map_id, is_transactional, extra_row_info);
    if (unlikely(!ev)) return nullptr;
    ev->server_id = serv_id;  // I don't like this, it's too easy to forget.
    /*
      flush the pending event and replace it with the newly created
      event...
    */
    if (unlikely(mysql_bin_log.flush_and_set_pending_rows_event(
            this, ev, is_transactional))) {
      delete ev;
      return nullptr;
    }

    return ev; /* This is the new pending event */
  }
  return pending; /* This is the current pending event */
}

/* Declare in unnamed namespace. */
namespace {

/**
   Class to handle temporary allocation of memory for row data.

   The responsibilities of the class is to provide memory for
   packing one or two rows of packed data (depending on what
   constructor is called).

   In order to make the allocation more efficient for rows without blobs,
   a pointer to the allocated memory is stored in the table structure
   for such rows.  If memory for a table containing a blob field
   is requested, only memory for that is allocated, and subsequently
   released when the object is destroyed.

 */
class Row_data_memory {
 public:
  /**
    Build an object to keep track of a block-local piece of memory
    for storing a row of data.

    @param table
    Table where the pre-allocated memory is stored.

    @param data
    Pointer to the table record.
   */
  Row_data_memory(TABLE *table, const uchar *data) : m_memory(nullptr) {
#ifndef NDEBUG
    m_alloc_checked = false;
#endif
    allocate_memory(table, max_row_length(table, data));
    m_ptr[0] = has_memory() ? m_memory : nullptr;
    m_ptr[1] = nullptr;
  }

  Row_data_memory(TABLE *table, const uchar *data1, const uchar *data2,
                  ulonglong value_options = 0)
      : m_memory(nullptr) {
#ifndef NDEBUG
    m_alloc_checked = false;
#endif
    size_t len1 = max_row_length(table, data1);
    size_t len2 = max_row_length(table, data2, value_options);
    allocate_memory(table, len1 + len2);
    m_ptr[0] = has_memory() ? m_memory : nullptr;
    m_ptr[1] = has_memory() ? m_memory + len1 : nullptr;
  }

  ~Row_data_memory() {
    if (m_memory != nullptr && m_release_memory_on_destruction)
      my_free(m_memory);
  }

  /**
     Is there memory allocated?

     @retval true There is memory allocated
     @retval false Memory allocation failed
   */
  bool has_memory() const {
#ifndef NDEBUG
    m_alloc_checked = true;
#endif
    return m_memory != nullptr;
  }

  uchar *slot(uint s) {
    assert(s < sizeof(m_ptr) / sizeof(*m_ptr));
    assert(m_ptr[s] != nullptr);
    assert(m_alloc_checked == true);
    return m_ptr[s];
  }

 private:
  /**
    Compute an upper bound on the amount of memory needed.

    This may return an over-approximation.

    @param table The table
    @param data The server's row record.
    @param value_options The value of @@global.binlog_row_value_options
  */
  size_t max_row_length(TABLE *table, const uchar *data,
                        ulonglong value_options = 0) {
    TABLE_SHARE *table_s = table->s;
    cs::util::ReplicatedColumnsView fields{table};
    fields.add_filter(
        cs::util::ColumnFilterFactory::ColumnFilterType::outbound_func_index);
    /*
      The server stores rows using "records".  A record is a sequence of bytes
      which contains values or pointers to values for all fields (columns).  The
      server uses table_s->reclength bytes for a row record.

      The layout of a record is roughly:

      - N+1+B bits, packed into CEIL((N+1+B)/8) bytes, where N is the number of
        nullable columns in the table, and B is the sum of the number of bits of
        all BIT columns.

      - A sequence of serialized fields, each corresponding to a non-BIT,
        non-NULL column in the table.

        For variable-length columns, the first component of the serialized field
        is a length, stored using 1, 2, 3, or 4 bytes depending on the maximum
        length for the data type.

        For most data types, the next component of the serialized field is the
        actual data.  But for for VARCHAR, VARBINARY, TEXT, BLOB, and JSON, the
        next component of the serialized field is a serialized pointer,
        i.e. sizeof(pointer) bytes, which point to another memory area where the
        actual data is stored.

      The layout of a row image in the binary log is roughly:

      - If this is an after-image and partial JSON is enabled, 1 byte containing
        value_options.  If the PARTIAL_JSON bit of value_options is set, this is
        followed by P bits (the "partial_bits"), packed into CEIL(P) bytes,
        where P is the number of JSON columns in the table.

      - M bits (the "null_bits"), packed into CEIL(M) bytes, where M is the
        number of columns in the image.

      - A sequence of serialized fields, each corresponding to a non-NULL column
        in the row image.

        For variable-length columns, the first component of the serialized field
        is a length, stored using 1, 2, 3, or 4 bytes depending on the maximum
        length for the data type.

        For most data types, the next component of the serialized field is the
        actual field data.  But for JSON fields where the corresponding bit of
        the partial_bits is 1, this is a sequence of diffs instead.

      Now we try to use table_s->reclength to estimate how much memory to
      allocate for a row image in the binlog.  Due to the differences this will
      only be an upper bound.  Notice the differences:

      - The binlog may only include a subset of the fields (the row image),
        whereas reclength contains space for all fields.

      - BIT columns are not packed together with NULL bits in the binlog, so up
        to 1 more byte per BIT column may be needed.

      - The binlog has a null bit even for non-nullable fields, whereas the
        reclength only contains space nullable fields, so the binlog may need up
        to CEIL(table_s->fields/8) more bytes.

      - The binlog only has a null bit for fields in the image, whereas the
        reclength contains space for all fields.

      - The binlog contains the full blob whereas the record only contains
        sizeof(pointer) bytes.

      - The binlog contains value_options and partial_bits.  So this may use up
        to 1+CEIL(table_s->fields/8) more bytes.

      - The binlog may contain partial JSON.  This is guaranteed to be smaller
        than the size of the full value.

      - There may exist columns that, due to their nature, are not replicated,
        for instance, hidden generated columns used for functional indexes.

      For those data types that are not stored using a pointer, the size of the
      field in the binary log is at most 2 bytes more than what the field
      contributes to in table_s->reclength, because those data types use at most
      1 byte for the length and waste less than a byte on extra padding and
      extra bits in null_bits or BIT columns.

      For those data types that are stored using a pointer, the size of the
      field in the binary log is at most 2 bytes more than what the field
      contributes to in table_s->reclength, plus the size of the data.  The size
      of the pointer is at least 4 on all supported platforms, so it is bigger
      than what is used by partial_bits, value_format, or any waste due to extra
      padding and extra bits in null_bits.
    */
    size_t length = table_s->reclength + 2 * (fields.filtered_size());

    for (uint i = 0; i < table_s->blob_fields; i++) {
      if (fields.is_excluded(table_s->blob_field[i])) continue;

      Field *field = table->field[table_s->blob_field[i]];
      Field_blob *field_blob = down_cast<Field_blob *>(field);

      if (field_blob->type() == MYSQL_TYPE_JSON &&
          (value_options & PARTIAL_JSON_UPDATES) != 0) {
        Field_json *field_json = down_cast<Field_json *>(field_blob);
        length += field_json->get_diff_vector_and_length(value_options);
      } else
        length +=
            field_blob->get_length(data + field_blob->offset(table->record[0]));
    }
    return length;
  }

  void allocate_memory(TABLE *const table, const size_t total_length) {
    if (table->s->blob_fields == 0) {
      /*
        The maximum length of a packed record is less than this
        length. We use this value instead of the supplied length
        when allocating memory for records, since we don't know how
        the memory will be used in future allocations.

        Since table->s->reclength is for unpacked records, we have
        to add two bytes for each field, which can potentially be
        added to hold the length of a packed field.
      */
      size_t const maxlen = table->s->reclength + 2 * table->s->fields;

      /*
        Allocate memory for two records if memory hasn't been
        allocated. We allocate memory for two records so that it can
        be used when processing update rows as well.
      */
      if (table->write_row_record == nullptr)
        table->write_row_record = (uchar *)table->mem_root.Alloc(2 * maxlen);
      m_memory = table->write_row_record;
      m_release_memory_on_destruction = false;
    } else {
      m_memory = (uchar *)my_malloc(key_memory_Row_data_memory_memory,
                                    total_length, MYF(MY_WME));
      m_release_memory_on_destruction = true;
    }
  }

#ifndef NDEBUG
  mutable bool m_alloc_checked;
#endif
  bool m_release_memory_on_destruction;
  uchar *m_memory;
  uchar *m_ptr[2];
};

}  // namespace

int THD::binlog_write_row(TABLE *table, bool is_trans, uchar const *record,
                          const unsigned char *extra_row_info) {
  assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());

  /*
    Pack records into format for transfer. We are allocating more
    memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, record);
  if (!memory.has_memory()) return HA_ERR_OUT_OF_MEM;

  uchar *row_data = memory.slot(0);

  size_t const len = pack_row(table, table->write_set, row_data, record,
                              enum_row_image_type::WRITE_AI);

  Rows_log_event *const ev =
      binlog_prepare_pending_rows_event<Write_rows_log_event>(
          table, server_id, len, is_trans, extra_row_info);

  if (unlikely(ev == nullptr)) return HA_ERR_OUT_OF_MEM;

  return ev->add_row_data(row_data, len);
}

int THD::binlog_update_row(TABLE *table, bool is_trans,
                           const uchar *before_record,
                           const uchar *after_record,
                           const unsigned char *extra_row_info) {
  assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  int error = 0;

  /**
    Save a reference to the original read and write set bitmaps.
    We will need this to restore the bitmaps at the end.
   */
  MY_BITMAP *old_read_set = table->read_set;
  MY_BITMAP *old_write_set = table->write_set;

  /**
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(this, table);

  Row_data_memory row_data(table, before_record, after_record,
                           variables.binlog_row_value_options);
  if (!row_data.has_memory()) return HA_ERR_OUT_OF_MEM;

  uchar *before_row = row_data.slot(0);
  uchar *after_row = row_data.slot(1);

  size_t const before_size =
      pack_row(table, table->read_set, before_row, before_record,
               enum_row_image_type::UPDATE_BI);
  size_t const after_size = pack_row(
      table, table->write_set, after_row, after_record,
      enum_row_image_type::UPDATE_AI, variables.binlog_row_value_options);

  DBUG_DUMP("before_record", before_record, table->s->reclength);
  DBUG_DUMP("after_record", after_record, table->s->reclength);
  DBUG_DUMP("before_row", before_row, before_size);
  DBUG_DUMP("after_row", after_row, after_size);

  partition_info *part_info = table->part_info;
  uint32 source_part_id =
      mysql::binlog::event::Rows_event::Extra_row_info::UNDEFINED;
  if (part_info) {
    uint32 new_part_id =
        mysql::binlog::event::Rows_event::Extra_row_info::UNDEFINED;
    longlong func_value = 0;
    get_parts_for_update(before_record, after_record, table->record[0],
                         part_info, &source_part_id, &new_part_id, &func_value);
  }

  Rows_log_event *const ev =
      binlog_prepare_pending_rows_event<Update_rows_log_event>(
          table, server_id, before_size + after_size, is_trans, extra_row_info,
          source_part_id);

  if (unlikely(ev == nullptr)) return HA_ERR_OUT_OF_MEM;

  if (part_info) {
    ev->m_extra_row_info.set_source_partition_id(source_part_id);
  }

  error = ev->add_row_data(before_row, before_size) ||
          ev->add_row_data(after_row, after_size);

  /* restore read/write set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set, old_write_set);

  bitmap_clear_all(&table->tmp_set);

  return error;
}

int THD::binlog_delete_row(TABLE *table, bool is_trans, uchar const *record,
                           const unsigned char *extra_row_info) {
  assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  int error = 0;

  /**
    Save a reference to the original read and write set bitmaps.
    We will need this to restore the bitmaps at the end.
   */
  MY_BITMAP *old_read_set = table->read_set;
  MY_BITMAP *old_write_set = table->write_set;

  /**
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(this, table);

  /*
     Pack records into format for transfer. We are allocating more
     memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, record);
  if (unlikely(!memory.has_memory())) return HA_ERR_OUT_OF_MEM;

  uchar *row_data = memory.slot(0);

  DBUG_DUMP("table->read_set", (uchar *)table->read_set->bitmap,
            (table->s->fields + 7) / 8);
  size_t const len = pack_row(table, table->read_set, row_data, record,
                              enum_row_image_type::DELETE_BI);

  Rows_log_event *const ev =
      binlog_prepare_pending_rows_event<Delete_rows_log_event>(
          table, server_id, len, is_trans, extra_row_info);

  if (unlikely(ev == nullptr)) return HA_ERR_OUT_OF_MEM;

  error = ev->add_row_data(row_data, len);

  /* restore read/write set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set, old_write_set);

  bitmap_clear_all(&table->tmp_set);
  return error;
}

void binlog_prepare_row_images(const THD *thd, TABLE *table) {
  DBUG_TRACE;
  /**
    Remove from read_set spurious columns. The write_set has been
    handled before in table->mark_columns_needed_for_update.
   */

  DBUG_PRINT_BITSET("debug", "table->read_set (before preparing): %s",
                    table->read_set);

  /**
    if there is a primary key in the table (ie, user declared PK or a
    non-null unique index) and we dont want to ship the entire image,
    and the handler involved supports this.
   */
  if (table->s->primary_key < MAX_KEY &&
      (thd->variables.binlog_row_image < BINLOG_ROW_IMAGE_FULL) &&
      !ha_check_storage_engine_flag(table->s->db_type(),
                                    HTON_NO_BINLOG_ROW_OPT)) {
    /**
      Just to be sure that tmp_set is currently not in use as
      the read_set already.
    */
    assert(table->read_set != &table->tmp_set);
    // Verify it's not used
    assert(bitmap_is_clear_all(&table->tmp_set));

    switch (thd->variables.binlog_row_image) {
      case BINLOG_ROW_IMAGE_MINIMAL:
        /* MINIMAL: Mark only PK */
        table->mark_columns_used_by_index_no_reset(table->s->primary_key,
                                                   &table->tmp_set);
        break;
      case BINLOG_ROW_IMAGE_NOBLOB:
        /**
          NOBLOB: Remove unnecessary BLOB fields from read_set
                  (the ones that are not part of PK).
         */
        bitmap_union(&table->tmp_set, table->read_set);
        for (Field **ptr = table->field; *ptr; ptr++) {
          Field *field = (*ptr);
          if ((field->type() == MYSQL_TYPE_BLOB) &&
              !field->is_flag_set(PRI_KEY_FLAG))
            bitmap_clear_bit(&table->tmp_set, field->field_index());
        }
        break;
      default:
        assert(0);  // impossible.
    }

    /* set the temporary read_set */
    table->column_bitmaps_set_no_signal(&table->tmp_set, table->write_set);
  }

  DBUG_PRINT_BITSET("debug", "table->read_set (after preparing): %s",
                    table->read_set);
}

int THD::binlog_flush_pending_rows_event(bool stmt_end, bool is_transactional) {
  DBUG_TRACE;
  /*
    We shall flush the pending event even if we are not in row-based
    mode: it might be the case that we left row-based mode before
    flushing anything (e.g., if we have explicitly locked tables).
   */
  if (!mysql_bin_log.is_open()) return 0;

  /*
    Mark the event as the last event of a statement if the stmt_end
    flag is set.
  */
  int error = 0;
  if (Rows_log_event *pending =
          binlog_get_pending_rows_event(is_transactional)) {
    if (stmt_end) {
      pending->set_flags(Rows_log_event::STMT_END_F);
      binlog_table_maps = 0;
    }

    error = mysql_bin_log.flush_and_set_pending_rows_event(this, nullptr,
                                                           is_transactional);
  }

  return error;
}

#if !defined(NDEBUG)
static const char *show_query_type(THD::enum_binlog_query_type qtype) {
  switch (qtype) {
    case THD::ROW_QUERY_TYPE:
      return "ROW";
    case THD::STMT_QUERY_TYPE:
      return "STMT";
    case THD::QUERY_TYPE_COUNT:
    default:
      assert(0 <= qtype && qtype < THD::QUERY_TYPE_COUNT);
  }
  static char buf[64];
  sprintf(buf, "UNKNOWN#%d", qtype);
  return buf;
}
#endif

/**
  Auxiliary function to reset the limit unsafety warning suppression.
*/
static void reset_binlog_unsafe_suppression() {
  DBUG_TRACE;
  unsafe_warning_suppression_is_activated = false;
  limit_unsafe_warning_count = 0;
  limit_unsafe_suppression_start_time = my_getsystime() / 10000000;
}

/**
  Auxiliary function to print warning in the error log.
*/
static void print_unsafe_warning_to_log(int unsafe_type, char *buf,
                                        const char *query) {
  DBUG_TRACE;
  sprintf(buf, ER_DEFAULT(ER_BINLOG_UNSAFE_STATEMENT),
          ER_DEFAULT_NONCONST(LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
  LogErr(WARNING_LEVEL, ER_BINLOG_UNSAFE_MESSAGE_AND_STATEMENT, buf, query);
}

/**
  Auxiliary function to check if the warning for limit unsafety should be
  thrown or suppressed. Details of the implementation can be found in the
  comments inline.

  @param buf         Buffer to hold the warning message text
  @param unsafe_type The type of unsafety.
  @param query       The actual query statement.

  TODO: Remove this function and implement a general service for all warnings
  that would prevent flooding the error log. => switch to log_throttle class?
*/
static void do_unsafe_limit_checkout(char *buf, int unsafe_type,
                                     const char *query) {
  ulonglong now;
  DBUG_TRACE;
  assert(unsafe_type == LEX::BINLOG_STMT_UNSAFE_LIMIT);
  limit_unsafe_warning_count++;
  /*
    INITIALIZING:
    If this is the first time this function is called with log warning
    enabled, the monitoring the unsafe warnings should start.
  */
  if (limit_unsafe_suppression_start_time == 0) {
    limit_unsafe_suppression_start_time = my_getsystime() / 10000000;
    print_unsafe_warning_to_log(unsafe_type, buf, query);
  } else {
    if (!unsafe_warning_suppression_is_activated)
      print_unsafe_warning_to_log(unsafe_type, buf, query);

    if (limit_unsafe_warning_count >=
        LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT) {
      now = my_getsystime() / 10000000;
      if (!unsafe_warning_suppression_is_activated) {
        /*
          ACTIVATION:
          We got LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT warnings in
          less than LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT we activate the
          suppression.
        */
        if ((now - limit_unsafe_suppression_start_time) <=
            LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT) {
          unsafe_warning_suppression_is_activated = true;
          DBUG_PRINT("info", ("A warning flood has been detected and the "
                              "limit unsafety warning suppression has been "
                              "activated."));
        } else {
          /*
           there is no flooding till now, therefore we restart the monitoring
          */
          limit_unsafe_suppression_start_time = my_getsystime() / 10000000;
          limit_unsafe_warning_count = 0;
        }
      } else {
        /*
          Print the suppression note and the unsafe warning.
        */
        LogErr(INFORMATION_LEVEL, ER_BINLOG_WARNING_SUPPRESSED,
               limit_unsafe_warning_count,
               (int)(now - limit_unsafe_suppression_start_time));
        print_unsafe_warning_to_log(unsafe_type, buf, query);
        /*
          DEACTIVATION: We got LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT
          warnings in more than  LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT, the
          suppression should be deactivated.
        */
        if ((now - limit_unsafe_suppression_start_time) >
            LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT) {
          reset_binlog_unsafe_suppression();
          DBUG_PRINT("info", ("The limit unsafety warning supression has "
                              "been deactivated"));
        }
      }
      limit_unsafe_warning_count = 0;
    }
  }
}

/**
  Auxiliary method used by @c binlog_query() to raise warnings.

  The type of warning and the type of unsafeness is stored in
  THD::binlog_unsafe_warning_flags.
*/
void THD::issue_unsafe_warnings() {
  char buf[MYSQL_ERRMSG_SIZE * 2];
  DBUG_TRACE;
  /*
    Ensure that binlog_unsafe_warning_flags is big enough to hold all
    bits.  This is actually a constant expression.
  */
  assert(LEX::BINLOG_STMT_UNSAFE_COUNT <=
         sizeof(binlog_unsafe_warning_flags) * CHAR_BIT);

  uint32 unsafe_type_flags = binlog_unsafe_warning_flags;

  /*
    For each unsafe_type, check if the statement is unsafe in this way
    and issue a warning.
  */
  for (int unsafe_type = 0; unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
       unsafe_type++) {
    if ((unsafe_type_flags & (1 << unsafe_type)) != 0) {
      push_warning_printf(
          this, Sql_condition::SL_NOTE, ER_BINLOG_UNSAFE_STATEMENT,
          ER_THD(this, ER_BINLOG_UNSAFE_STATEMENT),
          ER_THD_NONCONST(this, LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      if (log_error_verbosity > 1 && opt_log_unsafe_statements) {
        if (unsafe_type == LEX::BINLOG_STMT_UNSAFE_LIMIT)
          do_unsafe_limit_checkout(buf, unsafe_type, query().str);
        else  // cases other than LIMIT unsafety
          print_unsafe_warning_to_log(unsafe_type, buf, query().str);
      }
    }
  }
}

/**
  Log the current query.

  The query will be logged in either row format or statement format
  depending on the value of @c current_stmt_binlog_format_row field and
  the value of the @c qtype parameter.

  This function must be called:

  - After the all calls to ha_*_row() functions have been issued.

  - After any writes to system tables. Rationale: if system tables
    were written after a call to this function, and the master crashes
    after the call to this function and before writing the system
    tables, then the master and slave get out of sync.

  - Before tables are unlocked and closed.

  @see decide_logging_format

  @retval 0 Success

  @retval nonzero If there is a failure when writing the query (e.g.,
  write failure), then the error code is returned.
*/
int THD::binlog_query(THD::enum_binlog_query_type qtype, const char *query_arg,
                      size_t query_len, bool is_trans, bool direct,
                      bool suppress_use, int errcode) {
  DBUG_TRACE;
  DBUG_PRINT("enter",
             ("qtype: %s  query: '%s'", show_query_type(qtype), query_arg));
  assert(query_arg && mysql_bin_log.is_open());

  if (get_binlog_local_stmt_filter() == BINLOG_FILTER_SET) {
    /*
      The current statement is to be ignored, and not written to
      the binlog. Do not call issue_unsafe_warnings().
    */
    return 0;
  }

  /*
    If we are not in prelocked mode, mysql_unlock_tables() will be
    called after this binlog_query(), so we have to flush the pending
    rows event with the STMT_END_F set to unlock all tables at the
    slave side as well.

    If we are in prelocked mode, the flushing will be done inside the
    top-most close_thread_tables().
  */
  if (this->locked_tables_mode <= LTM_LOCK_TABLES)
    if (int error = binlog_flush_pending_rows_event(true, is_trans))
      return error;

  /*
    Warnings for unsafe statements logged in statement format are
    printed in three places instead of in decide_logging_format().
    This is because the warnings should be printed only if the statement
    is actually logged. When executing decide_logging_format(), we cannot
    know for sure if the statement will be logged:

    1 - sp_head::execute_procedure which prints out warnings for calls to
    stored procedures.

    2 - sp_head::execute_function which prints out warnings for calls
    involving functions.

    3 - THD::binlog_query (here) which prints warning for top level
    statements not covered by the two cases above: i.e., if not inside a
    procedure and a function.

    Besides, we should not try to print these warnings if it is not
    possible to write statements to the binary log as it happens when
    the execution is inside a function, or generally speaking, when
    the variables.option_bits & OPTION_BIN_LOG is false.
  */
  if ((variables.option_bits & OPTION_BIN_LOG) && sp_runtime_ctx == nullptr &&
      !binlog_evt_union.do_union)
    issue_unsafe_warnings();

  switch (qtype) {
      /*
        ROW_QUERY_TYPE means that the statement may be logged either in
        row format or in statement format.  If
        current_stmt_binlog_format is row, it means that the
        statement has already been logged in row format and hence shall
        not be logged again.
      */
    case THD::ROW_QUERY_TYPE:
      DBUG_PRINT("debug", ("is_current_stmt_binlog_format_row: %d",
                           is_current_stmt_binlog_format_row()));
      if (is_current_stmt_binlog_format_row()) return 0;
      [[fallthrough]];

      /*
        STMT_QUERY_TYPE means that the query must be logged in statement
        format; it cannot be logged in row format.  This is typically
        used by DDL statements.  It is an error to use this query type
        if current_stmt_binlog_format_row is row.

        @todo Currently there are places that call this method with
        STMT_QUERY_TYPE and current_stmt_binlog_format is row.  Fix those
        places and add assert to ensure correct behavior. /Sven
      */
    case THD::STMT_QUERY_TYPE:
      /*
        The MYSQL_BIN_LOG::write() function will set the STMT_END_F flag and
        flush the pending rows event if necessary.
      */
      {
        Query_log_event qinfo(this, query_arg, query_len, is_trans, direct,
                              suppress_use, errcode);
        /*
          Binlog table maps will be irrelevant after a Query_log_event
          (they are just removed on the slave side) so after the query
          log event is written to the binary log, we pretend that no
          table maps were written.
         */
        int error = mysql_bin_log.write_event(&qinfo);
        binlog_table_maps = 0;
        return error;
      }
      break;

    case THD::QUERY_TYPE_COUNT:
    default:
      assert(0 <= qtype && qtype < QUERY_TYPE_COUNT);
  }
  return 0;
}

namespace {
void finish_transaction_in_engines(THD *thd, bool all, bool run_after_commit) {
  if (thd->get_transaction()->m_flags.commit_low) {
    if (trx_coordinator::commit_in_engines(thd, all, run_after_commit))
      thd->commit_error = THD::CE_COMMIT_ERROR;
  } else if (is_xa_rollback(thd)) {
    if (trx_coordinator::rollback_in_engines(thd, all))
      thd->commit_error = THD::CE_COMMIT_ERROR;
  }
}
}  // namespace

struct st_mysql_storage_engine binlog_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

/** @} */

mysql_declare_plugin(binlog){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &binlog_storage_engine,
    "binlog",
    PLUGIN_AUTHOR_ORACLE,
    "This is a pseudo storage engine to represent the binlog in a transaction",
    PLUGIN_LICENSE_GPL,
    binlog_init,   /* Plugin Init */
    nullptr,       /* Plugin Check uninstall */
    binlog_deinit, /* Plugin Deinit */
    0x0100 /* 1.0 */,
    nullptr, /* status variables                */
    nullptr, /* system variables                */
    nullptr, /* config options                  */
    0,
} mysql_declare_plugin_end;
