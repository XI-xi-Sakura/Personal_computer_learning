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

#include <time.h>
#include <atomic>

#include "lex_string.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "my_systime.h"
#include "mysql/binlog/event/control_events.h"
#include "mysql/components/services/bits/psi_stage_bits.h"
#include "mysql/components/services/log_builtins.h"  //LogErr
#include "mysql/psi/mysql_mutex.h"
#include "mysqld_error.h"
#include "prealloced_array.h"
#include "sql/binlog.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"  // DEBUG_SYNC
#include "sql/mdl.h"
#include "sql/mysqld.h"  // opt_bin_log
#include "sql/rpl_context.h"
#include "sql/rpl_gtid.h"
#include "sql/rpl_gtid_persist.h"  // gtid_table_persistor
#include "sql/sql_class.h"         // THD
#include "sql/sql_error.h"
#include "sql/system_variables.h"
#include "sql/thr_malloc.h"

class Table_ref;

PSI_memory_key key_memory_Gtid_state_group_commit_sidno;

using Tsid = mysql::gtid::Tsid;

int Gtid_state::clear(THD *thd) {
  DBUG_TRACE;
  int ret = 0;
  // the wrlock implies that no other thread can hold any of the mutexes
  tsid_lock->assert_some_wrlock();
  lost_gtids.clear();
  executed_gtids.clear();
  gtids_only_in_table.clear();
  previous_gtids_logged.clear();
  /* Reset gtid_executed table. */
  if ((ret = gtid_table_persistor->reset(thd)) == 1) {
    /*
      Gtid table is not ready to be used, so failed to
      open it. Ignore the error.
    */
    thd->clear_error();
    ret = 0;
  }
  next_free_gno_map.clear();
  return ret;
}

enum_return_status Gtid_state::acquire_ownership(THD *thd, const Gtid &gtid) {
  DBUG_TRACE;
  // caller must take both global_tsid_lock and lock on the SIDNO.
  global_tsid_lock->assert_some_lock();
  gtid_state->assert_sidno_lock_owner(gtid.sidno);
  assert(!executed_gtids.contains_gtid(gtid));
  DBUG_PRINT("info", ("gtid=%d:%" PRId64, gtid.sidno, gtid.gno));
  assert(thd->owned_gtid.sidno == 0);
  if (owned_gtids.add_gtid_owner(gtid, thd->thread_id()) != RETURN_STATUS_OK)
    goto err;
  if (thd->get_gtid_next_list() != nullptr) {
#ifdef HAVE_GTID_NEXT_LIST
    thd->owned_gtid_set._add_gtid(gtid);
    thd->owned_gtid.sidno = THD::OWNED_SIDNO_GTID_SET;
    thd->owned_sid.clear();
#else
    assert(0);
#endif
  } else {
    thd->owned_gtid = gtid;
    thd->owned_gtid.dbug_print(nullptr, "set owned_gtid in acquire_ownership");
    thd->owned_tsid = tsid_map->sidno_to_tsid(gtid.sidno);
    thd->rpl_thd_ctx.last_used_gtid_tracker_ctx().set_last_used_gtid(
        gtid, thd->owned_tsid);
  }
  RETURN_OK;
err:
  if (thd->get_gtid_next_list() != nullptr) {
#ifdef HAVE_GTID_NEXT_LIST
    Gtid_set::Gtid_iterator git(&thd->owned_gtid_set);
    Gtid g = git.get();
    while (g.sidno != 0) {
      owned_gtids.remove_gtid(g);
      g = git.get();
    }
#else
    assert(0);
#endif
  }
  thd->clear_owned_gtids();
  thd->owned_gtid.dbug_print(nullptr,
                             "set owned_gtid (clear) in acquire_ownership");
  RETURN_REPORTED_ERROR;
}

#ifdef HAVE_GTID_NEXT_LIST
void Gtid_state::lock_owned_sidnos(const THD *thd) {
  if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET)
    lock_sidnos(&thd->owned_gtid_set);
  else if (thd->owned_gtid.sidno > 0)
    lock_sidno(thd->owned_gtid.sidno);
}
#endif

void Gtid_state::unlock_owned_sidnos(const THD *thd) {
  if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET) {
#ifdef HAVE_GTID_NEXT_LIST
    unlock_sidnos(&thd->owned_gtid_set);
#else
    assert(0);
#endif
  } else if (thd->owned_gtid.sidno > 0) {
    unlock_sidno(thd->owned_gtid.sidno);
  }
}

void Gtid_state::broadcast_owned_sidnos(const THD *thd) {
  if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET) {
#ifdef HAVE_GTID_NEXT_LIST
    broadcast_sidnos(&thd->owned_gtid_set);
#else
    assert(0);
#endif
  } else if (thd->owned_gtid.sidno > 0) {
    broadcast_sidno(thd->owned_gtid.sidno);
  }
}

