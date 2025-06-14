/* Copyright (c) 2011, 2025, Oracle and/or its affiliates.

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

#include "sql/rpl_rli_pdb.h"

#include "my_config.h"

#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "lex_string.h"
#include "m_string.h"
#include "map_helpers.h"
#include "my_bitmap.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "my_systime.h"
#include "my_thread.h"
#include "mysql/components/services/bits/psi_stage_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/thread_type.h"
#include "mysqld_error.h"
#include "scope_guard.h"  // Scope_guard
#include "sql/binlog.h"
#include "sql/binlog_reader.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"
#include "sql/log.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"  // key_mutex_slave_parallel_worker
#include "sql/psi_memory_key.h"
#include "sql/raii/sentry.h"  // raii::Sentry<>
#include "sql/rpl_info_handler.h"
#include "sql/rpl_msr.h"                           // For channel_map
#include "sql/rpl_replica_commit_order_manager.h"  // Commit_order_manager
#include "sql/rpl_reporting.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "sql/transaction_info.h"
#include "string_with_len.h"
#include "strmake.h"
#include "thr_mutex.h"

#ifndef NDEBUG
ulong w_rr = 0;
uint mta_debug_concurrent_access = 0;
#endif

#define HASH_DYNAMIC_INIT 4

using std::max;
using std::min;

/**
   This function is called by both coordinator and workers.

   Upon receiving the STOP command, the workers will identify a
   maximum group index already executed (or under execution).

   All groups whose index are below or equal to the maximum
   group index will be applied by the workers before stopping.

   The workers with groups above the maximum group index will
   exit without applying these groups by setting their running
   status to "STOP_ACCEPTED".

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return true if STOP command gets accepted otherwise false is returned.
*/
bool handle_slave_worker_stop(Slave_worker *worker, Slave_job_item *job_item) {
  ulonglong group_index = 0;
  Relay_log_info *rli = worker->c_rli;
  mysql_mutex_lock(&rli->exit_count_lock);
  /*
    First, W calculates a group-"at-hands" index which is
    either the currently read ev group index, or the last executed
    group's one when the  queue is empty.
  */
  group_index =
      (job_item->data)
          ? rli->gaq->get_job_group(job_item->data->mts_group_idx)->total_seqno
          : worker->last_groups_assigned_index;

  /*
    The max updated index is being updated as long as
    exit_counter permits. That's stopped with the final W's
    increment of it.
  */
  if (!worker->exit_incremented) {
    if (rli->exit_counter < rli->replica_parallel_workers)
      rli->max_updated_index = max(rli->max_updated_index, group_index);

    ++rli->exit_counter;
    worker->exit_incremented = true;
    assert(!is_mts_worker(current_thd));
  }
#ifndef NDEBUG
  else
    assert(is_mts_worker(current_thd));
#endif

  /*
    Now let's decide about the deferred exit to consider
    the empty queue and the counter value reached
    replica_parallel_workers.
  */
  if (!job_item->data) {
    worker->running_status = Slave_worker::STOP_ACCEPTED;
    mysql_cond_signal(&worker->jobs_cond);
    mysql_mutex_unlock(&rli->exit_count_lock);
    return (true);
  } else if (rli->exit_counter == rli->replica_parallel_workers) {
    // over steppers should exit with accepting STOP
    if (group_index > rli->max_updated_index) {
      worker->running_status = Slave_worker::STOP_ACCEPTED;
      mysql_cond_signal(&worker->jobs_cond);
      mysql_mutex_unlock(&rli->exit_count_lock);
      return (true);
    }
  }
  mysql_mutex_unlock(&rli->exit_count_lock);
  return (false);
}

/**
   This function is called by both coordinator and workers.
   Both coordinator and workers contribute to max_updated_index.

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return true if STOP command gets accepted otherwise false is returned.
*/
bool set_max_updated_index_on_stop(Slave_worker *worker,
                                   Slave_job_item *job_item) {
  const auto head = worker->jobs.head_queue();
  if (head != nullptr) {
    *job_item = *head;
  } else {
    job_item->data = nullptr;
  }
  if (worker->running_status == Slave_worker::STOP) {
    if (handle_slave_worker_stop(worker, job_item)) return true;
  }
  return false;
}

/*
  Please every time you add a new field to the worker slave info, update
  what follows. For now, this is just used to get the number of fields.
*/
const char *info_slave_worker_fields[] = {
    "id",
    /*
      These positions identify what has been executed. Notice that they are
      redundant and only the group_master_log_name and group_master_log_pos
      are really necessary. However, the additional information is kept to
      ease debugging.
    */
    "group_relay_log_name", "group_relay_log_pos", "group_source_log_name",
    "group_source_log_pos",

    /*
      These positions identify what a worker knew about the coordinator at
      the time a job was assigned. Notice that they are redundant and are
      kept to ease debugging.
    */
    "checkpoint_relay_log_name", "checkpoint_relay_log_pos",
    "checkpoint_source_log_name", "checkpoint_source_log_pos",

    /*
      Identify the greatest job, i.e. group, processed by a worker.
    */
    "checkpoint_seqno",
    /*
      Maximum number of jobs that can be assigned to a worker. This
      information is necessary to read the next entry.
    */
    "checkpoint_group_size",
    /*
      Bitmap used to identify what jobs were processed by a worker.
    */
    "checkpoint_group_bitmap",
    /*
      Channel on which this workers are acting
    */
    "channel_name"};

/*
  Number of records in the mts partition hash below which
  entries with zero usage are tolerated so could be quickly
  recycled.
*/
const ulong mts_partition_hash_soft_max = 16;

/*
  index value of some outstanding slots of info_slave_worker_fields
*/
enum {
  LINE_FOR_CHANNEL = 12,
};

const uint info_slave_worker_table_pk_field_indexes[] = {
    LINE_FOR_CHANNEL,
    0,
};

Slave_worker::Slave_worker(Relay_log_info *rli,
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
                           uint param_id, const char *param_channel)
    : Relay_log_info(false,
#ifdef HAVE_PSI_INTERFACE
                     param_key_info_run_lock, param_key_info_data_lock,
                     param_key_info_sleep_lock, param_key_info_thd_lock,
                     param_key_info_data_cond, param_key_info_start_cond,
                     param_key_info_stop_cond, param_key_info_sleep_cond,
#endif
                     param_id + 1, param_channel, true),
      c_rli(rli),
      curr_group_exec_parts(key_memory_db_worker_hash_entry),
      id(param_id),
      checkpoint_relay_log_pos(0),
      checkpoint_master_log_pos(0),
      worker_checkpoint_seqno(0),
      running_status(NOT_RUNNING),
      exit_incremented(false) {
  /*
    In the future, it would be great if we use only one identifier.
    So when factoring out this code, please, consider this.
  */
  assert(internal_id == id + 1);
  checkpoint_relay_log_name[0] = 0;
  checkpoint_master_log_name[0] = 0;

  mysql_mutex_init(key_mutex_slave_parallel_worker, &jobs_lock,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_cond_slave_parallel_worker, &jobs_cond);
  mysql_cond_init(key_cond_mta_gaq, &logical_clock_cond);
}

Slave_worker::~Slave_worker() {
  end_info();
  if (jobs.inited_queue) {
    assert(jobs.m_Q.size() == jobs.capacity);
    jobs.m_Q.clear();
  }
  mysql_mutex_destroy(&jobs_lock);
  mysql_cond_destroy(&jobs_cond);
  mysql_cond_destroy(&logical_clock_cond);
  mysql_mutex_lock(&info_thd_lock);
  info_thd = nullptr;
  mysql_mutex_unlock(&info_thd_lock);
  if (set_rli_description_event(nullptr)) {
#ifndef NDEBUG
    bool set_rli_description_event_failed = false;
#endif
    assert(set_rli_description_event_failed);
  }
}

/**
   Method is executed by Coordinator at Worker startup time to initialize
   members parly with values supplied by Coordinator through rli.

   @param  rli  Coordinator's Relay_log_info pointer
   @param  i    identifier of the Worker

   @return 0          success
           non-zero   failure
*/
int Slave_worker::init_worker(Relay_log_info *rli, ulong i) {
  DBUG_TRACE;
  assert(!rli->info_thd->is_error());

  Slave_job_item empty = Slave_job_item();

  c_rli = rli;
  this->set_require_row_format(rli->is_row_format_required());

  set_commit_order_manager(c_rli->get_commit_order_manager());

  if (rli_init_info(false) ||
      DBUG_EVALUATE_IF("inject_init_worker_init_info_fault", true, false))
    return 1;

  if (!rli->is_privilege_checks_user_null()) {
    this->set_privilege_checks_user(
        rli->get_privilege_checks_username().c_str(),
        rli->get_privilege_checks_hostname().c_str());
  }

  if (this->m_assign_gtids_to_anonymous_transactions_info.set_info(
          rli->m_assign_gtids_to_anonymous_transactions_info.get_type(),
          (rli->m_assign_gtids_to_anonymous_transactions_info.get_value()
               .c_str())))
    return 1;
  id = i;
  curr_group_exec_parts.clear();
  relay_log_change_notified = false;  // the 1st group to contain relaylog name
  checkpoint_notified = false;        // the same as above
  master_log_change_notified =
      false;                   // W learns master log during 1st group exec
  fd_change_notified = false;  // W is to learn master FD version same as above
  server_version = version_product(rli->slave_version_split);
  bitmap_shifted = 0;
  workers = c_rli->workers;  // shallow copying is sufficient
  transactions_handled = 0;
  worker_queue_mem_exceeded_count = 0;
  curr_jobs = 0;
  usage_partition = 0;
  end_group_sets_max_dbs = false;
  gaq_index = c_rli->gaq->capacity;              // out of range
  last_group_done_index = c_rli->gaq->capacity;  // out of range
  last_groups_assigned_index = 0;
  assert(!jobs.inited_queue);
  jobs.avail = 0;
  jobs.entry = 0;
  jobs.len = 0;
  jobs.overfill = false;  //  todo: move into Slave_jobs_queue constructor
  jobs.waited_overfill = 0;
  jobs.capacity = c_rli->mts_slave_worker_queue_len_max;
  jobs.inited_queue = true;
  curr_group_seen_gtid = false;
#ifndef NDEBUG
  curr_group_seen_sequence_number = false;
#endif
  jobs.m_Q.resize(jobs.capacity, empty);
  assert(jobs.m_Q.size() == jobs.capacity);

  wq_overrun_cnt = excess_cnt = 0;
  underrun_level =
      (ulong)((rli->mts_worker_underrun_level * jobs.capacity) / 100.0);
  // overrun level is symmetric to underrun (as underrun to the full queue)
  overrun_level = jobs.capacity - underrun_level;

  /* create mts submode for each of the the workers. */
  current_mts_submode = (rli->channel_mts_submode == MTS_PARALLEL_TYPE_DB_NAME)
                            ? (Mts_submode *)new Mts_submode_database()
                            : (Mts_submode *)new Mts_submode_logical_clock();

  // workers and coordinator must be of the same type
  assert(rli->current_mts_submode->get_type() ==
         current_mts_submode->get_type());

  reset_commit_order_deadlock();

  set_worker_metric_collection_status(rli->mi->is_metric_collection_enabled());

  return 0;
}

