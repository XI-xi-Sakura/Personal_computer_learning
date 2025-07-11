/* Copyright (c) 2013, 2025, Oracle and/or its affiliates.

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

#include <assert.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/service_rpl_transaction_ctx.h>
#include <mysql/service_rpl_transaction_write_set.h>
#include <stddef.h>
#include <string>
#include <vector>

#include "base64.h"
#include "my_byteorder.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "plugin/group_replication/include/consistency_manager.h"
#include "plugin/group_replication/include/observer_trans.h"
#include "plugin/group_replication/include/plugin.h"
#include "plugin/group_replication/include/plugin_messages/transaction_message.h"
#include "plugin/group_replication/include/plugin_messages/transaction_with_guarantee_message.h"
#include "plugin/group_replication/include/plugin_observers/group_transaction_observation_manager.h"

#ifndef NDEBUG
#include "plugin/group_replication/include/sql_service/sql_command_test.h"
#endif
#include "string_with_len.h"

/*
  Buffer to read the write_set value as a string.
  Since we support up to 64 bits hashes, 8 bytes are enough to store the info.
*/
#define BUFFER_READ_PKE 8

int add_write_set(Transaction_context_log_event *tcle, std::vector<uint64> *set,
                  const THD *thd) {
  DBUG_TRACE;
  for (std::vector<uint64>::iterator it = set->begin(); it != set->end();
       ++it) {
    uchar buff[BUFFER_READ_PKE];
    int8store(buff, *it);
    uint64 const tmp_str_sz =
        base64_needed_encoded_length((uint64)BUFFER_READ_PKE);
    char *write_set_value =
        (char *)my_malloc(key_write_set_encoded, tmp_str_sz, MYF(MY_WME));
    // key_write_set_encoded is being tracked for connection memory limits, if
    // exceed thread will be killed
    if (thd->is_killed()) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CONN_KILLED,
                   "Generate write identification hash failed");
      my_free(write_set_value);
      return 1;
    }

    if (!write_set_value) {
      /* purecov: begin inspected */
      LogPluginErr(ERROR_LEVEL,
                   ER_GRP_RPL_OOM_FAILED_TO_GENERATE_IDENTIFICATION_HASH);
      my_free(write_set_value);
      return 1;
      /* purecov: end */
    }

    if (base64_encode(buff, (size_t)BUFFER_READ_PKE, write_set_value)) {
      /* purecov: begin inspected */
      LogPluginErr(ERROR_LEVEL,
                   ER_GRP_RPL_WRITE_IDENT_HASH_BASE64_ENCODING_FAILED);
      my_free(write_set_value);
      return 1;
      /* purecov: end */
    }

    tcle->add_write_set(write_set_value);
  }
  return 0;
}

/*
  Transaction lifecycle events observers.
*/

int group_replication_trans_before_dml(Trans_param *param, int &out) {
  DBUG_TRACE;

  out = 0;

  // If group replication has not started, then moving along...
  if (!plugin_is_group_replication_running()) {
    return 0;
  }

  /*
   The first check to be made is if the session binlog is active
   If it is not active, this query is not relevant for the plugin.
   */
  if (!param->trans_ctx_info.binlog_enabled) {
    return 0;
  }

  /*
   In runtime, check the global variables that can change.
   */
  if ((out += (param->trans_ctx_info.binlog_format != BINLOG_FORMAT_ROW))) {
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_INVALID_BINLOG_FORMAT);
    return 0;
  }

  if (local_member_info->has_enforces_update_everywhere_checks() &&
      (out += (param->trans_ctx_info.tx_isolation == ISO_SERIALIZABLE))) {
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_UNSUPPORTED_TRANS_ISOLATION);
    return 0;
  }
  /*
    Cycle through all involved tables to assess if they all
    comply with the plugin runtime requirements. For now:
    - The table must be from a transactional engine
    - It must contain at least one primary key
    - It should not contain 'ON DELETE/UPDATE CASCADE' referential action
   */
  for (uint table = 0; out == 0 && table < param->number_of_tables; table++) {
    if (param->tables_info[table].db_type != DB_TYPE_INNODB) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_NEEDS_INNODB_TABLE,
                   param->tables_info[table].table_name);
      out++;
    }

    if (param->tables_info[table].number_of_primary_keys == 0) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_PRIMARY_KEY_NOT_DEFINED,
                   param->tables_info[table].table_name);
      out++;
    }
    if (local_member_info->has_enforces_update_everywhere_checks() &&
        param->tables_info[table].has_cascade_foreign_key) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_FK_WITH_CASCADE_UNSUPPORTED,
                   param->tables_info[table].table_name);
      out++;
    }
  }

  return 0;
}