void Gtid_state::update_commit_group(THD *first_thd) {
  DBUG_TRACE;

  bool gtid_threshold_breach = false;
  /*
    We are going to loop in all sessions of the group commit in order to avoid
    being taking and releasing the global_tsid_lock and sidno_lock for each
    session.
  */
  DEBUG_SYNC(first_thd, "update_gtid_state_before_global_tsid_lock");
  global_tsid_lock->rdlock();
  DEBUG_SYNC(first_thd, "update_gtid_state_after_global_tsid_lock");

  update_gtids_impl_lock_sidnos(first_thd);

  for (THD *thd = first_thd; thd != nullptr; thd = thd->next_to_commit) {
    bool is_commit = (thd->commit_error != THD::CE_COMMIT_ERROR);

    if (update_gtids_impl_do_nothing(thd) ||
        (!is_commit && update_gtids_impl_check_skip_gtid_rollback(thd)))
      continue;

    bool more_trx_with_same_gtid_next = update_gtids_impl_begin(thd);

    if (!gtid_threshold_breach)
      gtid_threshold_breach = (thd->owned_gtid.gno > GNO_WARNING_THRESHOLD);

    if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET) {
      update_gtids_impl_own_gtid_set(thd, is_commit);
    } else if (thd->owned_gtid.sidno > 0) {
      update_gtids_impl_own_gtid(thd, is_commit);
    } else if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS) {
      update_gtids_impl_own_anonymous(thd, &more_trx_with_same_gtid_next);
    } else {
      update_gtids_impl_own_nothing(thd);
    }

    update_gtids_impl_end(thd, more_trx_with_same_gtid_next);
  }

  update_gtids_impl_broadcast_and_unlock_sidnos();

  global_tsid_lock->unlock();

  if (gtid_threshold_breach)
    LogErr(WARNING_LEVEL, ER_WARN_GTID_THRESHOLD_BREACH);
}

void Gtid_state::update_on_commit(THD *thd) {
  DBUG_TRACE;

  update_gtids_impl(thd, true);
  DEBUG_SYNC(thd, "end_of_gtid_state_update_on_commit");
}

void Gtid_state::update_on_rollback(THD *thd) {
  DBUG_TRACE;

  if (!update_gtids_impl_check_skip_gtid_rollback(thd))
    update_gtids_impl(thd, false);
}

void Gtid_state::update_gtids_impl(THD *thd, bool is_commit) {
  DBUG_TRACE;

  if (update_gtids_impl_do_nothing(thd)) return;

  bool more_trx_with_same_gtid_next = update_gtids_impl_begin(thd);

  DEBUG_SYNC(thd, "update_gtid_state_before_global_tsid_lock");
  global_tsid_lock->rdlock();
  DEBUG_SYNC(thd, "update_gtid_state_after_global_tsid_lock");
  bool gtid_threshold_breach = (thd->owned_gtid.gno > GNO_WARNING_THRESHOLD);

  if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET) {
    update_gtids_impl_own_gtid_set(thd, is_commit);
  } else if (thd->owned_gtid.sidno > 0) {
    rpl_sidno sidno = thd->owned_gtid.sidno;
    update_gtids_impl_lock_sidno(sidno);
    update_gtids_impl_own_gtid(thd, is_commit);
    update_gtids_impl_broadcast_and_unlock_sidno(sidno);
  } else if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_ANONYMOUS) {
    update_gtids_impl_own_anonymous(thd, &more_trx_with_same_gtid_next);
  } else {
    update_gtids_impl_own_nothing(thd);
  }

  global_tsid_lock->unlock();

  update_gtids_impl_end(thd, more_trx_with_same_gtid_next);

  if (gtid_threshold_breach)
    LogErr(WARNING_LEVEL, ER_WARN_GTID_THRESHOLD_BREACH);

  thd->owned_gtid.dbug_print(nullptr,
                             "set owned_gtid (clear) in update_gtids_impl");
}

void Gtid_state::end_gtid_violating_transaction(THD *thd) {
  DBUG_TRACE;
  if (thd->has_gtid_consistency_violation) {
    if (thd->variables.gtid_next.type == AUTOMATIC_GTID)
      end_automatic_gtid_violating_transaction();
    else {
      assert(thd->variables.gtid_next.type == ANONYMOUS_GTID);
      end_anonymous_gtid_violating_transaction();
    }
    thd->has_gtid_consistency_violation = false;
  }
}

bool Gtid_state::wait_for_sidno(THD *thd, rpl_sidno sidno,
                                struct timespec *abstime,
                                bool update_thd_status) {
  DBUG_TRACE;
  PSI_stage_info old_stage;
  PSI_stage_info stage = stage_waiting_for_gtid_to_be_committed;
  tsid_lock->assert_some_lock();
  tsid_locks.assert_owner(sidno);

  if (!update_thd_status) {
    // Keep the same stage info on the new condition.
    stage.m_key = thd->get_current_stage_key();
    stage.m_name = thd->proc_info();
  }

  tsid_locks.enter_cond(thd, sidno, &stage, &old_stage);
  bool ret = tsid_locks.wait(thd, sidno, abstime);
  // Can't call tsid_locks.unlock() as that requires global_tsid_lock.
  mysql_mutex_unlock(thd->current_mutex);
  thd->EXIT_COND(&old_stage);
  return ret;
}