/**
   A part of Slave worker initializer that provides a
   minimum context for MTS recovery.

   @param is_gaps_collecting_phase
          clarifies what state the caller
          executes this method from. When it's @c true
          that is @c mts_recovery_groups() and Worker should
          restore the last session time info which is processed
          to collect gaps that is not executed transactions (groups).
          Such recovery Slave_worker instance is destroyed at the end of
          @c mts_recovery_groups().
          When it's @c false Slave_worker is initialized for the run time
          and should not read the last session time stale info.
          Its info will be ultimately reset once all gaps are executed
          to finish off recovery.

   @return 0 on success, non-zero for a failure
*/
int Slave_worker::rli_init_info(bool is_gaps_collecting_phase) {
  enum_return_check return_check = ERROR_CHECKING_REPOSITORY;

  DBUG_TRACE;

  if (inited) return 0;

  /*
    Worker bitmap size depends on recovery mode.
    If it is gaps collecting the bitmaps must be capable to accept
    up to MTS_MAX_BITS_IN_GROUP of bits.
  */
  size_t num_bits = is_gaps_collecting_phase ? MTS_MAX_BITS_IN_GROUP
                                             : c_rli->checkpoint_group;
  /*
    This checks if the repository was created before and thus there
    will be values to be read. Please, do not move this call after
    the handler->init_info().
  */
  return_check = check_info();
  if (return_check == ERROR_CHECKING_REPOSITORY ||
      (return_check == REPOSITORY_DOES_NOT_EXIST && is_gaps_collecting_phase))
    goto err;

  if (handler->init_info()) goto err;

  bitmap_init(&group_executed, nullptr, num_bits);
  bitmap_init(&group_shifted, nullptr, num_bits);

  if (is_gaps_collecting_phase &&
      (DBUG_EVALUATE_IF("mta_replica_worker_init_at_gaps_fails", true, false) ||
       read_info(handler))) {
    bitmap_free(&group_executed);
    bitmap_free(&group_shifted);
    goto err;
  }
  inited = true;

  return 0;

err:
  // todo: handler->end_info(uidx, nidx);
  inited = false;
  LogErr(ERROR_LEVEL, ER_RPL_ERROR_READING_REPLICA_WORKER_CONFIGURATION);
  return 1;
}

void Slave_worker::end_info() {
  DBUG_TRACE;

  if (!inited) return;

  if (handler) handler->end_info();

  if (inited) {
    bitmap_free(&group_executed);
    bitmap_free(&group_shifted);
  }
  inited = false;
}

int Slave_worker::flush_info(const bool force) {
  DBUG_TRACE;

  if (!inited) return 0;

  if (c_rli->mi->is_gtid_only_mode()) return 0;

  /*
    We update the sync_period at this point because only here we
    now that we are handling a Slave_worker. This needs to be
    update every time we call flush because the option may be
    dynamically set.
  */
  handler->set_sync_period(sync_relayloginfo_period);

  /*
    This only fails on out-of-memory errors, which are reported (using
    the MY_WME flag to my_malloc).
  */
  if (write_info(handler)) return 1;

  /*
    This fails on errors committing the info, or when
    replica_preserve_commit_order is enabled and a previous transaction
    has failed.  In both cases, the error is reported already.
  */
  if (handler->flush_info(force)) return 1;

  return 0;
}

bool Slave_worker::read_info(Rpl_info_handler *from) {
  DBUG_TRACE;

  ulong temp_group_relay_log_pos = 0;
  ulong temp_group_master_log_pos = 0;
  ulong temp_checkpoint_relay_log_pos = 0;
  ulong temp_checkpoint_master_log_pos = 0;
  ulong temp_checkpoint_seqno = 0;
  ulong nbytes = 0;
  uchar *buffer = (uchar *)group_executed.bitmap;
  int temp_internal_id = 0;

  if (from->prepare_info_for_read()) return true;

  if (!!from->get_info(&temp_internal_id, 0) ||
      !!from->get_info(group_relay_log_name, sizeof(group_relay_log_name),
                       "") ||
      !!from->get_info(&temp_group_relay_log_pos, 0UL) ||
      !!from->get_info(group_master_log_name, sizeof(group_master_log_name),
                       "") ||
      !!from->get_info(&temp_group_master_log_pos, 0UL) ||
      !!from->get_info(checkpoint_relay_log_name,
                       sizeof(checkpoint_relay_log_name), "") ||
      !!from->get_info(&temp_checkpoint_relay_log_pos, 0UL) ||
      !!from->get_info(checkpoint_master_log_name,
                       sizeof(checkpoint_master_log_name), "") ||
      !!from->get_info(&temp_checkpoint_master_log_pos, 0UL) ||
      !!from->get_info(&temp_checkpoint_seqno, 0UL) ||
      !!from->get_info(&nbytes, 0UL) ||
      !!from->get_info(buffer, (size_t)nbytes, (uchar *)nullptr) ||
      /* default is empty string */
      !!from->get_info(channel, sizeof(channel), ""))
    return true;

  assert(nbytes <= no_bytes_in_map(&group_executed));

  internal_id = (uint)temp_internal_id;
  group_relay_log_pos = temp_group_relay_log_pos;
  group_master_log_pos = temp_group_master_log_pos;
  checkpoint_relay_log_pos = temp_checkpoint_relay_log_pos;
  checkpoint_master_log_pos = temp_checkpoint_master_log_pos;
  worker_checkpoint_seqno = temp_checkpoint_seqno;

  return false;
}

/*
  This function is used to make a copy of the worker object before we
  destroy it while STOP REPLICA. This new object is then used to report the
  worker status until next START REPLICA following which the new worker objects
  will be used.
*/
void Slave_worker::copy_values_for_PFS(ulong worker_id,
                                       en_running_state thd_running_status,
                                       THD *worker_thd, const Error &last_error,
                                       Gtid_monitoring_info *monitoring_info) {
  id = worker_id;
  running_status = thd_running_status;
  info_thd = worker_thd;
  m_last_error = last_error;
  monitoring_info->copy_info_to(get_gtid_monitoring_info());
}

bool Slave_worker::set_info_search_keys(Rpl_info_handler *to) {
  DBUG_TRACE;

  /* primary keys are Id and channel_name */
  if (to->set_info(0, (int)internal_id) ||
      to->set_info(LINE_FOR_CHANNEL, channel))
    return true;

  return false;
}

bool Slave_worker::write_info(Rpl_info_handler *to) {
  DBUG_TRACE;

  ulong nbytes = (ulong)no_bytes_in_map(&group_executed);
  uchar *buffer = (uchar *)group_executed.bitmap;
  assert(nbytes <= (c_rli->checkpoint_group + 7) / 8);

  if (to->prepare_info_for_write() || to->set_info((int)internal_id) ||
      to->set_info(group_relay_log_name) ||
      to->set_info((ulong)group_relay_log_pos) ||
      to->set_info(group_master_log_name) ||
      to->set_info((ulong)group_master_log_pos) ||
      to->set_info(checkpoint_relay_log_name) ||
      to->set_info((ulong)checkpoint_relay_log_pos) ||
      to->set_info(checkpoint_master_log_name) ||
      to->set_info((ulong)checkpoint_master_log_pos) ||
      to->set_info(worker_checkpoint_seqno) || to->set_info(nbytes) ||
      to->set_info(buffer, (size_t)nbytes) || to->set_info(channel))
    return true;

  return false;
}

/**
   Clean up a part of Worker info table that is regarded in
   in gaps collecting at recovery.
   This worker won't contribute to recovery bitmap at future
   slave restart (see @c mts_recovery_groups).

   @return false as success true as failure
*/
bool Slave_worker::reset_recovery_info() {
  DBUG_TRACE;

  set_group_master_log_name("");
  set_group_master_log_pos(0);

  return flush_info(true);
}

size_t Slave_worker::get_number_worker_fields() {
  return sizeof(info_slave_worker_fields) / sizeof(info_slave_worker_fields[0]);
}

void Slave_worker::set_nullable_fields(MY_BITMAP *nullable_fields) {
  bitmap_init(nullable_fields, nullptr,
              Slave_worker::get_number_worker_fields());
  bitmap_clear_all(nullable_fields);
}

const char *Slave_worker::get_master_log_name() {
  Slave_job_group *ptr_g = c_rli->gaq->get_job_group(gaq_index);

  return (ptr_g->checkpoint_log_name != nullptr) ? ptr_g->checkpoint_log_name
                                                 : checkpoint_master_log_name;
}

bool Slave_worker::commit_positions(Log_event *ev, Slave_job_group *ptr_g,
                                    bool force) {
  DBUG_TRACE;

  /*
    Initial value of checkpoint_master_log_name is learned from
    group_master_log_name. The latter can be passed to Worker
    at rare event of master binlog rotation.
    This initialization is needed to provide to Worker info
    on physical coordinates during execution of the very first group
    after a rotation.
  */
  if (ptr_g->group_master_log_name != nullptr) {
    my_claim(ptr_g->group_master_log_name, /*claim=*/true);
    strmake(group_master_log_name, ptr_g->group_master_log_name,
            sizeof(group_master_log_name) - 1);
    my_free(ptr_g->group_master_log_name);
    ptr_g->group_master_log_name = nullptr;
    strmake(checkpoint_master_log_name, group_master_log_name,
            sizeof(checkpoint_master_log_name) - 1);
  }
  if (ptr_g->checkpoint_log_name != nullptr) {
    my_claim(ptr_g->checkpoint_log_name, /*claim=*/true);
    my_claim(ptr_g->checkpoint_relay_log_name, /*claim=*/true);

    strmake(checkpoint_relay_log_name, ptr_g->checkpoint_relay_log_name,
            sizeof(checkpoint_relay_log_name) - 1);
    checkpoint_relay_log_pos = ptr_g->checkpoint_relay_log_pos;
    strmake(checkpoint_master_log_name, ptr_g->checkpoint_log_name,
            sizeof(checkpoint_master_log_name) - 1);
    checkpoint_master_log_pos = ptr_g->checkpoint_log_pos;

    my_free(ptr_g->checkpoint_log_name);
    ptr_g->checkpoint_log_name = nullptr;
    my_free(ptr_g->checkpoint_relay_log_name);
    ptr_g->checkpoint_relay_log_name = nullptr;

    bitmap_copy(&group_shifted, &group_executed);
    bitmap_clear_all(&group_executed);
    for (uint pos = ptr_g->shifted; pos < c_rli->checkpoint_group; pos++) {
      if (bitmap_is_set(&group_shifted, pos))
        bitmap_set_bit(&group_executed, pos - ptr_g->shifted);
    }
  }
  /*
    Extracts an updated relay-log name to store in Worker's rli.
  */
  if (ptr_g->group_relay_log_name) {
    assert(strlen(ptr_g->group_relay_log_name) + 1 <=
           sizeof(group_relay_log_name));
    strmake(group_relay_log_name, ptr_g->group_relay_log_name,
            sizeof(group_relay_log_name) - 1);
  }

  assert(ptr_g->checkpoint_seqno <= (c_rli->checkpoint_group - 1));

  bitmap_set_bit(&group_executed, ptr_g->checkpoint_seqno);
  worker_checkpoint_seqno = ptr_g->checkpoint_seqno;
  group_relay_log_pos = ev->future_event_relay_log_pos;
  group_master_log_pos = ev->common_header->log_pos;

  /*
    Directly accessing c_rli->get_group_master_log_name() does not
    represent a concurrency issue because the current code places
    a synchronization point when master rotates.
  */
  strmake(group_master_log_name, c_rli->get_group_master_log_name(),
          sizeof(group_master_log_name) - 1);

  DBUG_PRINT("mta", ("Committing worker-id %lu group source log pos %llu "
                     "group source log name %s checkpoint sequence number %lu.",
                     id, group_master_log_pos, group_master_log_name,
                     worker_checkpoint_seqno));

  DBUG_EXECUTE_IF("mta_debug_concurrent_access",
                  { mta_debug_concurrent_access++; };);

  m_flag_positions_committed = true;
  return flush_info(force);
}

void Slave_worker::rollback_positions(Slave_job_group *ptr_g) {
  if (!is_transactional()) {
    bitmap_clear_bit(&group_executed, ptr_g->checkpoint_seqno);
    flush_info(false);
  }
}

static void free_entry(db_worker_hash_entry *entry) {
  THD *c_thd = current_thd;

  DBUG_TRACE;

  DBUG_PRINT("info", ("free_entry %s, %zu", entry->db, strlen(entry->db)));

  assert(c_thd->system_thread == SYSTEM_THREAD_SLAVE_SQL);

  /*
    Although assert is correct valgrind senses entry->worker can be freed.

    assert(entry->usage == 0 ||
                !entry->worker    ||  // last entry owner could have errored out
                entry->worker->running_status != Slave_worker::RUNNING);
  */

  mts_move_temp_tables_to_thd(c_thd, entry->temporary_tables);
  entry->temporary_tables = nullptr;

  my_free(const_cast<char *>(entry->db));
  my_free(entry);
}