int group_replication_trans_before_commit(Trans_param *param) {
  DBUG_TRACE;
  int error = 0;
  const int pre_wait_error = 1;
  const int post_wait_error = 2;
  int64 last_committed = 0;
  int64 sequence_number = 1;

  DBUG_EXECUTE_IF("group_replication_force_error_on_before_commit_listener",
                  return 1;);

  DBUG_EXECUTE_IF("group_replication_before_commit_hook_wait", {
    const char act[] =
        "now signal signal.group_replication_before_commit_hook_wait_reached "
        "wait_for continue_commit";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  DBUG_EXECUTE_IF("group_replication_pause_on_before_commit_hook", {
    // DBUG_SYNC are hold by same MDL lock test is using
    const uint sleep_time_seconds = VIEW_MODIFICATION_TIMEOUT * 1.5;
    my_sleep(sleep_time_seconds * 1000000);
  });

  /*
    If the originating id belongs to a thread in the plugin, the transaction
    was already certified. Channel operations can deadlock against
    plugin/applier thread stops so they must remain outside the plugin stop
    lock below.
  */
  Replication_thread_api channel_interface;
  if (GR_APPLIER_CHANNEL == param->rpl_channel_type) {
    // If plugin is not initialized, there is nothing to do.
    if (nullptr == local_member_info) {
      return 0;
    }

    // If plugin is stopping, there is no point in update the statistics.
    bool fail_to_lock = shared_plugin_stop_lock->try_grab_read_lock();
    if (!fail_to_lock) {
      const Group_member_info::Group_member_status member_status =
          local_member_info->get_recovery_status();
      if (Group_member_info::MEMBER_ONLINE == member_status ||
          Group_member_info::MEMBER_IN_RECOVERY == member_status) {
        applier_module->get_pipeline_stats_member_collector()
            ->decrement_transactions_waiting_apply();
        applier_module->get_pipeline_stats_member_collector()
            ->increment_transactions_applied();
      }
      if (Group_member_info::MEMBER_IN_RECOVERY == member_status) {
        applier_module->get_pipeline_stats_member_collector()
            ->increment_transactions_applied_during_recovery();
      }
      shared_plugin_stop_lock->release_read_lock();

      if ((Group_member_info::MEMBER_ONLINE == member_status ||
           Group_member_info::MEMBER_IN_RECOVERY == member_status) &&
          transaction_consistency_manager->after_applier_prepare(
              param->gtid_info.sidno, param->gtid_info.gno, param->thread_id,
              member_status)) {
        return 1; /* purecov: inspected */
      }
    }

    return 0;
  }

  if (GR_RECOVERY_CHANNEL == param->rpl_channel_type) {
    return 0;
  }

  if (shared_plugin_stop_lock->try_grab_read_lock()) {
    /* If plugin is stopping, rollback the transaction immediately. */
    return 1;
  }

  if (is_plugin_waiting_to_set_server_read_mode()) {
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CANNOT_EXECUTE_TRANS_WHILE_STOPPING);
    shared_plugin_stop_lock->release_read_lock();
    return 1;
  }

  /* If the plugin is not running, before commit should return success. */
  if (!plugin_is_group_replication_running()) {
    shared_plugin_stop_lock->release_read_lock();
    return 0;
  }

  Group_member_info::Group_member_status member_status =
      local_member_info->get_recovery_status();

  if (member_status == Group_member_info::MEMBER_IN_RECOVERY) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CANNOT_EXECUTE_TRANS_WHILE_RECOVERING);
    shared_plugin_stop_lock->release_read_lock();
    return 1;
    /* purecov: end */
  }

  if (member_status == Group_member_info::MEMBER_ERROR) {
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CANNOT_EXECUTE_TRANS_IN_ERROR_STATE);
    shared_plugin_stop_lock->release_read_lock();
    return 1;
  }

  if (member_status == Group_member_info::MEMBER_OFFLINE) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CANNOT_EXECUTE_TRANS_IN_OFFLINE_MODE);
    shared_plugin_stop_lock->release_read_lock();
    return 1;
    /* purecov: end */
  }

  assert(applier_module != nullptr && recovery_module != nullptr);
  // Transaction information.
  const ulong transaction_size_limit = get_transaction_size_limit();
  my_off_t transaction_size = 0;

  bool is_gtid_specified = param->gtid_info.type == ASSIGNED_GTID;

  mysql::gtid::Tag_plain automatic_tag;
  automatic_tag.clear();

  Gtid gtid = {param->gtid_info.sidno, param->gtid_info.gno};
  if (!is_gtid_specified) {
    // sidno and gno are dummy values that will be replaced after certification
    gtid.sidno = 1;
    gtid.gno = 1;
    automatic_tag = param->gtid_info.automatic_tag;
  }

  Gtid_specification gtid_specification = {ASSIGNED_GTID, gtid, automatic_tag};
  Gtid_log_event *gle = nullptr;

  Transaction_context_log_event *tcle = nullptr;

  const enum_group_replication_consistency_level consistency_level =
      static_cast<enum_group_replication_consistency_level>(
          param->group_replication_consistency);

  Transaction_message_interface *transaction_msg = nullptr;
  enum enum_gcs_error send_error = GCS_OK;

  // Binlog cache.
  /*
    Atomic DDL:s are logged through the transactional cache so they should
    be exempted from considering as DML by the plugin: not
    everything that is in the trans cache is actually DML.
  */
  bool is_dml = !param->is_atomic_ddl;
  bool may_have_sbr_stmts = !is_dml;
  Binlog_cache_storage *cache_log = nullptr;
  my_off_t cache_log_position = 0;
  const my_off_t trx_cache_log_position = param->trx_cache_log->length();
  const my_off_t stmt_cache_log_position = param->stmt_cache_log->length();
  unsigned long long immediate_commit_timestamp = 0;

  if (trx_cache_log_position > 0 && stmt_cache_log_position == 0) {
    cache_log = param->trx_cache_log;
    cache_log_position = trx_cache_log_position;
  } else if (trx_cache_log_position == 0 && stmt_cache_log_position > 0) {
    cache_log = param->stmt_cache_log;
    cache_log_position = stmt_cache_log_position;
    is_dml = false;
    may_have_sbr_stmts = true;
  } else {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL,
                 ER_GRP_RPL_MULTIPLE_CACHE_TYPE_NOT_SUPPORTED_FOR_SESSION,
                 param->thread_id);
    shared_plugin_stop_lock->release_read_lock();
    return 1;
    /* purecov: end */
  }

  applier_module->get_pipeline_stats_member_collector()
      ->increment_transactions_local();

  DBUG_PRINT("cache_log", ("thread_id: %u, trx_cache_log_position: %llu,"
                           " stmt_cache_log_position: %llu",
                           param->thread_id, trx_cache_log_position,
                           stmt_cache_log_position));

  // Create transaction context.
  tcle = new Transaction_context_log_event(param->server_uuid,
                                           is_dml || param->is_atomic_ddl,
                                           param->thread_id, is_gtid_specified);
  if (!tcle->is_valid()) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_FAILED_TO_CREATE_TRANS_CONTEXT,
                 param->thread_id);
    error = pre_wait_error;
    goto err;
    /* purecov: end */
  }

  if (is_dml) {
    Rpl_transaction_write_set_ctx *transaction_write_set_ctx =
        param->thd->get_transaction()->get_transaction_write_set_ctx();

    std::vector<uint64> *write_set = transaction_write_set_ctx->get_write_set();
    /*
      When GTID is specified we may have empty transactions, that is,
      a transaction may have not write set at all because it didn't
      change any data, it will just persist that GTID as applied.
    */
    if ((write_set->size() == 0) && (!is_gtid_specified) &&
        (!param->is_create_table_as_query_block)) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_FAILED_TO_EXTRACT_TRANS_WRITE_SET,
                   param->thread_id);
      error = pre_wait_error;
      goto err;
    }

    if (write_set->size() > 0) {
      if (add_write_set(tcle, write_set, param->thd)) {
        LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_FAILED_TO_GATHER_TRANS_WRITE_SET,
                     param->thread_id);
        error = pre_wait_error;
        goto err;
      }
      assert(is_gtid_specified || (tcle->get_write_set()->size() > 0));
    } else {
      /*
        For empty transactions we should set the GTID may_have_sbr_stmts. See
        comment at binlog_cache_data::may_have_sbr_stmts().
      */
      may_have_sbr_stmts = true;

      /*
        Empty transactions, despite not having write-set, can be
        applied in parallel with any other transaction, as such we
        flag those transactions with `last_committed = -1`.
      */
      if (is_gtid_specified && !param->is_create_table_as_query_block) {
        last_committed = -1;
      }
    }

    DBUG_EXECUTE_IF("group_replication_after_add_write_set", {
      const char act[] =
          "now signal signal.group_replication_after_add_write_set_reached "
          "wait_for signal.group_replication_after_add_write_set_continue";
      assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
    });

    /*
      'CREATE TABLE ... AS SELECT' is considered a DML, though in reality it
      is DDL + DML, which write-sets do not capture all dependencies.
      Example:
        CREATE TABLE test.t1 (c1 INT NOT NULL PRIMARY KEY);
        INSERT INTO test.t1 VALUES (99);
        CREATE TABLE test.t2 (c1 INT NOT NULL PRIMARY KEY) AS SELECT * FROM
      test.t1; The 'CREATE TABLE ... AS SELECT' does depend on the INSERT on t1,
      though these transactions have non intersecting write-sets, the INSERT
      will refer to table t1 and the 'CREATE TABLE ... AS SELECT' to table t2.
      As such we need to say that 'CREATE TABLE ... AS SELECT' can only be
      executed on parallel applier after all precedent transactions like any
      other DDL.
    */
    if (param->is_create_table_as_query_block) {
      sequence_number = 0;
      may_have_sbr_stmts = true;
    }
  }

  if (*(param->original_commit_timestamp) == UNDEFINED_COMMIT_TIMESTAMP) {
    /*
     Assume that this transaction is original from this server and update status
     variable so that it won't be re-defined when this GTID is written to the
     binlog
    */
    *(param->original_commit_timestamp) = immediate_commit_timestamp =
        my_micro_time();
  } else {  // the transaction did not originate in this server
    immediate_commit_timestamp = my_micro_time();
  }

  *(param->immediate_server_version) = do_server_version_int(::server_version);
  if (*(param->original_server_version) == UNDEFINED_SERVER_VERSION) {
    /*
     Assume that this transaction is original from this server and update status
     variable so that it won't be re-defined when this GTID is written to the
     binlog
    */
    *(param->original_server_version) = do_server_version_int(::server_version);
    DBUG_EXECUTE_IF("gr_fixed_server_version",
                    *(param->original_server_version) = 777777;);
  }  // otherwise the transaction did not originate in this server

  // Notice the GTID of atomic DDL is written to the trans cache as well.
  gle = new Gtid_log_event(
      param->server_id, is_dml || param->is_atomic_ddl, last_committed,
      sequence_number, may_have_sbr_stmts, *(param->original_commit_timestamp),
      immediate_commit_timestamp, gtid_specification,
      *(param->original_server_version), *(param->immediate_server_version));
  /*
    GR does not support event checksumming. If GR start to support event
    checksumming, the calculation below should take the checksum payload into
    account.
  */
  gle->set_trx_length_by_cache_size(cache_log_position);

  /*
    Only proceed to message serialization and send if the transaction
    size does not exceed the limit.
  */
  transaction_size =
      cache_log_position + tcle->get_event_length() + gle->get_event_length();
  if (is_dml && transaction_size_limit &&
      transaction_size > transaction_size_limit) {
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_TRANS_SIZE_EXCEEDS_LIMIT,
                 param->thread_id, transaction_size, transaction_size_limit);
    error = pre_wait_error;
    goto err;
  }

  /*
    Serialize transaction context, Gtid and binlog cache into
    transaction message.
    There is a chance we encounter an OOM, thence consider it.
  */
  try {
    /*
      The BEFORE consistency can be used on groups with members that
      do not support GROUP_REPLICATION_CONSISTENCY_BEFORE. In order to
      allow that, after the wait is done on the transaction begin on
      the local member, we broadcast the transaction as a normal
      transaction that all versions do understand.
    */
    if (consistency_level < GROUP_REPLICATION_CONSISTENCY_AFTER) {
      transaction_msg = new Transaction_message(transaction_size);
    } else {
      transaction_msg = new Transaction_with_guarantee_message(
          transaction_size, consistency_level);
    }

    // transaction_message use memory key being tracked for connection limits,
    // if exceed thread will be killed
    if (param->thd->is_killed()) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_CONN_KILLED,
                   "Generate transaction message failed");
      error = pre_wait_error;
      goto err;
    }

    if (binary_event_serialize(tcle, transaction_msg) ||
        binary_event_serialize(gle, transaction_msg)) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_WRITE_TO_TRANSACTION_MESSAGE_FAILED,
                   param->thread_id);
      error = pre_wait_error;
      goto err;
    }
    /* Release memory as soon as possible. */
    delete tcle;
    tcle = nullptr;
    delete gle;
    gle = nullptr;

    if (cache_log->copy_to(transaction_msg)) {
      LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_WRITE_TO_TRANSACTION_MESSAGE_FAILED,
                   param->thread_id);
      error = pre_wait_error;
      goto err;
    }
  } catch (const std::bad_alloc &) {
    LogPluginErr(ERROR_LEVEL, ER_OUT_OF_RESOURCES);
    error = pre_wait_error;
    goto err;
  }

  if (transactions_latch->registerTicket(param->thread_id)) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL,
                 ER_GRP_RPL_FAILED_TO_REGISTER_TRANS_OUTCOME_NOTIFICTION,
                 param->thread_id);
    error = pre_wait_error;
    goto err;
    /* purecov: end */
  }