bool Gtid_state::wait_for_gtid(THD *thd, const Gtid &gtid,
                               struct timespec *abstime) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("SIDNO=%d GNO=%" PRId64 " thread_id=%u", gtid.sidno,
                      gtid.gno, thd->thread_id()));
  assert(!owned_gtids.is_owned_by(gtid, thd->thread_id()));
  assert(!owned_gtids.is_owned_by(gtid, 0));

  bool ret = wait_for_sidno(thd, gtid.sidno, abstime);
  return ret;
}

bool Gtid_state::wait_for_gtid_set(THD *thd, Gtid_set *wait_for, double timeout,
                                   bool update_thd_status) {
  struct timespec abstime;
  DBUG_TRACE;
  DEBUG_SYNC(thd, "begin_wait_for_executed_gtid_set");
  wait_for->dbug_print("Waiting for");
  DBUG_PRINT("info", ("Timeout %f", timeout));

  global_tsid_lock->assert_some_rdlock();

  assert(wait_for->get_tsid_map() == global_tsid_map);

  if (timeout > 0) {
    set_timespec_nsec(&abstime,
                      static_cast<ulonglong>(timeout * 1000000000ULL));
  }

  /*
    Algorithm:

    Let 'todo' contain the GTIDs to wait for. Iterate over SIDNOs in
    'todo' (this is the 'for' loop below).

    For each SIDNO in 'todo', remove gtid_executed for that SIDNO from
    'todo'.  If, after this removal, there is still some interval for
    this SIDNO in 'todo', then wait for a signal on this SIDNO.
    Repeat this step until 'todo' is empty for this SIDNO (this is the
    innermost 'while' loop below).

    Once the loop over SIDNOs has completed, 'todo' is guaranteed to
    be empty.  However, it may still be the case that not all GTIDs of
    wait_for are included in gtid_executed, since RESET BINARY LOGS AND GTIDS
    may have been executed while we were waiting.

    RESET BINARY LOGS AND GTIDS requires global_tsid_lock.wrlock.  We hold
    global_tsid_lock.rdlock while removing GTIDs from 'todo', but the
    wait operation releases global_tsid_lock.rdlock.  So if we
    completed the 'for' loop without waiting, we know for sure that
    global_tsid_lock.rdlock was held while emptying 'todo', and thus
    RESET BINARY LOGS AND GTIDS cannot have executed in the meantime.
    But if we waited at some point during the execution of the 'for'
    loop, RESET BINARY LOGS AND GTIDS may have been called.
    Thus, we repeatedly run 'for' loop until it completes without waiting
    (this is the outermost 'while' loop).
  */

  // Will be true once the entire 'for' loop completes without waiting.
  bool verified = false;

  // The set of GTIDs that we are still waiting for.
  Gtid_set todo(global_tsid_map, nullptr);
  // As an optimization, add 100 Intervals that do not need to be
  // allocated. This avoids allocation of these intervals.
  static const int preallocated_interval_count = 100;
  Gtid_set::Interval ivs[preallocated_interval_count];
  todo.add_interval_memory(preallocated_interval_count, ivs);

  /*
    Iterate until we have verified that all GTIDs in the set are
    included in gtid_executed.
  */
  while (!verified) {
    todo.add_gtid_set(wait_for);

    // Iterate over SIDNOs until all GTIDs have been removed from 'todo'.

    // Set 'verified' to true; it will be set to 'false' if any wait
    // is done.
    verified = true;
    for (int sidno = 1; sidno <= todo.get_max_sidno(); sidno++) {
      // Iterate until 'todo' is empty for this SIDNO.
      while (todo.contains_sidno(sidno)) {
        lock_sidno(sidno);
        todo.remove_intervals_for_sidno(&executed_gtids, sidno);

        if (todo.contains_sidno(sidno)) {
          bool ret = wait_for_sidno(
              thd, sidno, timeout > 0 ? &abstime : nullptr, update_thd_status);

          // wait_for_gtid will release both the global lock and the
          // mutex.  Acquire the global lock again.
          global_tsid_lock->rdlock();
          verified = false;

          if (thd->killed) {
            switch (thd->killed.load()) {
              case ER_SERVER_SHUTDOWN:
              case ER_QUERY_INTERRUPTED:
              case ER_QUERY_TIMEOUT:
                my_error(thd->killed, MYF(0));
                break;
              default:
                my_error(ER_QUERY_INTERRUPTED, MYF(0));
                break;
            }
            return true;
          }

          if (ret) return true;
        } else {
          // Keep the global lock since it may be needed in a later
          // iteration of the for loop.
          unlock_sidno(sidno);
          break;
        }
      }
    }
  }
  return false;
}