bool init_hash_workers(Relay_log_info *rli) {
  DBUG_TRACE;

  rli->inited_hash_workers = true;
  mysql_mutex_init(key_mutex_replica_worker_hash, &rli->slave_worker_hash_lock,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_cond_slave_worker_hash, &rli->slave_worker_hash_cond);

  return false;
}

void destroy_hash_workers(Relay_log_info *rli) {
  DBUG_TRACE;
  if (rli->inited_hash_workers) {
    rli->mapping_db_to_worker.clear();
    mysql_mutex_destroy(&rli->slave_worker_hash_lock);
    mysql_cond_destroy(&rli->slave_worker_hash_cond);
    rli->inited_hash_workers = false;
  }
}

/**
   Relocating temporary table reference into @c entry's table list head.
   Sources can be the coordinator's and the Worker's thd->temporary_tables.

   @param table   TABLE instance pointer
   @param thd     THD instance pointer of the source of relocation
   @param entry   db_worker_hash_entry instance pointer

   @note  thd->temporary_tables can become NULL

   @return the pointer to a table following the unlinked
*/
TABLE *mts_move_temp_table_to_entry(TABLE *table, THD *thd,
                                    db_worker_hash_entry *entry) {
  TABLE *ret = table->next;

  if (table->prev) {
    table->prev->next = table->next;
    if (table->prev->next) table->next->prev = table->prev;
  } else {
    /* removing the first item from the list */
    assert(table == thd->temporary_tables);

    thd->temporary_tables = table->next;
    if (thd->temporary_tables) table->next->prev = nullptr;
  }
  table->next = entry->temporary_tables;
  table->prev = nullptr;
  if (table->next) table->next->prev = table;
  entry->temporary_tables = table;

  return ret;
}

/**
   Relocation of the list of temporary tables to thd->temporary_tables.

   @param thd     THD instance pointer of the destination
   @param temporary_tables
                  the source temporary_tables list

   @note     destroying references to the source list, if necessary,
             is left to the caller.

   @return   the post-merge value of thd->temporary_tables.
*/
TABLE *mts_move_temp_tables_to_thd(THD *thd, TABLE *temporary_tables) {
  DBUG_TRACE;
  TABLE *table = temporary_tables;
  if (!table) return nullptr;

  // accept only if this is the start of the list.
  assert(!table->prev);

  // walk along the source list and associate the tables with thd
  do {
    table->in_use = thd;
  } while (table->next && (table = table->next));

  // link the former list against the tail of the source list
  if (thd->temporary_tables) thd->temporary_tables->prev = table;
  table->next = thd->temporary_tables;
  thd->temporary_tables = temporary_tables;
  return thd->temporary_tables;
}

/**
   Relocating references of temporary tables of a database
   of the entry argument from THD into the entry.

   @param thd    THD pointer of the source temporary_tables list
   @param entry  a pointer to db_worker_hash_entry record
                 containing database descriptor and temporary_tables list.

*/
static void move_temp_tables_to_entry(THD *thd, db_worker_hash_entry *entry) {
  for (TABLE *table = thd->temporary_tables; table;) {
    if (strcmp(table->s->db.str, entry->db) == 0) {
      // table pointer is shifted inside the function
      table = mts_move_temp_table_to_entry(table, thd, entry);
    } else {
      table = table->next;
    }
  }
}

/**
   The function produces a reference to the struct of a Worker
   that has been or will be engaged to process the @c dbname -keyed  partition
   (D). It checks a local to Coordinator CGAP list first and returns
   @c last_assigned_worker when found (todo: assert).

   Otherwise, the partition is appended to the current group list:

        CGAP .= D

   here .= is concatenate operation,
   and a possible D's Worker id is searched in Assigned Partition Hash
   (APH) that collects tuples (P, W_id, U, mutex, cond).
   In case not found,

        W_d := W_c unless W_c is NULL.

   When W_c is NULL it is assigned to a least occupied as defined by
   @c get_least_occupied_worker().

        W_d := W_c := W_{least_occupied}

        APH .=  a new (D, W_d, 1)

   In a case APH contains W_d == W_c, (assert U >= 1)

        update APH set  U++ where  APH.P = D

   The case APH contains a W_d != W_c != NULL assigned to D-partition represents
   the hashing conflict and is handled as the following:

     a. marks the record of APH with a flag requesting to signal in the
        cond var when `U' the usage counter drops to zero by the other Worker;
     b. waits for the other Worker to finish tasks on that partition and
        gets the signal;
     c. updates the APH record to point to the first Worker (naturally, U := 1),
        scheduled the event, and goes back into the parallel mode

   @param  dbname      pointer to c-string containing database name
                       It can be empty string to indicate specific locking
                       to facilitate sequential applying.
   @param  rli         pointer to Coordinators relay-log-info instance
   @param  ptr_entry   reference to a pointer to the resulted entry in
                       the Assigned Partition Hash where
                       the entry's pointer is stored at return.
   @param  need_temp_tables
                       if false migration of temporary tables not needed
   @param  last_worker caller opts for this Worker, it must be
                       rli->last_assigned_worker if one is determined.

   @note modifies  CGAP, APH and unlinks @c dbname -keyd temporary tables
         from C's thd->temporary_tables to move them into the entry record.

   @return the pointer to a Worker struct
*/
Slave_worker *map_db_to_worker(const char *dbname, Relay_log_info *rli,
                               db_worker_hash_entry **ptr_entry,
                               bool need_temp_tables,
                               Slave_worker *last_worker) {
  Slave_worker_array *workers = &rli->workers;

  THD *thd = rli->info_thd;

  DBUG_TRACE;

  assert(!rli->last_assigned_worker ||
         rli->last_assigned_worker == last_worker);
  assert(is_mts_db_partitioned(rli));

  if (!rli->inited_hash_workers) return nullptr;

  db_worker_hash_entry *entry = nullptr;
  size_t dblength = strlen(dbname);

  // Search in CGAP
  for (db_worker_hash_entry **it = rli->curr_group_assigned_parts.begin();
       it != rli->curr_group_assigned_parts.end(); ++it) {
    entry = *it;
    if ((uchar)entry->db_len != dblength)
      continue;
    else if (strncmp(entry->db, const_cast<char *>(dbname), dblength) == 0) {
      *ptr_entry = entry;
      return last_worker;
    }
  }

  DBUG_PRINT("info", ("Searching for %s, %zu", dbname, dblength));

  mysql_mutex_lock(&rli->slave_worker_hash_lock);

  std::string key(dbname, dblength);
  entry = find_or_nullptr(rli->mapping_db_to_worker, key);
  if (!entry) {
    DBUG_PRINT("debug", ("NO ENTRY found for: %s!", dbname));
    /*
      The database name was not found which means that a worker never
      processed events from that database. In such case, we need to
      map the database to a worker my inserting an entry into the
      hash map.
    */
    bool ret;
    char *db = nullptr;

    mysql_mutex_unlock(&rli->slave_worker_hash_lock);

    DBUG_PRINT("info", ("Inserting %s, %zu", dbname, dblength));
    /*
      Allocate an entry to be inserted and if the operation fails
      an error is returned.
    */
    if (!(db = (char *)my_malloc(key_memory_db_worker_hash_entry, dblength + 1,
                                 MYF(0))))
      goto err;
    if (!(entry = (db_worker_hash_entry *)my_malloc(
              key_memory_db_worker_hash_entry, sizeof(db_worker_hash_entry),
              MYF(0)))) {
      my_free(db);
      goto err;
    }
    my_stpcpy(db, dbname);
    entry->db = db;
    entry->db_len = strlen(db);
    entry->usage = 1;
    entry->temporary_tables = nullptr;
    /*
      Unless \exists the last assigned Worker, get a free worker based
      on a policy described in the function get_least_occupied_worker().
    */
    mysql_mutex_lock(&rli->slave_worker_hash_lock);

    entry->worker = (!last_worker)
                        ? get_least_occupied_worker(rli, workers, nullptr)
                        : last_worker;
    entry->worker->usage_partition++;
    if (rli->mapping_db_to_worker.size() > mts_partition_hash_soft_max) {
      /*
        remove zero-usage (todo: rare or long ago scheduled) records.
        Free the element if the usage of the hash entry is 0 or not.
      */
      for (auto it = rli->mapping_db_to_worker.begin();
           it != rli->mapping_db_to_worker.end();) {
        assert(!entry->temporary_tables || !entry->temporary_tables->prev);
        assert(!thd->temporary_tables || !thd->temporary_tables->prev);

        db_worker_hash_entry *zero_entry = it->second.get();
        if (zero_entry->usage == 0) {
          mts_move_temp_tables_to_thd(thd, zero_entry->temporary_tables);
          zero_entry->temporary_tables = nullptr;
          it = rli->mapping_db_to_worker.erase(it);
        } else
          ++it;
      }
    }

    ret =
        !rli->mapping_db_to_worker
             .emplace(entry->db, unique_ptr_with_deleter<db_worker_hash_entry>(
                                     entry, free_entry))
             .second;

    if (ret) {
      my_free(db);
      entry = nullptr;
      goto err;
    }
    DBUG_PRINT("info", ("Inserted %s, %zu", entry->db, strlen(entry->db)));
    DBUG_PRINT("debug", ("worker=%lu, partition=%s, usage=%ld, wait=false!",
                         entry->worker->id, entry->db,
                         entry->worker->usage_partition++));
  } else {
    DBUG_PRINT("debug", ("ENTRY found for: %s!", entry->db));
    /* There is a record. Either  */
    if (entry->usage == 0) {
      entry->worker = (!last_worker)
                          ? get_least_occupied_worker(rli, workers, nullptr)
                          : last_worker;
      entry->worker->usage_partition++;
      entry->usage++;
      DBUG_PRINT(
          "debug",
          ("worker=%lu, partition=%s, usage=%ld (was 0), wait=false!",
           entry->worker->id, entry->db, entry->worker->usage_partition++));
    } else if (entry->worker == last_worker || !last_worker) {
      assert(entry->worker);

      entry->usage++;
      DBUG_PRINT("debug", ("worker=%lu, partition=%s, usage=%ld, wait=false!",
                           entry->worker->id, entry->db,
                           entry->worker->usage_partition++));
    } else {
      // The case APH contains a W_d != W_c != NULL assigned to
      // D-partition represents
      // the hashing conflict and is handled as the following:
      PSI_stage_info old_stage;

      assert(last_worker != nullptr &&
             rli->curr_group_assigned_parts.size() > 0);

      // future assignenment and marking at the same time
      entry->worker = last_worker;
      DBUG_PRINT("debug", ("worker=%lu, partition=%s, usage=%ld, wait=true!",
                           entry->worker->id, entry->db,
                           entry->worker->usage_partition++));
      // loop while a user thread is stopping Coordinator gracefully
      do {
        thd->ENTER_COND(
            &rli->slave_worker_hash_cond, &rli->slave_worker_hash_lock,
            &stage_replica_waiting_worker_to_release_partition, &old_stage);
        mysql_cond_wait(&rli->slave_worker_hash_cond,
                        &rli->slave_worker_hash_lock);
      } while (entry->usage != 0 && !thd->killed);

      mysql_mutex_unlock(&rli->slave_worker_hash_lock);
      thd->EXIT_COND(&old_stage);
      if (thd->killed) {
        entry = nullptr;
        goto err;
      }
      mysql_mutex_lock(&rli->slave_worker_hash_lock);
      entry->usage = 1;
      entry->worker->usage_partition++;
    }
  }

  /*
     relocation belonging to db temporary tables from C to W via entry
  */
  if (entry->usage == 1 && need_temp_tables) {
    if (!entry->temporary_tables) {
      if (entry->db_len != 0) {
        move_temp_tables_to_entry(thd, entry);
      } else {
        entry->temporary_tables = thd->temporary_tables;
        thd->temporary_tables = nullptr;
      }
    }
#ifndef NDEBUG
    else {
      // all entries must have been emptied from temps by the caller

      for (TABLE *table = thd->temporary_tables; table; table = table->next) {
        assert(0 != strcmp(table->s->db.str, entry->db));
      }
    }
#endif
  }
  mysql_mutex_unlock(&rli->slave_worker_hash_lock);

  assert(entry);

err:
  if (entry) {
    DBUG_PRINT("info",
               ("Updating %s with worker %lu", entry->db, entry->worker->id));
    rli->curr_group_assigned_parts.push_back(entry);
    *ptr_entry = entry;
  }
  return entry ? entry->worker : nullptr;
}