#ifndef NDEBUG
  DBUG_EXECUTE_IF("test_basic_CRUD_operations_sql_service_interface", {
    DBUG_SET("-d,test_basic_CRUD_operations_sql_service_interface");
    assert(!sql_command_check());
  };);

  DBUG_EXECUTE_IF("group_replication_before_message_broadcast", {
    const char act[] =
        "now signal signal.group_replication_before_message_broadcast_reached "
        "wait_for waiting";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });
#endif

  /*
    Check if member needs to throttle its transactions to avoid
    cause starvation on the group.
  */
  applier_module->get_flow_control_module()->do_wait();

  // Broadcast the Transaction Message
  send_error = gcs_module->send_transaction_message(*transaction_msg);

  /* Release memory as soon as possible. */
  delete transaction_msg;
  transaction_msg = nullptr;

  if (send_error == GCS_MESSAGE_TOO_BIG) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_MSG_TOO_LONG_BROADCASTING_TRANS_FAILED,
                 param->thread_id);
    error = pre_wait_error;
    goto err;
    /* purecov: end */
  } else if (send_error == GCS_NOK) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL, ER_GRP_RPL_BROADCASTING_TRANS_TO_GRP_FAILED,
                 param->thread_id);
    error = pre_wait_error;
    goto err;
    /* purecov: end */
  }

  shared_plugin_stop_lock->release_read_lock();

  if (transactions_latch->waitTicket(param->thread_id)) {
    /* purecov: begin inspected */
    LogPluginErr(ERROR_LEVEL,
                 ER_GRP_RPL_ERROR_WHILE_WAITING_FOR_CONFLICT_DETECTION,
                 param->thread_id);
    error = post_wait_error;
    goto err;
    /* purecov: end */
  }