rpl_gno Gtid_state::get_automatic_gno(rpl_sidno sidno) const {
  DBUG_TRACE;
  assert_sidno_lock_owner(sidno);
  Gtid_set::Const_interval_iterator ivit(&executed_gtids, sidno);
  /*
    When assigning new automatic GTIDs, we can optimize the assignment by start
    searching an available GNO from the "supposed" next free one instead of
    starting from 1.

    This is useful mostly on systems having many transactions committing in
    group asking for automatic GTIDs. When a GNO is assigned to be owned by a
    transaction, it is not removed from the free intervals, but will be added
    to the owned_gtids set. In this way, picking up the actual first free GNO
    would often lead to getting a GNO already owned by other thread. This can
    lead to many "tries" of getting a free and not owned yet GNO (a thread
    would try N times, N being the sum of transactions in the FLUSH stage plus
    the transactions in the COMMIT stage that didn't released their ownership
    yet).

    The optimization just set next_free_gno variable to the last assigned
    GNO + 1, as this would be the common case without having transactions
    rolling back. This is done at Gtid_state::generate_automatic_gtid.

    In order to fill the gaps of GTID_EXECUTED when a transaction rolls back
    releasing the ownership of a GTID, we check if the released GNO is smaller
    than the next_free_gno at Gtid_state::update_gtids_impl_own_gtid function
    to set next_free_gno with the "released" GNO in this case.
  */
  auto next_free_gno_res = next_free_gno_map.find(sidno);

  Gtid next_candidate = {sidno, next_free_gno_res == next_free_gno_map.end()
                                    ? 1
                                    : next_free_gno_res->second};
  while (true) {
    const Gtid_set::Interval *iv = ivit.get();
    rpl_gno next_interval_start = iv != nullptr ? iv->start : GNO_END;
    while (next_candidate.gno < next_interval_start &&
           DBUG_EVALUATE_IF("simulate_gno_exhausted", false, true)) {
      DBUG_PRINT("debug", ("Checking availability of gno= %" PRId64,
                           next_candidate.gno));
      if (owned_gtids.is_owned_by(next_candidate, 0)) return next_candidate.gno;
      next_candidate.gno++;
    }
    if (iv == nullptr ||
        DBUG_EVALUATE_IF("simulate_gno_exhausted", true, false)) {
      my_error(ER_GNO_EXHAUSTED, MYF(0));
      return -1;
    }
    if (next_candidate.gno < iv->end) next_candidate.gno = iv->end;
    ivit.next();
  }
}

rpl_gno Gtid_state::get_last_executed_gno(rpl_sidno sidno) const {
  DBUG_TRACE;
  rpl_gno gno = 0;

  gtid_state->lock_sidno(sidno);
  gno = executed_gtids.get_last_gno(sidno);
  gtid_state->unlock_sidno(sidno);

  return gno;
}

rpl_sidno Gtid_state::specify_transaction_sidno(
    THD *thd, Gtid_state::Locked_sidno_set &sidno_set) {
  DBUG_TRACE;
  const auto &gtid_next = thd->variables.gtid_next;
  rpl_sidno sidno =
      thd->get_transaction()->get_rpl_transaction_ctx()->get_sidno();
  if (gtid_next.is_automatic() &&
      global_gtid_mode.get() >= Gtid_mode::ON_PERMISSIVE) {
    if (gtid_next.is_automatic_tagged() && sidno == 0) {
      sidno = global_tsid_map->add_tsid(
          Tsid(get_server_tsid().get_uuid(), gtid_next.generate_tag()));
    }
    if (sidno == 0) {
      sidno = get_server_sidno();
    }
    sidno_set.add_lock_for_sidno(sidno);
  }
  return sidno;
}

enum_return_status Gtid_state::generate_automatic_gtid(
    THD *thd, rpl_sidno specified_sidno, rpl_gno specified_gno) {
  DBUG_TRACE;
  enum_return_status ret = RETURN_STATUS_OK;

  [[maybe_unused]] const auto &gtid_next = thd->variables.gtid_next;

  assert(gtid_next.is_automatic());
  assert(specified_sidno >= 0);
  assert(specified_gno >= 0);
  assert(thd->owned_gtid.is_empty());

  // If GTID_MODE = ON_PERMISSIVE or ON, generate a new GTID
  if (global_gtid_mode.get() >= Gtid_mode::ON_PERMISSIVE) {
    Gtid automatic_gtid = {specified_sidno, specified_gno};
    assert(specified_sidno > 0);  // assert that sidno has been already assigned
    if (automatic_gtid.gno == 0) {
      automatic_gtid.gno = get_automatic_gno(automatic_gtid.sidno);
      if (automatic_gtid.gno != -1) {
        // insert new element or update the map element
        next_free_gno_map[automatic_gtid.sidno] = automatic_gtid.gno + 1;
      }
    }
    if (automatic_gtid.gno == -1 || acquire_ownership(thd, automatic_gtid))
      ret = RETURN_STATUS_REPORTED_ERROR;

  } else {
    // If GTID_MODE = OFF or OFF_PERMISSIVE, just mark this thread as
    // using an anonymous transaction.
    thd->owned_gtid.sidno = THD::OWNED_SIDNO_ANONYMOUS;
    thd->owned_gtid.gno = 0;
    acquire_anonymous_ownership();
    thd->owned_gtid.dbug_print(
        nullptr, "set owned_gtid (anonymous) in generate_automatic_gtid");
  }

  gtid_set_performance_schema_values(thd);

  return ret;
}