/**
   Get the least occupied worker.

   @param rli pointer to Relay_log_info of Coordinator
   @param ws  dynarray of pointers to Slave_worker
   @param ev event for which we are searching for a worker

   @return a pointer to chosen Slave_worker instance

*/
Slave_worker *get_least_occupied_worker(Relay_log_info *rli,
                                        Slave_worker_array *ws, Log_event *ev) {
  return rli->current_mts_submode->get_least_occupied_worker(rli, ws, ev);
}

/**
   Deallocation routine to cancel out few effects of
   @c map_db_to_worker().
   Involved into processing of the group APH tuples are updated.
   @c last_group_done_index member is set to the GAQ index of
   the current group.
   CGEP the Worker partition cache is cleaned up.

   @param ev     a pointer to Log_event
   @param error  error code after processing the event by caller.
*/
void Slave_worker::slave_worker_ends_group(Log_event *ev, int error) {
  DBUG_TRACE;
  Slave_job_group *ptr_g = nullptr;

  if (!error) {
    ptr_g = c_rli->gaq->get_job_group(gaq_index);

    assert(gaq_index == ev->mts_group_idx);
    /*
      It guarantees that the worker is removed from order commit queue when
      its transaction doesn't binlog anything. It will break innodb group
      commit, but it should rarely happen.
    */
    Commit_order_manager::wait_and_finish(info_thd, false);

    // first ever group must have relay log name
    assert(last_group_done_index != c_rli->gaq->capacity ||
           ptr_g->group_relay_log_name != nullptr);
    assert(ptr_g->worker_id == id);

    /*
      DDL that has not yet updated the slave info repository does it now.
    */
    if (!m_flag_positions_committed && !is_committed_ddl(ev)) {
      commit_positions(ev, ptr_g, true);
      DBUG_EXECUTE_IF(
          "crash_after_commit_and_update_pos",
          sql_print_information("Crashing crash_after_commit_and_update_pos.");
          flush_info(true); DBUG_SUICIDE(););
    }
    m_flag_positions_committed = false;  // set to false for the next group

    ptr_g->group_master_log_pos = group_master_log_pos;
    ptr_g->group_relay_log_pos = group_relay_log_pos;
    ptr_g->done.store(1);
    last_group_done_index = gaq_index;
    last_groups_assigned_index = ptr_g->total_seqno;
    reset_gaq_index();
    transactions_handled++;

  } else {
    if (running_status != STOP_ACCEPTED) {
      // tagging as exiting so Coordinator won't be able synchronize with it
      mysql_mutex_lock(&jobs_lock);
      running_status = ERROR_LEAVING;
      mysql_mutex_unlock(&jobs_lock);

      // Fatal error happens, it notifies the following transaction to rollback
      Commit_order_manager::wait_and_finish(info_thd, true);

      // Killing Coordinator to indicate eventual consistency error
      mysql_mutex_lock(&c_rli->info_thd->LOCK_thd_data);
      c_rli->info_thd->awake(THD::KILL_QUERY);
      mysql_mutex_unlock(&c_rli->info_thd->LOCK_thd_data);
    }
  }

  /*
    Cleanup relating to the last executed group regardless of error.
  */
  if (current_mts_submode->get_type() == MTS_PARALLEL_TYPE_DB_NAME) {
#ifndef NDEBUG
    {
      std::stringstream ss;
      for (size_t i = 0; i < curr_group_exec_parts.size(); i++) {
        if (curr_group_exec_parts[i]->db_len) {
          ss << curr_group_exec_parts[i]->db << ", ";
        }
      }
      DBUG_PRINT("debug", ("UNASSIGN %p %s", current_thd, ss.str().c_str()));
    }
#endif
    for (size_t i = 0; i < curr_group_exec_parts.size(); i++) {
      db_worker_hash_entry *entry = curr_group_exec_parts[i];

      mysql_mutex_lock(&c_rli->slave_worker_hash_lock);

      assert(entry);

      entry->usage--;

      assert(entry->usage >= 0);

      if (entry->usage == 0) {
        usage_partition--;
        /*
          The detached entry's temp table list, possibly updated, remains
          with the entry at least until time Coordinator will deallocate it
          from the hash, that is either due to stop or extra size of the hash.
        */
        assert(usage_partition >= 0);
        assert(this->info_thd->temporary_tables == nullptr);
        assert(!entry->temporary_tables || !entry->temporary_tables->prev);

        if (entry->worker != this)  // Coordinator is waiting
        {
          DBUG_PRINT("info", ("Notifying entry %p release by worker %lu", entry,
                              this->id));

          mysql_cond_signal(&c_rli->slave_worker_hash_cond);
        }
      } else
        assert(usage_partition != 0);

      mysql_mutex_unlock(&c_rli->slave_worker_hash_lock);
    }

    curr_group_exec_parts.clear();
    curr_group_exec_parts.shrink_to_fit();

    if (error) {
      // Awakening Coordinator that could be waiting for entry release
      mysql_mutex_lock(&c_rli->slave_worker_hash_lock);
      mysql_cond_signal(&c_rli->slave_worker_hash_cond);
      mysql_mutex_unlock(&c_rli->slave_worker_hash_lock);
    }
  } else  // not DB-type scheduler
  {
    assert(current_mts_submode->get_type() == MTS_PARALLEL_TYPE_LOGICAL_CLOCK);
    /*
      Check if there're any waiter. If there're try incrementing lwm and
      signal to those who've got sasfied with the waiting condition.

      In a "good" "likely" execution branch the waiter set is expected
      to be empty. LWM is advanced by Coordinator asynchronously.
      Also lwm is advanced by a dependent Worker when it inserts its waiting
      request into the waiting list.
    */
    Mts_submode_logical_clock *mts_submode =
        static_cast<Mts_submode_logical_clock *>(c_rli->current_mts_submode);
    int64 min_child_waited_logical_ts =
        mts_submode->min_waited_timestamp.load();

    DBUG_EXECUTE_IF("replica_worker_ends_group_before_signal_lwm", {
      const char act[] = "now WAIT_FOR worker_continue";
      assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
    });

    if (unlikely(error)) {
      mysql_mutex_lock(&c_rli->mts_gaq_LOCK);
      mts_submode->is_error = true;
      if (mts_submode->min_waited_timestamp != SEQ_UNINIT)
        mysql_cond_signal(&c_rli->logical_clock_cond);
      mysql_mutex_unlock(&c_rli->mts_gaq_LOCK);
    } else if (min_child_waited_logical_ts != SEQ_UNINIT) {
      mysql_mutex_lock(&c_rli->mts_gaq_LOCK);

      /*
        min_child_waited_logical_ts may include an old value, so we need to
        check it again after getting the lock.
      */
      if (mts_submode->min_waited_timestamp != SEQ_UNINIT) {
        longlong curr_lwm = mts_submode->get_lwm_timestamp(c_rli, true);

        if (mts_submode->clock_leq(mts_submode->min_waited_timestamp,
                                   curr_lwm)) {
          /*
            There's a transaction that depends on the current.
          */
          mysql_cond_signal(&c_rli->logical_clock_cond);
        }
      }
      mysql_mutex_unlock(&c_rli->mts_gaq_LOCK);
    }

#ifndef NDEBUG
    curr_group_seen_sequence_number = false;
#endif
  }
  curr_group_seen_gtid = false;
}

Slave_committed_queue::Slave_committed_queue(size_t max, uint n)
    : circular_buffer_queue<Slave_job_group>(max),
      inited(false),
      last_done(key_memory_Replica_job_group_group_relay_log_name) {
  if (max >= (ulong)-1 || !inited_queue)
    return;
  else
    inited = true;

  last_done.resize(n);

  lwm.group_relay_log_name = (char *)my_malloc(
      key_memory_Replica_job_group_group_relay_log_name, FN_REFLEN + 1, MYF(0));
  lwm.group_relay_log_name[0] = 0;
  lwm.sequence_number = SEQ_UNINIT;
}

#ifndef NDEBUG
bool Slave_committed_queue::count_done(Relay_log_info *rli) {
  size_t cnt = 0;

  for (auto i = entry; i < avail; ++i) {
    Slave_job_group *ptr_g;

    ptr_g = &m_Q[i % capacity];

    if (ptr_g->worker_id != MTS_WORKER_UNDEF && ptr_g->done) cnt++;
  }

  assert(cnt <= capacity);
  assert(cnt <= get_length());

  DBUG_PRINT("mta",
             ("Checking if it can simulate a crash:"
              " mta_checkpoint_group %u counter %zu parallel replicas %lu\n",
              opt_mta_checkpoint_group, cnt, rli->replica_parallel_workers));

  return (cnt == (rli->replica_parallel_workers * opt_mta_checkpoint_group));
}
#endif

/**
   The queue is processed from the head item by item
   to purge items representing committed groups.
   Progress in GAQ is assessed through comparison of GAQ index value
   with Worker's @c last_group_done_index.
   Purging breaks at a first discovered gap, that is an item
   that the assigned item->w_id'th Worker has not yet completed.

   The caller is supposed to be the checkpoint handler.

   A copy of the last discarded item containing
   the refreshed value of the committed low-water-mark is stored
   into @c lwm container member for further caller's processing.
   @c last_done is updated with the latest total_seqno for each Worker
   that was met during GAQ parse.

   @note dyn-allocated members of Slave_job_group such as
         group_relay_log_name as freed here.

   @return number of discarded items
*/
size_t Slave_committed_queue::move_queue_head(Slave_worker_array *ws) {
  DBUG_TRACE;
  size_t cnt = 0;
  assert(entry <= avail);
  while (!empty()) {
    Slave_worker *w_i;
    Slave_job_group *ptr_g;
    char grl_name[FN_REFLEN];

#ifndef NDEBUG
    if (DBUG_EVALUATE_IF("check_replica_debug_group", 1, 0) &&
        cnt == opt_mta_checkpoint_period)
      return cnt;
#endif

    grl_name[0] = 0;
    ptr_g = &m_Q[entry];

    /*
      The current job has not been processed or it was not
      even assigned, this means there is a gap.
    */
    if (ptr_g->worker_id == MTS_WORKER_UNDEF || ptr_g->done.load() == 0)
      break; /* gap at entry'th */

    /* Worker-id domain guard */
    static_assert(MTS_WORKER_UNDEF > MTS_MAX_WORKERS, "");

    w_i = ws->at(ptr_g->worker_id);

    /*
      Memorizes the latest valid group_relay_log_name.
    */
    if (ptr_g->group_relay_log_name) {
      strcpy(grl_name, ptr_g->group_relay_log_name);
      my_free(ptr_g->group_relay_log_name);
      /*
        It is important to mark the field as freed.
      */
      ptr_g->group_relay_log_name = nullptr;
    }

    /*
      Removes the job from the (G)lobal (A)ssigned (Q)ueue.
    */
    Slave_job_group g = Slave_job_group();
    (void)de_queue(&g);

    /*
      Stores the memorized name into the result struct. Note that we
      take care of the pointer first and then copy the other elements
      by assigning the structures.
    */
    if (grl_name[0] != 0) {
      strcpy(lwm.group_relay_log_name, grl_name);
    }
    g.group_relay_log_name = lwm.group_relay_log_name;
    lwm = g;

    assert(!ptr_g->group_relay_log_name);
    assert(ptr_g->total_seqno == lwm.total_seqno);
#ifndef NDEBUG
    {
      ulonglong l = last_done[w_i->id];
      /*
        There must be some progress otherwise we should have
        exit the loop earlier.
      */
      assert(l < ptr_g->total_seqno);
    }
#endif
    /*
      This is used to calculate the last time each worker has
      processed events.
    */
    last_done[w_i->id] = ptr_g->total_seqno;
    cnt++;
  }

  assert(cnt <= capacity);

  return cnt;
}