err:
  delete gle;
  delete tcle;
  delete transaction_msg;

  if (error) {
    /* purecov: begin inspected */
    if (error == pre_wait_error) shared_plugin_stop_lock->release_read_lock();

    // Release and remove certification latch ticket.
    transactions_latch->releaseTicket(param->thread_id);
    transactions_latch->waitTicket(param->thread_id);
    /* purecov: end */
  }

  DBUG_EXECUTE_IF("group_replication_after_before_commit_hook", {
    const char act[] =
        "now SIGNAL signal.group_replication_after_before_commit_hook_reached "
        "WAIT_FOR signal.group_replication_after_before_commit_hook_continue";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });
  return error;
}

int group_replication_trans_before_rollback(Trans_param *) {
  DBUG_TRACE;
  return 0;
}

int group_replication_trans_after_commit(Trans_param *param) {
  DBUG_TRACE;
  int error = 0;

  /**
    We don't use locks here as observers are unregistered before the classes
    used here disappear. Unregistration also avoids usage vs removal scenarios.
  */

  /*
    If the plugin is not running, after commit should return success.
    If there are no observers, we also don't care
  */
  if (!plugin_is_group_replication_running() ||
      !group_transaction_observation_manager->is_any_observer_present()) {
    return 0;
  }

  group_transaction_observation_manager->read_lock_observer_list();
  std::list<Group_transaction_listener *> *transaction_observers =
      group_transaction_observation_manager->get_all_observers();
  for (Group_transaction_listener *transaction_observer :
       *transaction_observers) {
    transaction_observer->after_commit(param->thread_id, param->gtid_info.sidno,
                                       param->gtid_info.gno);
  }
  group_transaction_observation_manager->unlock_observer_list();
  return error;
}