void Gtid_state::lock_sidnos(const Gtid_set *gs) {
  assert(gs);
  rpl_sidno max_sidno = gs->get_max_sidno();
  for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++)
    if (gs->contains_sidno(sidno)) lock_sidno(sidno);
}

void Gtid_state::unlock_sidnos(const Gtid_set *gs) {
  assert(gs);
  rpl_sidno max_sidno = gs->get_max_sidno();
  for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++)
    if (gs->contains_sidno(sidno)) unlock_sidno(sidno);
}

void Gtid_state::broadcast_sidnos(const Gtid_set *gs) {
  assert(gs);
  rpl_sidno max_sidno = gs->get_max_sidno();
  for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++)
    if (gs->contains_sidno(sidno)) broadcast_sidno(sidno);
}

enum_return_status Gtid_state::ensure_sidno() {
  DBUG_TRACE;
  tsid_lock->assert_some_wrlock();
  rpl_sidno sidno = tsid_map->get_max_sidno();
  if (sidno > 0) {
    // The lock may be temporarily released during one of the calls to
    // ensure_sidno or ensure_index.  Hence, we must re-check the
    // condition after the calls.
    PROPAGATE_REPORTED_ERROR(executed_gtids.ensure_sidno(sidno));
    PROPAGATE_REPORTED_ERROR(gtids_only_in_table.ensure_sidno(sidno));
    PROPAGATE_REPORTED_ERROR(previous_gtids_logged.ensure_sidno(sidno));
    PROPAGATE_REPORTED_ERROR(lost_gtids.ensure_sidno(sidno));
    PROPAGATE_REPORTED_ERROR(owned_gtids.ensure_sidno(sidno));
    PROPAGATE_REPORTED_ERROR(tsid_locks.ensure_index(sidno));
    PROPAGATE_REPORTED_ERROR(ensure_commit_group_sidnos(sidno));
    sidno = tsid_map->get_max_sidno();
    assert(executed_gtids.get_max_sidno() >= sidno);
    assert(gtids_only_in_table.get_max_sidno() >= sidno);
    assert(previous_gtids_logged.get_max_sidno() >= sidno);
    assert(lost_gtids.get_max_sidno() >= sidno);
    assert(owned_gtids.get_max_sidno() >= sidno);
    assert(tsid_locks.get_max_index() >= sidno);
    assert(commit_group_sidnos.size() >= (unsigned int)sidno);
  }
  RETURN_OK;
}

void Gtid_state::update_prev_gtids(Gtid_set *write_gtid_set) {
  DBUG_ENTER("Gtid_state::add_prev_gtids()");
  write_gtid_set->dbug_print("add_prev_gtids");

  if (!opt_bin_log) {
    DBUG_VOID_RETURN;
  }
  global_tsid_lock->wrlock();

  /* Remove from list if GTID is already written. */
  write_gtid_set->remove_gtid_set(&previous_gtids_logged);

  /* Add to the list so that it won't be written again later. */
  previous_gtids_logged.add_gtid_set(write_gtid_set);

  global_tsid_lock->unlock();
  DBUG_VOID_RETURN;
}