/**
   Finds low-water mark of committed jobs in GAQ.
   That is an index below which all jobs are marked as done.

   Notice the first available index is returned when the queue
   does not have any incomplete jobs. That includes cases of
   the empty and the full of complete jobs queue.
   A mutex protecting from concurrent LWM change by
   move_queue_head() (by Coordinator) should be taken by the caller.

   @param [out] arg_g  a double pointer to Slave job descriptor item
                       last marked with done-as-true boolean.
   @param start_index  a GAQ index to start/resume searching.
                       Caller is to make sure the index points into
                       assigned (occupied) range of circular buffer of GAQ.
   @return             GAQ index of the last consecutive done job, or the GAQ
                       size when none is found.
*/
size_t Slave_committed_queue::find_lwm(Slave_job_group **arg_g,
                                       size_t start_index) {
  Slave_job_group *ptr_g = nullptr;

  assert(in(start_index));

  /*
    Loop continuation condition relies on
    (TODO: assert it)
    the start_index being in the running range:

       start_index \in [entry, avail - 1].

    It satisfies any queue size including 1.
    It does not satisfy the empty queue case which is bailed out earlier above.
  */
  size_t i;
  for (i = start_index; i < avail; i++) {
    ptr_g = &m_Q[i % capacity];
    if (ptr_g->done.load() == 0) {
      break;
    }
  }
  if (i == start_index) {
    return capacity;  // the first node of the queue is not done}
  }
  ptr_g = &m_Q[(i - 1) % capacity];
  *arg_g = ptr_g;

  return (i - 1) % capacity;
}

/**
   Method should be executed at slave system stop to
   cleanup dynamically allocated items that remained as unprocessed
   by Coordinator and Workers in their regular execution course.
*/
void Slave_committed_queue::free_dynamic_items() {
  for (size_t i = entry; i < avail; i++) {
    Slave_job_group *ptr_g = &m_Q[i % capacity];
    if (ptr_g->group_relay_log_name) {
      my_free(ptr_g->group_relay_log_name);
    }
    if (ptr_g->checkpoint_log_name) {
      my_free(ptr_g->checkpoint_log_name);
    }
    if (ptr_g->checkpoint_relay_log_name) {
      my_free(ptr_g->checkpoint_relay_log_name);
    }
    if (ptr_g->group_master_log_name) {
      my_free(ptr_g->group_master_log_name);
    }
  }
}

void Slave_worker::do_report(loglevel level, int err_code, const char *msg,
                             va_list args) const {
  const Gtid_specification *gtid_next = &info_thd->variables.gtid_next;
  do_report(level, err_code, gtid_next, msg, args);
}

void Slave_worker::do_report(loglevel level, int err_code,
                             const Gtid_specification *gtid_next,
                             const char *msg, va_list args) const {
  char buff_coord[MAX_SLAVE_ERRMSG];
  char buff_gtid[Gtid::MAX_TEXT_LENGTH + 1];
  const char *log_name =
      const_cast<Slave_worker *>(this)->get_master_log_name();
  ulonglong log_pos = const_cast<Slave_worker *>(this)->get_master_log_pos();
  bool is_group_replication_applier_channel =
      channel_map.is_group_replication_applier_channel_name(
          c_rli->get_channel());
  THD *thd = info_thd;

  gtid_next->to_string(global_tsid_map, buff_gtid, true);

  if (level == ERROR_LEVEL && (!has_temporary_error(thd, err_code) ||
                               thd->get_transaction()->cannot_safely_rollback(
                                   Transaction_ctx::SESSION))) {
    char coordinator_errmsg[MAX_SLAVE_ERRMSG];

    if (is_group_replication_applier_channel) {
      snprintf(coordinator_errmsg, MAX_SLAVE_ERRMSG,
               "Coordinator stopped because there were error(s) in the "
               "worker(s). "
               "The most recent failure being: Worker %u failed executing "
               "transaction '%s'. See error log and/or "
               "performance_schema.replication_applier_status_by_worker "
               "table for "
               "more details about this failure or others, if any.",
               internal_id, buff_gtid);
    } else {
      snprintf(coordinator_errmsg, MAX_SLAVE_ERRMSG,
               "Coordinator stopped because there were error(s) in the "
               "worker(s). "
               "The most recent failure being: Worker %u failed executing "
               "transaction '%s' at source log %s, end_log_pos %llu. "
               "See error log and/or "
               "performance_schema.replication_applier_status_by_worker "
               "table for "
               "more details about this failure or others, if any.",
               internal_id, buff_gtid, log_name, log_pos);
    }

    /*
      We want to update the errors in coordinator as well as worker.
      The fill_coord_err_buf() function update the error number, message and
      timestamp fields. This function is different from va_report() as
      va_report() also logs the error message in the log apart from updating the
      error fields. So, the worker does the job of reporting the error in the
      log. We just make coordinator aware of the error.
    */
    c_rli->fill_coord_err_buf(level, err_code, coordinator_errmsg);
  }

  if (is_group_replication_applier_channel) {
    snprintf(buff_coord, sizeof(buff_coord),
             "Worker %u failed executing transaction '%s'", internal_id,
             buff_gtid);
  } else {
    snprintf(buff_coord, sizeof(buff_coord),
             "Worker %u failed executing transaction '%s' at "
             "source log %s, end_log_pos %llu",
             internal_id, buff_gtid, log_name, log_pos);
  }

  /*
    Error reporting by the worker. The worker updates its error fields as well
    as reports the error in the log.
  */
  this->va_report(level, err_code, buff_coord, msg, args);
}

#ifndef NDEBUG
static bool may_have_timestamp(Log_event *ev) {
  bool res = false;

  switch (ev->get_type_code()) {
    case mysql::binlog::event::QUERY_EVENT:
    case mysql::binlog::event::GTID_LOG_EVENT:
    case mysql::binlog::event::GTID_TAGGED_LOG_EVENT:
      res = true;
      break;

    default:
      break;
  }

  return res;
}

static int64 get_last_committed(Log_event *ev) {
  int64 res = SEQ_UNINIT;

  switch (ev->get_type_code()) {
    case mysql::binlog::event::GTID_LOG_EVENT:
    case mysql::binlog::event::GTID_TAGGED_LOG_EVENT:
      res = static_cast<Gtid_log_event *>(ev)->last_committed;
      break;

    default:
      break;
  }

  return res;
}

static int64 get_sequence_number(Log_event *ev) {
  int64 res = SEQ_UNINIT;

  switch (ev->get_type_code()) {
    case mysql::binlog::event::GTID_LOG_EVENT:
    case mysql::binlog::event::GTID_TAGGED_LOG_EVENT:
      res = static_cast<Gtid_log_event *>(ev)->sequence_number;
      break;

    default:
      break;
  }

  return res;
}
#endif

/**
  MTS worker main routine.
  The worker thread loops in waiting for an event, executing it and
  fixing statistics counters.

  @return 0 success
         -1 got killed or an error happened during applying
*/
int Slave_worker::slave_worker_exec_event(Log_event *ev) {
  Relay_log_info *rli = c_rli;
  THD *thd = info_thd;
  int ret = 0;

  DBUG_TRACE;

  thd->server_id = ev->server_id;
  thd->unmasked_server_id = ev->common_header->unmasked_server_id;
  thd->set_time();
  thd->lex->set_current_query_block(nullptr);
  if (!ev->common_header->when.tv_sec)
    ev->common_header->when.tv_sec = static_cast<long>(time(nullptr));
  ev->thd = thd;  // todo: assert because up to this point, ev->thd == 0
  ev->worker = this;

#ifndef NDEBUG
  if (!is_mts_db_partitioned(rli) && may_have_timestamp(ev) &&
      !curr_group_seen_sequence_number) {
    curr_group_seen_sequence_number = true;

    longlong lwm_estimate =
        static_cast<Mts_submode_logical_clock *>(rli->current_mts_submode)
            ->estimate_lwm_timestamp();
    int64 last_committed = get_last_committed(ev);
    int64 sequence_number = get_sequence_number(ev);
    /*
      The commit timestamp waiting condition:

        lwm_estimate < last_committed  <=>  last_committed  \not <= lwm_estimate

      must have been satisfied by Coordinator.
      The first scheduled transaction does not have to wait for anybody.
    */
    assert(rli->gaq->entry == ev->mts_group_idx ||
           Mts_submode_logical_clock::clock_leq(last_committed, lwm_estimate));
    assert(lwm_estimate != SEQ_UNINIT || rli->gaq->entry == ev->mts_group_idx);
    /*
      The current transaction's timestamp can't be less that lwm.
    */
    assert(sequence_number == SEQ_UNINIT ||
           !Mts_submode_logical_clock::clock_leq(
               sequence_number, static_cast<Mts_submode_logical_clock *>(
                                    rli->current_mts_submode)
                                    ->estimate_lwm_timestamp()));
  }
#endif

  // Address partitioning only in database mode
  if (!is_any_gtid_event(ev) && is_mts_db_partitioned(rli)) {
    if (ev->contains_partition_info(end_group_sets_max_dbs)) {
      uint num_dbs = ev->mts_number_dbs();

      if (num_dbs == OVER_MAX_DBS_IN_EVENT_MTS) num_dbs = 1;

      assert(num_dbs > 0);

      for (uint k = 0; k < num_dbs; k++) {
        bool found = false;

        for (size_t i = 0; i < curr_group_exec_parts.size() && !found; i++) {
          found = curr_group_exec_parts[i] == ev->mts_assigned_partitions[k];
        }
        if (!found) {
          /*
            notice, can't assert
            assert(ev->mts_assigned_partitions[k]->worker == worker);
            since entry could be marked as wanted by other worker.
          */
          curr_group_exec_parts.push_back(ev->mts_assigned_partitions[k]);
        }
      }
      end_group_sets_max_dbs = false;
    }
  }

  set_future_event_relay_log_pos(ev->future_event_relay_log_pos);
  set_master_log_pos(static_cast<ulong>(ev->common_header->log_pos));
  set_gaq_index(ev->mts_group_idx);
  ret = ev->do_apply_event_worker(this);
  return ret;
}

/**
  Sleep for a given amount of seconds or until killed.

  @param seconds    The number of seconds to sleep.

  @retval True if the thread has been killed, false otherwise.
*/

bool Slave_worker::worker_sleep(ulong seconds) {
  bool ret = false;
  struct timespec abstime;
  mysql_mutex_t *lock = &jobs_lock;
  mysql_cond_t *cond = &jobs_cond;

  /* Absolute system time at which the sleep time expires. */
  set_timespec(&abstime, seconds);

  mysql_mutex_lock(lock);
  info_thd->ENTER_COND(cond, lock, nullptr, nullptr);

  while (!(ret = info_thd->killed || running_status != RUNNING)) {
    int error = mysql_cond_timedwait(cond, lock, &abstime);
    if (is_timeout(error)) break;
  }

  mysql_mutex_unlock(lock);
  info_thd->EXIT_COND(nullptr);
  return ret;
}

void Slave_worker::reset_commit_order_deadlock() {
  m_commit_order_deadlock.store(false);
}

bool Slave_worker::found_commit_order_deadlock() {
  return m_commit_order_deadlock.load();
}

void Slave_worker::report_commit_order_deadlock() {
  DBUG_TRACE;
  assert(get_commit_order_manager() != nullptr);
  m_commit_order_deadlock.store(true);
}