int group_replication_trans_after_rollback(Trans_param *param) {
  DBUG_TRACE;

  int error = 0;

  /*
    If the plugin is not running, after rollback should return success.
    If there are no observers, we also don't care
  */
  if (!plugin_is_group_replication_running() ||
      !group_transaction_observation_manager->is_any_observer_present()) {
    return 0;
  }

  group_transaction_observation_manager->read_lock_observer_list();
  std::list<Group_transaction_listener *> *transaction_observers =
      group_transaction_observation_manager->get_all_observers();
  for (Group_transaction_listener *transaction_observer :
       *transaction_observers) {
    transaction_observer->after_rollback(param->thread_id);
  }
  group_transaction_observation_manager->unlock_observer_list();

  return error;
}

int group_replication_trans_begin(Trans_param *param, int &out) {
  DBUG_TRACE;

  /*
    If the plugin is not running, before begin should return success.
    If there are no observers, we also don't care
  */
  if (!plugin_is_group_replication_running() ||
      !group_transaction_observation_manager->is_any_observer_present()) {
    return 0;
  }

  group_transaction_observation_manager->read_lock_observer_list();

  DBUG_EXECUTE_IF("group_replication_wait_on_observer_trans", {
    const char act[] =
        "now signal signal.group_replication_wait_on_observer_trans_waiting "
        "wait_for signal.group_replication_wait_on_observer_trans_continue";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  std::list<Group_transaction_listener *> *transaction_observers =
      group_transaction_observation_manager->get_all_observers();
  for (Group_transaction_listener *transaction_observer :
       *transaction_observers) {
    out = transaction_observer->before_transaction_begin(
        param->thread_id, param->group_replication_consistency,
        param->hold_timeout, param->rpl_channel_type, param->thd);
    if (out) break;
  }
  group_transaction_observation_manager->unlock_observer_list();

  return 0;
}

Trans_observer trans_observer = {
    sizeof(Trans_observer),

    group_replication_trans_before_dml,
    group_replication_trans_before_commit,
    group_replication_trans_before_rollback,
    group_replication_trans_after_commit,
    group_replication_trans_after_rollback,
    group_replication_trans_begin,
};