enum_return_status Gtid_state::add_lost_gtids(Gtid_set *gtid_set,
                                              bool starts_with_plus) {
  DBUG_TRACE;
  tsid_lock->assert_some_wrlock();

  gtid_set->dbug_print("add_lost_gtids");

  if (!starts_with_plus) {
    if (!gtid_state->get_lost_gtids()->is_subset(gtid_set)) {
      my_error(ER_CANT_SET_GTID_PURGED_DUE_SETS_CONSTRAINTS, MYF(0),
               "the new value must be a superset of the old value");
      RETURN_REPORTED_ERROR;
    }
    /*
      Remove @@GLOBAL.GTID_PURGED from gtid_set. This ensures that
      the next check generates an error only if gtid_set intersects
      (@@GLOBAL.GTID_EXECUTED - @@GLOBAL.GTID_PURGED).
    */
    gtid_set->remove_gtid_set(gtid_state->get_lost_gtids());
  }
  if (executed_gtids.is_intersection_nonempty(gtid_set)) {
    my_error(ER_CANT_SET_GTID_PURGED_DUE_SETS_CONSTRAINTS, MYF(0),
             "the added gtid set must not overlap with @@GLOBAL.GTID_EXECUTED");
    RETURN_REPORTED_ERROR;
  }
  assert(!lost_gtids.is_intersection_nonempty(gtid_set));

  if (owned_gtids.is_intersection_nonempty(gtid_set)) {
    my_error(ER_CANT_SET_GTID_PURGED_DUE_SETS_CONSTRAINTS, MYF(0),
             "the added gtid set must not overlap with @@GLOBAL.GTID_OWNED");
    RETURN_REPORTED_ERROR;
  }

  if (save(gtid_set)) RETURN_REPORTED_ERROR;
  PROPAGATE_REPORTED_ERROR(gtids_only_in_table.add_gtid_set(gtid_set));
  PROPAGATE_REPORTED_ERROR(lost_gtids.add_gtid_set(gtid_set));
  PROPAGATE_REPORTED_ERROR(executed_gtids.add_gtid_set(gtid_set));
  lock_sidnos(gtid_set);
  broadcast_sidnos(gtid_set);
  unlock_sidnos(gtid_set);

  return RETURN_STATUS_OK;
}

int Gtid_state::init() {
  DBUG_TRACE;

  global_tsid_lock->assert_some_wrlock();

  rpl_sid server_sid{};
  if (server_sid.parse(server_uuid, mysql::gtid::Uuid::TEXT_LENGTH) != 0)
    return 1;
  rpl_sidno sidno =
      tsid_map->add_tsid(Tsid_map::Tsid(server_sid, Tsid_map::Tag()));
  if (sidno <= 0) return 1;
  server_sidno = sidno;
  next_free_gno_map.clear();
  return 0;
}

int Gtid_state::save(THD *thd) {
  DBUG_TRACE;
  assert(gtid_table_persistor != nullptr);
  assert(thd->owned_gtid.sidno > 0);
  int error = 0;

  int ret = gtid_table_persistor->save(thd, &thd->owned_gtid);
  if (1 == ret) {
    /*
      Gtid table is not ready to be used, so failed to
      open it. Ignore the error.
    */
    thd->clear_error();
    if (!thd->get_stmt_da()->is_set())
      thd->get_stmt_da()->set_ok_status(0, 0, nullptr);
  } else if (-1 == ret)
    error = -1;

  return error;
}

int Gtid_state::save(const Gtid_set *gtid_set) {
  DBUG_TRACE;
  int ret = gtid_table_persistor->save(gtid_set);
  return ret;
}

int Gtid_state::save_gtids_of_last_binlog_into_table() {
  DBUG_TRACE;
  int ret = 0;

  if (DBUG_EVALUATE_IF("gtid_executed_readonly", true, false)) {
    my_error(ER_DA_RPL_GTID_TABLE_CANNOT_OPEN, MYF(0), "mysql",
             "gtid_executed");
    return ER_RPL_GTID_TABLE_CANNOT_OPEN;
  }

  /*
    Use local Tsid_map, so that we don't need a lock while inserting
    into the table.
  */
  Tsid_map tsid_map(nullptr);
  Gtid_set logged_gtids_last_binlog(&tsid_map, nullptr);
  // Allocate some intervals on stack to reduce allocation.
  static const int PREALLOCATED_INTERVAL_COUNT = 64;
  Gtid_set::Interval iv[PREALLOCATED_INTERVAL_COUNT];
  logged_gtids_last_binlog.add_interval_memory(PREALLOCATED_INTERVAL_COUNT, iv);
  /*
    logged_gtids_last_binlog= executed_gtids - previous_gtids_logged -
                              gtids_only_in_table
  */
  global_tsid_lock->wrlock();
  ret = (logged_gtids_last_binlog.add_gtid_set(&executed_gtids) !=
         RETURN_STATUS_OK);
  if (!ret) {
    logged_gtids_last_binlog.remove_gtid_set(&previous_gtids_logged);
    logged_gtids_last_binlog.remove_gtid_set(&gtids_only_in_table);
    if (!logged_gtids_last_binlog.is_empty()) {
      /* Prepare previous_gtids_logged for next binlog always. Need it
      even during shutdown to synchronize with innodb GTID persister. */
      if (previous_gtids_logged.add_gtid_set(&logged_gtids_last_binlog))
        ret = ER_OOM_SAVE_GTIDS;

      global_tsid_lock->unlock();
      /* Save set of GTIDs of the last binlog into gtid_executed table */
      if (!ret) {
        if (save(&logged_gtids_last_binlog))
          ret = ER_RPL_GTID_TABLE_CANNOT_OPEN;
      }
    } else
      global_tsid_lock->unlock();
  } else
    global_tsid_lock->unlock();

  return ret;
}