void Slave_worker::prepare_for_retry(Log_event &event) {
  if (event.get_type_code() ==
      mysql::binlog::event::
          ROWS_QUERY_LOG_EVENT) {  // If a `Rows_query_log_event`, let
                                   // the event be disposed in the main
                                   // worker loop.
    event.worker = this;
    this->rows_query_ev = nullptr;
  }
}

std::tuple<bool, bool, uint> Slave_worker::check_and_report_end_of_retries(
    THD *thd) {
  DBUG_TRACE;

  bool silent = false;
  uint error = 0;

  if (found_commit_order_deadlock()) {
    /*
      This transaction was allowed to be executed in parallel with other that
      happened earlier according to binary log order. It was asked to be
      rolled back by the other transaction as it was holding a lock that is
      needed by the other transaction to progress, according to binary log
      order this configure a deadlock.

      At this point, this transaction *should* have no non-temporary errors.

      Having a non-temporary error may be a sign of:

      a) Slave has diverged from the master;
      b) There is an issue in the logical clock allowing a transaction to be
         applied in parallel with its dependencies (the two transactions are
         trying to change the same record in parallel).

      For (a), a retry of this transaction will produce the same error. For
      (b), this transaction might succeed upon retry, allowing the slave to
      progress without manual intervention, but it is a sign of problems in LC
      generation at the master.

      So, we will make the worker to retry this transaction only if there is
      no error or the error is a temporary error.
    */
    Diagnostics_area *da = thd->get_stmt_da();
    if (!da->is_error() ||
        has_temporary_error(thd, da->is_error() ? da->mysql_errno() : 0,
                            &silent)) {
      error = ER_LOCK_DEADLOCK;
    }
  }

  if (!has_temporary_error(thd, error, &silent) ||
      thd->get_transaction()->cannot_safely_rollback(Transaction_ctx::SESSION))
    return std::make_tuple(true, silent, error);

  if (trans_retries >= slave_trans_retries) {
    thd->fatal_error();
    c_rli->report(ERROR_LEVEL, thd->get_stmt_da()->mysql_errno(),
                  "worker thread retried transaction %lu time(s) "
                  "in vain, giving up. Consider raising the value of "
                  "the replica_transaction_retries variable.",
                  trans_retries);
    return std::make_tuple(true, silent, error);
  }

  return std::make_tuple(false, silent, error);
}

bool Slave_worker::retry_transaction(my_off_t start_relay_pos,
                                     const char *start_event_relay_log_name,
                                     my_off_t end_relay_pos,
                                     const char *end_event_relay_log_name) {
  THD *thd = info_thd;
  bool silent = false;

  DBUG_TRACE;

  /* Flag to check for cleanup */
  bool cleaned_up{false};

  /* Resets the worker context for next transaction retry, if any */
  auto clean_retry_context = [&cleaned_up, &thd, this]() -> void {
    if (!cleaned_up) {
      cleanup_context(thd, true);
      reset_commit_order_deadlock();
      cleaned_up = true;
    }
  };

  /* Object of sentry class to perform cleanup */
  raii::Sentry<> retry_context_guard{clean_retry_context};

  if (slave_trans_retries == 0) return true;

  do {
    /* Simulate a lock deadlock error */
    uint error = 0;
    bool ret;
    cleaned_up = false;

    std::tie(ret, silent, error) = check_and_report_end_of_retries(thd);
    DBUG_EXECUTE_IF("error_on_rows_query_event_apply", { ret = false; };);
    if (ret) return true;

    DBUG_EXECUTE_IF("error_on_rows_query_event_apply", {
      if (c_rli->retried_trans == 2) {
        DBUG_SET("-d,error_on_rows_query_event_apply");
        std::string act{"now SIGNAL end_retries_on_rows_query_event_apply"};
        assert(!debug_sync_set_action(thd, act.data(), act.length()));
      }
      silent = true;
    };);

    if (!silent) {
      trans_retries++;
      if (current_thd->rli_slave->is_processing_trx()) {
        // if the error code is zero, we get the top of the error stack
        uint transient_error =
            (error == 0) ? thd->get_stmt_da()->mysql_errno() : error;
        current_thd->rli_slave->retried_processing(
            transient_error, ER_THD_NONCONST(thd, transient_error),
            trans_retries);
#ifndef NDEBUG
        if (trans_retries == 2 || trans_retries == 6)
          DBUG_EXECUTE_IF("rpl_ps_tables_worker_retry", {
            char const act[] =
                "now SIGNAL signal.rpl_ps_tables_worker_retry_pause "
                "WAIT_FOR signal.rpl_ps_tables_worker_retry_continue";
            assert(opt_debug_sync_timeout > 0);
            // we can't add the usual assert here because thd->is_error()
            // is true (and that's OK)
            debug_sync_set_action(thd, STRING_WITH_LEN(act));
          });
#endif
      }
    }

    mysql_mutex_lock(&c_rli->data_lock);
    c_rli->retried_trans++;
    mysql_mutex_unlock(&c_rli->data_lock);

    clean_retry_context();
    worker_sleep(min<ulong>(trans_retries, MAX_SLAVE_RETRY_PAUSE));

  } while (read_and_apply_events(start_relay_pos, start_event_relay_log_name,
                                 end_relay_pos, end_event_relay_log_name));
  return false;
}

/**
  Read events from relay logs and apply them.

  @param[in] start_relay_pos The offset of the transaction's first event.
  @param[in] start_event_relay_log_name The name of the relay log which
               includes the first event of the transaction.

  @param[in] end_relay_pos The offset of the last event it should retry.
  @param[in] end_event_relay_log_name The name of the relay log which
               includes the last event it should retry.

  @return false if succeeds, otherwise returns true.
*/
bool Slave_worker::read_and_apply_events(my_off_t start_relay_pos,
                                         const char *start_event_relay_log_name,
                                         my_off_t end_relay_pos,
                                         const char *end_event_relay_log_name) {
  DBUG_TRACE;

  Relay_log_info *rli = c_rli;
  char file_name[FN_REFLEN + 1];
  bool arrive_end = false;
  Relaylog_file_reader relaylog_file_reader(opt_replica_sql_verify_checksum);

  strcpy(file_name, start_event_relay_log_name);

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *thread = thd_get_psi(rli->info_thd);
  Scope_guard guard([&] {
    if (thread != nullptr) PSI_THREAD_CALL(abort_telemetry)(thread);
  });
#endif

  while (!arrive_end) {
    Log_event *ev = nullptr;

    if (!relaylog_file_reader.is_open()) {
      DBUG_PRINT("info", ("Open relay log %s", file_name));

      if (relaylog_file_reader.open(file_name, start_relay_pos)) {
        LogErr(ERROR_LEVEL, ER_RPL_FAILED_TO_OPEN_RELAY_LOG, file_name,
               relaylog_file_reader.get_error_str());
        return true;
      }
    }

    /* If it is the last event, then set arrive_end as true */
    arrive_end = (relaylog_file_reader.position() == end_relay_pos &&
                  !(strcmp(file_name, end_event_relay_log_name)));

    ev = relaylog_file_reader.read_event_object();
    if (ev != nullptr) {
      /* It is a event belongs to the transaction */
      if (!ev->is_mts_sequential_exec()) {
        RLI_current_event_raii current_event_guard{this, ev};

        ev->future_event_relay_log_pos = relaylog_file_reader.position();
        ev->mts_group_idx = gaq_index;

        // event was re-read again, thence context was lost, attach
        // additional context needed, before re-executing (just like in
        // the main loop before exec_relay_log_event)
        if (rli->current_mts_submode->set_multi_threaded_applier_context(*rli,
                                                                         *ev)) {
          return true;
        }

        // we re-assign partitions only on retries
        if (is_mts_db_partitioned(rli) && ev->contains_partition_info(true))
          assign_partition_db(ev);

        int ret = slave_worker_exec_event(ev);

        if (ev->get_type_code() !=
            mysql::binlog::event::Log_event_type::TRANSACTION_PAYLOAD_EVENT) {
          increment_worker_metrics_for_event(*ev);
        }

        if (ev->worker != nullptr) {
          delete ev;
          ev = nullptr;
        }
        if (ret != 0) return true;
      } else {
        /*
          It is a Rotate_log_event, Format_description_log_event event or other
          type event doesn't belong to the transaction.
        */
        delete ev;
        ev = nullptr;
      }
    } else {
      /*
        IO error happens if error_type is not READ_EOF, otherwise it arrives the
        end of the relay log
      */
      if (relaylog_file_reader.get_error_type() !=
          Binlog_read_error::READ_EOF) {
        LogErr(ERROR_LEVEL, ER_RPL_WORKER_CANT_READ_RELAY_LOG,
               rli->get_event_relay_log_name(),
               relaylog_file_reader.position());
        return true;
      }

      if (rli->relay_log.find_next_relay_log(file_name)) {
        LogErr(ERROR_LEVEL, ER_RPL_WORKER_CANT_FIND_NEXT_RELAY_LOG, file_name);
        return true;
      }

      relaylog_file_reader.close();
      start_relay_pos = BIN_LOG_HEADER_SIZE;
    }
  }

  return false;
}

/*
  Find database entry from map_db_to_worker hash table.
 */
static db_worker_hash_entry *find_entry_from_db_map(const char *dbname,
                                                    Relay_log_info *rli) {
  db_worker_hash_entry *entry = nullptr;

  mysql_mutex_lock(&rli->slave_worker_hash_lock);
  entry = find_or_nullptr(rli->mapping_db_to_worker, dbname);
  mysql_mutex_unlock(&rli->slave_worker_hash_lock);
  return entry;
}

/*
  Initialize Log_event::mts_assigned_partitions array. It is for transaction
  retry and is only called when retrying a transaction by workers.
*/
void Slave_worker::assign_partition_db(Log_event *ev) {
  Mts_db_names mts_dbs;
  int i;

  ev->get_mts_dbs(&mts_dbs, c_rli->rpl_filter);

  if (mts_dbs.num == OVER_MAX_DBS_IN_EVENT_MTS)
    ev->mts_assigned_partitions[0] = find_entry_from_db_map("", c_rli);
  else
    for (i = 0; i < mts_dbs.num; i++)
      ev->mts_assigned_partitions[i] =
          find_entry_from_db_map(mts_dbs.name[i], c_rli);
}