int Gtid_state::read_gtid_executed_from_table() {
  return gtid_table_persistor->fetch_gtids(&executed_gtids);
}

int Gtid_state::compress(THD *thd) {
  return gtid_table_persistor->compress(thd);
}

int Gtid_state::warn_or_err_on_modify_gtid_table(THD *thd, Table_ref *table) {
  DBUG_TRACE;
  int ret =
      gtid_table_persistor->warn_or_err_on_explicit_modification(thd, table);
  return ret;
}

bool Gtid_state::update_gtids_impl_check_skip_gtid_rollback(THD *thd) {
  if (thd->skip_gtid_rollback) {
    DBUG_PRINT("info", ("skipping gtid rollback because "
                        "thd->skip_gtid_rollback is set"));
    return true;
  }
  return false;
}

bool Gtid_state::update_gtids_impl_do_nothing(THD *thd) {
  if (thd->owned_gtid_is_empty() && !thd->has_gtid_consistency_violation) {
    if (thd->variables.gtid_next.type == ASSIGNED_GTID)
      thd->variables.gtid_next.set_undefined();
    DBUG_PRINT("info", ("skipping update_gtids_impl because "
                        "thread does not own anything and does not violate "
                        "gtid consistency"));

    return true;
  }
  return false;
}

bool Gtid_state::update_gtids_impl_begin(THD *thd) {
#ifndef NDEBUG
  if (current_thd != thd) mysql_mutex_lock(&thd->LOCK_thd_query);
  DBUG_PRINT("info", ("query='%s' thd->is_commit_in_middle_of_statement=%d",
                      thd->query().str, thd->is_commit_in_middle_of_statement));
  if (current_thd != thd) mysql_mutex_unlock(&thd->LOCK_thd_query);
#endif
  return thd->is_commit_in_middle_of_statement;
}

void Gtid_state ::update_gtids_impl_own_gtid_set(THD *thd [[maybe_unused]],
                                                 bool is_commit
                                                 [[maybe_unused]]) {
#ifdef HAVE_GTID_NEXT_LIST
  rpl_sidno prev_sidno = 0;
  Gtid_set::Gtid_iterator git(&thd->owned_gtid_set);
  Gtid g = git.get();
  while (g.sidno != 0) {
    if (g.sidno != prev_sidno) tsid_locks.lock(g.sidno);
    owned_gtids.remove_gtid(g);
    git.next();
    g = git.get();
    if (is_commit) executed_gtids._add_gtid(g);
  }

  if (is_commit && !thd->owned_gtid_set.is_empty())
    thd->rpl_thd_ctx.session_gtids_ctx().notify_after_gtid_executed_update(thd);

  thd->variables.gtid_next.set_undefined();
  thd->owned_gtid.dbug_print(nullptr,
                             "set owned_gtid (clear; old was gtid_set) "
                             "in update_gtids_impl");
  thd->clear_owned_gtids();
#else
  assert(0);
#endif
}

void Gtid_state::update_gtids_impl_lock_sidno(rpl_sidno sidno) {
  assert(sidno > 0);
  DBUG_PRINT("info", ("Locking sidno %d", sidno));
  lock_sidno(sidno);
}

void Gtid_state::update_gtids_impl_lock_sidnos(THD *first_thd) {
  /* Define which sidnos should be locked to be updated */
  for (THD *thd = first_thd; thd != nullptr; thd = thd->next_to_commit) {
    if (thd->owned_gtid.sidno > 0) {
      DBUG_PRINT("info",
                 ("Setting sidno %d to be locked", thd->owned_gtid.sidno));
      commit_group_sidnos[thd->owned_gtid.sidno] = true;
    } else if (thd->owned_gtid.sidno == THD::OWNED_SIDNO_GTID_SET)
#ifdef HAVE_GTID_NEXT_LIST
      for (rpl_sidno i = 1; i < thd->owned_gtid_set.max_sidno; i++)
        if (owned_gtid_set.contains_sidno(i)) commit_group_sidnos[i] = true;
#else
      assert(0);
#endif
  }

  /* Take the sidno_locks in order */
  for (rpl_sidno i = 1; i < (rpl_sidno)commit_group_sidnos.size(); i++)
    if (commit_group_sidnos[i]) update_gtids_impl_lock_sidno(i);
}