/**
   Coordinator enqueues a job item into a Worker private queue.

   @param job_item  a pointer to struct carrying a reference to an event
   @param worker    a pointer to the assigned Worker struct
   @param rli       a pointer to Relay_log_info of Coordinator

   @return false Success.
           true  Thread killed or worker stopped while waiting for
                 successful enqueue.
*/
bool append_item_to_jobs(slave_job_item *job_item, Slave_worker *worker,
                         Relay_log_info *rli) {
  THD *thd = rli->info_thd;
  size_t ret = Slave_jobs_queue::error_result;
  size_t ev_size = job_item->data->common_header->data_written;
  ulonglong new_pend_size;
  PSI_stage_info old_stage;
  auto &applier_metrics{rli->get_applier_metrics()};

  assert(thd == current_thd);

  mysql_mutex_lock(&rli->pending_jobs_lock);
  new_pend_size = rli->mts_pending_jobs_size + ev_size;
  bool big_event = (ev_size > rli->mts_pending_jobs_size_max);
  Slave_job_group *ptr_g =
      rli->gaq->get_job_group(rli->gaq->assigned_group_index);
  /*
    C waits basing on *data* sizes in the queues.
    If it is a big event (event size is greater than
    replica_pending_jobs_size_max but less than replica_max_allowed_packet),
    it will wait for all the jobs in the workers's queue to be
    completed. If it is normal event (event size is less than
    replica_pending_jobs_size_max), then it will wait for
    enough empty memory to keep the event in one of the workers's
    queue.
    NOTE: Receiver thread (I/O thread) is taking care of restricting
    the event size to replica_max_allowed_packet. If an event from
    the master is bigger than this value, IO thread will be stopped
    with error ER_NET_PACKET_TOO_LARGE.
  */
  while ((!big_event && new_pend_size > rli->mts_pending_jobs_size_max) ||
         (big_event && rli->mts_pending_jobs_size != 0)) {
    rli->mts_wq_oversize = true;
    // waiting due to the total size
    rli->worker_queue_mem_exceeded_count++;
    {
      auto guard =
          applier_metrics.get_worker_queues_memory_exceeds_max_wait_metric()
              .time_scope();
      thd->ENTER_COND(&rli->pending_jobs_cond, &rli->pending_jobs_lock,
                      &stage_replica_waiting_worker_to_free_events, &old_stage);
      mysql_cond_wait(&rli->pending_jobs_cond, &rli->pending_jobs_lock);
      mysql_mutex_unlock(&rli->pending_jobs_lock);
      thd->EXIT_COND(&old_stage);
    }

    if (thd->killed) return true;
    if (rli->worker_queue_mem_exceeded_count % 10 == 1) {
      time_t my_now = time(nullptr);
      if ((my_now - rli->mta_coordinator_has_waited_stat) >=
          mts_online_stat_period) {
        LogErr(INFORMATION_LEVEL, ER_RPL_MTA_REPLICA_COORDINATOR_HAS_WAITED,
               rli->worker_queue_mem_exceeded_count, ev_size);
        rli->mta_coordinator_has_waited_stat = my_now;
      }
    }
    mysql_mutex_lock(&rli->pending_jobs_lock);

    new_pend_size = rli->mts_pending_jobs_size + ev_size;
  }
  rli->pending_jobs++;
  rli->mts_pending_jobs_size = new_pend_size;

  mysql_mutex_unlock(&rli->pending_jobs_lock);

  /*
    Sleep unless there is an underrunning Worker and the current Worker
    queue is empty or filled lightly (not more than underrun level).
  */
  if (rli->mts_wq_underrun_w_id == MTS_WORKER_UNDEF &&
      worker->jobs.get_length() > worker->underrun_level) {
    /*
      todo: experiment with weight to get a good approximation formula.
      Max possible nap time is chosen 1 ms.
      The bigger the excessive overrun counter the longer the nap.
    */
    ulong nap_weight = rli->mts_wq_excess_cnt + 1;
    /*
       Nap time is a product of a weight factor and the basic nap unit.
       The weight factor is proportional to the worker queues overrun excess
       counter. For example when there were only one overrunning Worker
       the max nap_weight as 0.1 * worker->jobs.size would be
       about 1600 so the max nap time is approx 0.008 secs.
       Such value is not reachable because of min().
       Notice, granularity of sleep depends on the resolution of the software
       clock, High-Resolution Timer (HRT) configuration. Without HRT
       the precision of wake-up through @c select() may be greater or
       equal 1 ms. So don't expect the nap last a prescribed fraction of 1 ms
       in such case.
    */
    my_sleep(min<ulong>(1000, nap_weight * rli->mts_coordinator_basic_nap));
  }

  // unclaim ownership of the event log memory
  job_item->data->claim_memory_ownership(/*claim=*/false);
  if (worker->checkpoint_notified) {
    my_claim(ptr_g->checkpoint_log_name, /*claim=*/false);
    my_claim(ptr_g->checkpoint_relay_log_name, /*claim=*/false);
  }
  if (worker->master_log_change_notified) {
    my_claim(ptr_g->group_master_log_name, /*claim=*/false);
  }

  mysql_mutex_lock(&worker->jobs_lock);

  // possible WQ overfill
  while (worker->running_status == Slave_worker::RUNNING && !thd->killed &&
         (ret = worker->jobs.en_queue(job_item)) ==
             Slave_jobs_queue::error_result) {
    auto guard =
        applier_metrics.get_worker_queues_full_wait_metric().time_scope();
    thd->ENTER_COND(&worker->jobs_cond, &worker->jobs_lock,
                    &stage_replica_waiting_worker_queue, &old_stage);
    worker->jobs.overfill = true;
    worker->jobs.waited_overfill++;
    mysql_cond_wait(&worker->jobs_cond, &worker->jobs_lock);
    mysql_mutex_unlock(&worker->jobs_lock);
    thd->EXIT_COND(&old_stage);
    mysql_mutex_lock(&worker->jobs_lock);
  }
  if (ret != Slave_jobs_queue::error_result) {
    worker->curr_jobs++;
    if (worker->jobs.get_length() == 1) mysql_cond_signal(&worker->jobs_cond);

    mysql_mutex_unlock(&worker->jobs_lock);
  } else {
    mysql_mutex_unlock(&worker->jobs_lock);

    mysql_mutex_lock(&rli->pending_jobs_lock);
    rli->pending_jobs--;  // roll back of the prev incr
    rli->mts_pending_jobs_size -= ev_size;
    mysql_mutex_unlock(&rli->pending_jobs_lock);

    // claim back ownership of the event log memory
    job_item->data->claim_memory_ownership(/*claim=*/true);
    if (worker->checkpoint_notified) {
      my_claim(ptr_g->checkpoint_log_name, /*claim=*/true);
      my_claim(ptr_g->checkpoint_relay_log_name, /*claim=*/true);
    }
    if (worker->master_log_change_notified) {
      my_claim(ptr_g->group_master_log_name, /*claim=*/true);
    }
  }

  return (Slave_jobs_queue::error_result != ret) ? false : true;
}

/**
  Remove a job item from the given workers job queue. It also updates related
  status.

  param[in] job_item The job item will be removed
  param[in] worker   The worker which job_item belongs to.
  param[in] rli      slave's relay log info object.
 */
static void remove_item_from_jobs(slave_job_item *job_item,
                                  Slave_worker *worker, Relay_log_info *rli) {
  Log_event *ev = job_item->data;

  mysql_mutex_lock(&worker->jobs_lock);
  worker->jobs.de_queue(job_item);
  /* possible overfill */
  if (worker->jobs.get_length() == worker->jobs.capacity - 1 &&
      worker->jobs.overfill == true) {
    worker->jobs.overfill = false;
    // todo: worker->hungry_cnt++;
    mysql_cond_signal(&worker->jobs_cond);
  }
  mysql_mutex_unlock(&worker->jobs_lock);

  /* statistics */

  const auto jobs_length = worker->jobs.get_length();
  /* todo: convert to rwlock/atomic write */
  mysql_mutex_lock(&rli->pending_jobs_lock);

  rli->pending_jobs--;
  rli->mts_pending_jobs_size -= ev->common_header->data_written;
  assert(rli->mts_pending_jobs_size < rli->mts_pending_jobs_size_max);

  /*
    The positive branch is underrun: number of pending assignments
    is less than underrun level.
    Zero of jobs.len has to reset underrun w_id as the worker may get
    the next piece of assignment in a long time.
  */
  if (worker->underrun_level > jobs_length && jobs_length != 0) {
    rli->mts_wq_underrun_w_id = worker->id;
  } else if (rli->mts_wq_underrun_w_id == worker->id) {
    // reset only own marking
    rli->mts_wq_underrun_w_id = MTS_WORKER_UNDEF;
  }

  /*
    Overrun handling.
    Incrementing the Worker private and the total excess counter corresponding
    to number of events filled above the overrun_level.
    The increment amount to the total counter is a difference between
    the current and the previous private excess (worker->wq_overrun_cnt).
    When the current queue length drops below overrun_level the global
    counter is decremented, the local is reset.
  */
  if (worker->overrun_level < jobs_length) {
    ulong last_overrun = worker->wq_overrun_cnt;
    ulong excess_delta;

    /* current overrun */
    worker->wq_overrun_cnt = jobs_length - worker->overrun_level;
    excess_delta = worker->wq_overrun_cnt - last_overrun;
    worker->excess_cnt += excess_delta;
    rli->mts_wq_excess_cnt += excess_delta;

    // guarding correctness of incrementing in case of the only one Worker
    assert(rli->workers.size() != 1 ||
           rli->mts_wq_excess_cnt == worker->wq_overrun_cnt);
  } else if (worker->excess_cnt > 0) {
    // When level drops below the total excess is decremented by the
    // value of the worker's contribution to the total excess.
    rli->mts_wq_excess_cnt -= worker->excess_cnt;
    worker->excess_cnt = 0;
    worker->wq_overrun_cnt = 0;  // and the local is reset

    assert(rli->mts_wq_excess_cnt >= 0);
    assert(rli->mts_wq_excess_cnt == 0 || rli->workers.size() > 1);
  }

  /* coordinator can be waiting */
  if (rli->mts_pending_jobs_size < rli->mts_pending_jobs_size_max &&
      rli->mts_wq_oversize)  // TODO: unit/general test wq_oversize
  {
    rli->mts_wq_oversize = false;
    mysql_cond_signal(&rli->pending_jobs_cond);
  }

  mysql_mutex_unlock(&rli->pending_jobs_lock);
}
/**
   Worker's routine to wait for a new assignment through
   @c append_item_to_jobs()

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return NULL failure or
           a-pointer to an item.
*/
static struct slave_job_item *pop_jobs_item(Slave_worker *worker,
                                            Slave_job_item *job_item) {
  THD *thd = worker->info_thd;

  mysql_mutex_lock(&worker->jobs_lock);

  job_item->data = nullptr;
  while (!job_item->data && !thd->killed &&
         (worker->running_status == Slave_worker::RUNNING ||
          worker->running_status == Slave_worker::STOP)) {
    PSI_stage_info old_stage;

    if (set_max_updated_index_on_stop(worker, job_item)) break;
    if (job_item->data == nullptr) {
      thd->ENTER_COND(&worker->jobs_cond, &worker->jobs_lock,
                      &stage_replica_waiting_event_from_coordinator,
                      &old_stage);
      mysql_cond_wait(&worker->jobs_cond, &worker->jobs_lock);
      mysql_mutex_unlock(&worker->jobs_lock);
      thd->EXIT_COND(&old_stage);
      mysql_mutex_lock(&worker->jobs_lock);
    }
  }
  if (job_item->data) worker->curr_jobs--;

  mysql_mutex_unlock(&worker->jobs_lock);

  thd_proc_info(worker->info_thd, "Executing event");
  return job_item;
}

/**
  Report a not yet reported error to the coordinator if necessary.

  All issues detected when applying binary log events are reported using
  rli->report(), but when an issue is not reported by the log event being
  applied, there is a workaround at handle_slave_sql() to report the issue
  also using rli->report() for the STS applier (or the MTS coordinator).

  This function implements the workaround for a MTS worker.

  @param worker the worker to be evaluated.
*/
void report_error_to_coordinator(Slave_worker *worker) {
  THD *thd = worker->info_thd;
  /*
    It is possible that the worker had failed to apply the event but
    did not reported about the failure using rli->report(). An example
    of such cases are failures caused by setting GTID_NEXT variable with
    an unsupported GTID mode (GTID_SET when GTID_MODE = OFF, anonymous
    GTID when GTID_MODE = ON).
  */
  if (thd->is_error()) {
    char const *const errmsg = thd->get_stmt_da()->message_text();
    DBUG_PRINT("info", ("thd->get_stmt_da()->get_mysql_errno()=%d; "
                        "worker->last_error.number=%d",
                        thd->get_stmt_da()->mysql_errno(),
                        worker->last_error().number));

    if (worker->last_error().number == 0 &&
        /*
          When another worker that should commit before the current worker
          being evaluated has failed and the commit order should be preserved
          the current worker was asked to roll back and would stop with the
          ER_REPLICA_WORKER_STOPPED_PREVIOUS_THD_ERROR not yet reported to the
          coordinator. Reporting this error to the coordinator would be a
          mistake and would mask the real issue that lead to the MTS stop as
          the coordinator reports only the last error reported to it as the
          cause of the MTS failure.

          So, we should skip reporting the error if it was reported because
          the current transaction had to be rolled back by a failure in a
          previous transaction in the commit order while the current
          transaction was waiting to be committed.
        */
        thd->get_stmt_da()->mysql_errno() !=
            ER_REPLICA_WORKER_STOPPED_PREVIOUS_THD_ERROR) {
      /*
        This function is reporting an error which was not reported
        while executing exec_relay_log_event().
      */
      worker->report(ERROR_LEVEL, thd->get_stmt_da()->mysql_errno(), "%s",
                     errmsg);
    }
  }
}