void Gtid_state::update_gtids_impl_own_gtid(THD *thd, bool is_commit) {
  assert_sidno_lock_owner(thd->owned_gtid.sidno);
  /*
    In Group Replication the GTID may additionally be owned by another
    thread, and we won't remove that ownership (it will be rolled back later)
  */
  assert(owned_gtids.is_owned_by(thd->owned_gtid, thd->thread_id()));
  owned_gtids.remove_gtid(thd->owned_gtid, thd->thread_id());

  if (is_commit) {
    assert(!executed_gtids.contains_gtid(thd->owned_gtid));
    DBUG_EXECUTE_IF(
        "rpl_gtid_update_on_commit_simulate_out_of_memory",
        DBUG_SET("+d,rpl_gtid_get_free_interval_simulate_out_of_memory"););
    /*
      Any session adds transaction owned GTID into global executed_gtids.

      If binlog is disabled, we report @@GLOBAL.GTID_PURGED from
      executed_gtids, since @@GLOBAL.GTID_PURGED and @@GLOBAL.GTID_EXECUTED
      are always same, so we did not save gtid into lost_gtids for every
      transaction for improving performance.

      If binlog is enabled and log_replica_updates is disabled, slave
      SQL thread or slave worker thread adds transaction owned GTID
      into global executed_gtids, lost_gtids and gtids_only_in_table.
    */
    CONDITIONAL_SYNC_POINT_FOR_TIMESTAMP("before_gtid_externalization");
    executed_gtids._add_gtid(thd->owned_gtid);
    thd->rpl_thd_ctx.session_gtids_ctx().notify_after_gtid_executed_update(thd);
    if (thd->slave_thread && opt_bin_log && !opt_log_replica_updates) {
      lost_gtids._add_gtid(thd->owned_gtid);
      gtids_only_in_table._add_gtid(thd->owned_gtid);
    }
  } else {
    auto iterator = next_free_gno_map.end();
    std::tie(iterator, std::ignore) =
        next_free_gno_map.try_emplace(thd->owned_gtid.sidno, 1);
    rpl_gno &next_free_gno = iterator->second;
    if (next_free_gno > thd->owned_gtid.gno) {
      next_free_gno = thd->owned_gtid.gno;
    }
  }

  thd->clear_owned_gtids();
  if (thd->variables.gtid_next.type == ASSIGNED_GTID) {
    assert(!thd->is_commit_in_middle_of_statement);
    thd->variables.gtid_next.set_undefined();
  } else {
    /*
      Can be UNDEFINED for statements where
      gtid_pre_statement_checks skips the test for undefined,
      e.g. ROLLBACK.
    */
    assert(thd->variables.gtid_next.is_automatic() ||
           thd->variables.gtid_next.is_undefined());
  }
}

void Gtid_state::update_gtids_impl_broadcast_and_unlock_sidno(rpl_sidno sidno) {
  DBUG_PRINT("info", ("Unlocking sidno %d", sidno));
  broadcast_sidno(sidno);
  unlock_sidno(sidno);
}

void Gtid_state::update_gtids_impl_broadcast_and_unlock_sidnos() {
  for (rpl_sidno i = 1; i < (rpl_sidno)commit_group_sidnos.size(); i++)
    if (commit_group_sidnos[i]) {
      update_gtids_impl_broadcast_and_unlock_sidno(i);
      commit_group_sidnos[i] = false;
    }
}

void Gtid_state::update_gtids_impl_own_anonymous(THD *thd, bool *more_trx) {
  assert(thd->variables.gtid_next.type == ANONYMOUS_GTID ||
         thd->variables.gtid_next.type == AUTOMATIC_GTID);
  /*
    If there is more in the transaction cache, set more_trx to indicate this.

    See comment for the update_gtids_impl_begin function.
  */
  if (opt_bin_log) {
    if (!thd->is_binlog_cache_empty(true)) {
      *more_trx = true;
      DBUG_PRINT("info", ("Transaction cache is non-empty: setting "
                          "more_transaction_with_same_gtid_next="
                          "true."));
    }
  }
  if (!(*more_trx && thd->variables.gtid_next.type == ANONYMOUS_GTID)) {
    release_anonymous_ownership();
    thd->clear_owned_gtids();
  }
}

void Gtid_state::update_gtids_impl_own_nothing(THD *thd [[maybe_unused]]) {
  assert(thd->commit_error != THD::CE_COMMIT_ERROR ||
         thd->has_gtid_consistency_violation);
  assert(thd->variables.gtid_next.type == AUTOMATIC_GTID);
}

void Gtid_state::update_gtids_impl_end(THD *thd, bool more_trx) {
  if (!more_trx) end_gtid_violating_transaction(thd);
}

enum_return_status Gtid_state::ensure_commit_group_sidnos(rpl_sidno sidno) {
  DBUG_TRACE;
  tsid_lock->assert_some_wrlock();
  /*
    As we use the sidno as index of commit_group_sidnos and there is no
    sidno=0, the array size must be at least sidno + 1.
  */
  while ((commit_group_sidnos.size()) < (size_t)sidno + 1) {
    if (commit_group_sidnos.push_back(false)) goto error;
  }
  RETURN_OK;
error:
  BINLOG_ERROR(("Out of memory."), (ER_OUT_OF_RESOURCES, MYF(0)));
  RETURN_REPORTED_ERROR;
}