/**
  apply one job group.

  @note the function maintains worker's CGEP and modifies APH, updates
        the current group item in GAQ via @c slave_worker_ends_group().

  param[in] worker the worker which calls it.
  param[in] rli    slave's relay log info object.

  return returns 0 if the group of jobs are applied successfully, otherwise
         returns an error code.
 */
int slave_worker_exec_job_group(Slave_worker *worker, Relay_log_info *rli) {
  struct slave_job_item item = {nullptr, 0, {'\0'}, false};
  struct slave_job_item *job_item = &item;
  THD *thd = worker->info_thd;
  bool seen_gtid = false;
  bool after_gtid = true;
  bool seen_begin = false;
  int error = 0;
  Log_event *ev = nullptr;
  my_off_t start_relay_pos;
  char start_event_relay_log_name[FN_REFLEN + 1];
  std::size_t transaction_size{0};
  std::size_t gtid_event_size{0};

  DBUG_TRACE;

  if (unlikely(worker->trans_retries > 0)) worker->trans_retries = 0;

  worker->get_worker_metrics().set_transaction_type(
      cs::apply::instruments::Worker_metrics::Transaction_type_info::UNKNOWN);
  job_item = pop_jobs_item(worker, job_item);
  start_relay_pos = job_item->relay_pos;
  strcpy(start_event_relay_log_name, job_item->event_relay_log_name);

  PSI_thread *thread = thd_get_psi(thd);

  auto &applier_metrics = rli->get_applier_metrics();
  auto &worker_metrics = worker->get_worker_metrics();

  /* Current event with Worker associator. */
  RLI_current_event_raii worker_curr_ev(worker, ev);

#ifdef HAVE_PSI_THREAD_INTERFACE
  /* Check telemetry session status. */
  if (thread != nullptr) {
    PSI_THREAD_CALL(detect_telemetry)(thread);
  }
#endif

  worker->set_group_master_log_name(rli->get_group_master_log_name());

  while (true) {
    Slave_job_group *ptr_g;

    if (unlikely(thd->killed ||
                 worker->running_status == Slave_worker::STOP_ACCEPTED)) {
      assert(worker->running_status != Slave_worker::ERROR_LEAVING);
      // de-queueing and decrement counters is in the caller's exit branch
      error = -1;
      goto err;
    }

    ev = job_item->data;
    assert(ev != nullptr);

    // claim ownership of the event log memory
    ev->claim_memory_ownership(/*claim=*/true);

    DBUG_PRINT("info", ("W_%lu <- job item: %p data: %p thd: %p", worker->id,
                        job_item, ev, thd));
    /*
      Associate the freshly read event with worker.
      The binding also remains when the loop breaks at the group end event
      so a DDL Query_log_event as such a breaker would remain pinned to
      the Worker by the slave info table update and commit time,
      see slave_worker_ends_group().
    */
    worker_curr_ev.set_current_event(ev);

    if (seen_gtid && after_gtid) {
      using cs::apply::instruments::Worker_metrics;
      if (ev->get_type_code() == mysql::binlog::event::QUERY_EVENT) {
        auto *query_event = dynamic_cast<Query_log_event const *>(ev);
        if (strncmp(query_event->query, STRING_WITH_LEN("BEGIN")) != 0 &&
            strncmp(query_event->query, STRING_WITH_LEN("XA START")) != 0) {
          worker_metrics.set_transaction_type(
              Worker_metrics::Transaction_type_info::DDL);
        } else {
          worker_metrics.set_transaction_type(
              Worker_metrics::Transaction_type_info::DML);
        }
      } else if (ev->get_type_code() ==
                 mysql::binlog::event::TRANSACTION_PAYLOAD_EVENT) {
        // Payload events are always DML. Although they contain a BEGIN event,
        // they do not visit the code path above that sets the transaction type
        // for BEGIN events. So we set the type here.
        worker_metrics.set_transaction_type(
            Worker_metrics::Transaction_type_info::DML);
      }
      after_gtid = false;
    }
    if (is_any_gtid_event(ev)) {
      seen_gtid = true;
      Gtid_log_event *gtid_ev = dynamic_cast<Gtid_log_event *>(ev);
      gtid_event_size = gtid_ev->common_header->data_written;
      transaction_size = gtid_ev->get_trx_length();
      worker_metrics.set_transaction_ongoing_full_size(transaction_size);
    }
    if (ev->get_type_code() ==
        mysql::binlog::event::Log_event_type::TRANSACTION_PAYLOAD_EVENT) {
      Transaction_payload_log_event *tp_ev =
          dynamic_cast<Transaction_payload_log_event *>(ev);
      std::size_t decompressed_transaction_size =
          gtid_event_size + tp_ev->get_uncompressed_size();
      worker_metrics.set_transaction_ongoing_full_size(
          decompressed_transaction_size);
      transaction_size = decompressed_transaction_size;
    }

    if (!seen_begin && ev->starts_group()) {
      seen_begin = true;  // The current group is started with B-event
      worker->end_group_sets_max_dbs = true;
    }
    /* Adapting to possible new Format_description_log_event */
    ptr_g = rli->gaq->get_job_group(ev->mts_group_idx);
    if (ptr_g->new_fd_event) {
      error = worker->set_rli_description_event(ptr_g->new_fd_event);
      if (unlikely(error)) goto err;
      ptr_g->new_fd_event = nullptr;
    }

    worker->set_group_source_log_start_end_pos(ev);

    error = worker->slave_worker_exec_event(ev);

    // Transaction payload events don't have a size, and the calculation for
    // them is done at Transaction_payload_log_event::apply_payload_event
    if (ev->get_type_code() !=
        mysql::binlog::event::Log_event_type::TRANSACTION_PAYLOAD_EVENT) {
      worker->increment_worker_metrics_for_event(*ev);
    }

    if (error || worker->found_commit_order_deadlock()) {
      worker->prepare_for_retry(*ev);

      // Reset the progress stats
      worker_metrics.reset_transaction_ongoing_progress_size();

      error = worker->retry_transaction(
          start_relay_pos, start_event_relay_log_name, job_item->relay_pos,
          job_item->event_relay_log_name);
      if (error) goto err;
    }
    /*
      p-event or any other event of B-free (malformed) group can
      "commit" with logical clock scheduler. In that case worker id
      points to the only active "exclusive" Worker that processes such
      malformed group events one by one.
      WL#7592 refines the original assert disjunction formula
      with the final disjunct.
    */
    assert(seen_begin || is_any_gtid_event(ev) ||
           ev->get_type_code() == mysql::binlog::event::QUERY_EVENT ||
           is_mts_db_partitioned(rli) || worker->id == 0 || seen_gtid);

    if (ev->ends_group() ||
        (!seen_begin && !is_any_gtid_event(ev) &&
         (ev->get_type_code() == mysql::binlog::event::QUERY_EVENT ||
          /* break through by LC only in GTID off */
          (!seen_gtid && !is_mts_db_partitioned(rli)))))
      break;

    remove_item_from_jobs(job_item, worker, rli);
    /* The event will be used later if worker is NULL, so it is not freed */
    if (ev->worker != nullptr) delete ev;

    job_item = pop_jobs_item(worker, job_item);
  }

  DBUG_PRINT("info", (" commits GAQ index %lu, last committed  %lu",
                      ev->mts_group_idx, worker->last_group_done_index));
  /* The group is applied successfully, so error should be 0 */
  worker->slave_worker_ends_group(ev, 0);

  /*
    Check if the finished group started with a Gtid_log_event to update the
    monitoring information
  */
  if (current_thd->rli_slave->is_processing_trx()) {
    DBUG_EXECUTE_IF("rpl_ps_tables", {
      const char act[] =
          "now SIGNAL signal.rpl_ps_tables_apply_before "
          "WAIT_FOR signal.rpl_ps_tables_apply_finish";
      assert(opt_debug_sync_timeout > 0);
      assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
    };);
    if (ev->get_type_code() == mysql::binlog::event::QUERY_EVENT &&
        ((Query_log_event *)ev)->rollback_injected_by_coord) {
      /*
        If this was a rollback event injected by the coordinator because of a
        partial transaction in the relay log, we must not consider this
        transaction completed and, instead, clear the monitoring info.
      */
      current_thd->rli_slave->clear_processing_trx();
    } else {
      current_thd->rli_slave->finished_processing();
    }
    DBUG_EXECUTE_IF("rpl_ps_tables", {
      const char act[] =
          "now SIGNAL signal.rpl_ps_tables_apply_after_finish "
          "WAIT_FOR signal.rpl_ps_tables_apply_continue";
      assert(opt_debug_sync_timeout > 0);
      assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
    };);
  }

#ifndef NDEBUG
  DBUG_PRINT("mta",
             ("Check_replica_debug_group worker %lu mta_checkpoint_group"
              " %u processed %llu debug %d\n",
              worker->id, opt_mta_checkpoint_group,
              static_cast<unsigned long long int>(worker->transactions_handled),
              DBUG_EVALUATE_IF("check_replica_debug_group", 1, 0)));

  if (DBUG_EVALUATE_IF("check_replica_debug_group", 1, 0) &&
      opt_mta_checkpoint_group == worker->transactions_handled) {
    DBUG_PRINT("mta", ("Putting worker %lu in busy wait.", worker->id));
    while (true) my_sleep(6000000);
  }
#endif

  {
    bool is_after_metrics_breakpoint = job_item->m_is_after_metrics_breakpoint;
    remove_item_from_jobs(job_item, worker, rli);
    delete ev;

    applier_metrics.inc_events_committed_count(
        worker->m_events_applied_in_transaction);
    applier_metrics.inc_transactions_committed_count(1);
    applier_metrics.inc_transactions_committed_size_sum(transaction_size);
    if (!is_after_metrics_breakpoint) {
      applier_metrics.inc_transactions_received_size_sum(transaction_size);
      applier_metrics.inc_transactions_received_count(1);
    }
    worker_metrics.set_transaction_ongoing_full_size(0);
    worker_metrics.reset_transaction_ongoing_progress_size();
    worker_metrics.set_transaction_type(
        cs::apply::instruments::Worker_metrics::Transaction_type_info::UNKNOWN);
  }

#ifdef HAVE_PSI_THREAD_INTERFACE
  if (thread != nullptr) {
    PSI_THREAD_CALL(abort_telemetry)(thread);
  }
#endif

  return 0;
err:
  if (error) {
    Commit_stage_manager::get_instance().finish_session_ticket(thd);

    report_error_to_coordinator(worker);
    DBUG_PRINT("info", ("Worker %lu is exiting: killed %i, error %i, "
                        "running_status %d",
                        worker->id, thd->killed.load(), thd->is_error(),
                        worker->running_status));
    worker->slave_worker_ends_group(ev, error); /* last done sets post exec */
    worker_metrics.reset_transaction_ongoing_progress_size();
    worker_metrics.set_transaction_type(
        cs::apply::instruments::Worker_metrics::Transaction_type_info::UNKNOWN);
  }
#ifdef HAVE_PSI_THREAD_INTERFACE
  if (thread != nullptr) {
    PSI_THREAD_CALL(abort_telemetry)(thread);
  }
#endif
  return error;
}

const char *Slave_worker::get_for_channel_str(bool upper_case) const {
  return c_rli->get_for_channel_str(upper_case);
}

const uint *Slave_worker::get_table_pk_field_indexes() {
  return info_slave_worker_table_pk_field_indexes;
}

uint Slave_worker::get_channel_field_index() { return LINE_FOR_CHANNEL; }

cs::apply::instruments::Worker_metrics &Slave_worker::get_worker_metrics() {
  if (m_is_worker_metric_collection_enabled) return m_worker_metrics;
  return m_disabled_worker_metrics;
}
