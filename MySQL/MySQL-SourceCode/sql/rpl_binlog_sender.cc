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

#include "sql/rpl_binlog_sender.h"
#include "mysql/binlog/event/codecs/factory.h"

#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <utility>

#include "lex_string.h"
#include "map_helpers.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_pointer_arithmetic.h"
#include "my_sys.h"
#include "my_thread.h"
#include "mysql.h"
#include "mysql/binlog/event/binlog_event.h"  // mysql::binlog::event::max_log_event_size
#include "mysql/components/services/bits/psi_stage_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/my_loglevel.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/psi/mysql_mutex.h"
#include "scope_guard.h"
#include "sql/binlog_reader.h"
#include "sql/debug_sync.h"  // debug_sync_set_action
#include "sql/derror.h"      // ER_THD
#include "sql/item_func.h"   // user_var_entry
#include "sql/log.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"  // global_system_variables ...
#include "sql/protocol.h"
#include "sql/protocol_classic.h"
#include "sql/rpl_constants.h"  // BINLOG_DUMP_NON_BLOCK
#include "sql/rpl_gtid.h"
#include "sql/rpl_handler.h"    // RUN_HOOK
#include "sql/rpl_reporting.h"  // MAX_SLAVE_ERRMSG
#include "sql/rpl_source.h"     // opt_sporadic_binlog_dump_fail
#include "sql/sql_class.h"      // THD
#include "sql/system_variables.h"
#include "sql_string.h"
#include "string_with_len.h"
#include "typelib.h"
#include "unsafe_string_append.h"

#ifndef NDEBUG
static uint binlog_dump_count = 0;
#endif
using mysql::binlog::event::checksum_crc32;

const uint32 Binlog_sender::PACKET_MIN_SIZE = 4096;
const uint32 Binlog_sender::PACKET_MAX_SIZE = UINT_MAX32;
const ushort Binlog_sender::PACKET_SHRINK_COUNTER_THRESHOLD = 100;
const float Binlog_sender::PACKET_GROW_FACTOR = 2.0;
const float Binlog_sender::PACKET_SHRINK_FACTOR = 0.5;

using mysql::binlog::event::Binary_log_event;
using mysql::binlog::event::enum_binlog_checksum_alg;
using mysql::binlog::event::Log_event_footer;
using mysql::binlog::event::Log_event_type;

/**
  @class Observe_transmission_guard

  Sentry class to guard the transitions for `Delegate::m_observe_transmission`
  flag within given contexts.

*/
class Observe_transmission_guard {
 public:
  /**
    Constructor for the class. It will change the value of the `flag` parameter
    according with the `event_type` and `event_ptr` content. The `flag` will be
    set to `true` as follows:

    - The event is an `XID_EVENT`
    - The event is an `XA_PREPARE_LOG_EVENT`.
    - The event is a `QUERY_EVENT` with query equal to "XA COMMIT" or "XA ABORT"
      or "COMMIT".
    - The event is the first `QUERY_EVENT` after a `GTID_EVENT` and the query is
      not "BEGIN" --the statement is a DDL, for instance.

    @param flag            The flag variable to guard
    @param event_type      The type of the event being processed
    @param event_ptr       The raw content of the event being processed
    @param checksum_alg    The checksum algorithm being used currently
    @param prev_event_type The type of the event processed just before the
                           current one
  */
  Observe_transmission_guard(
      bool &flag, mysql::binlog::event::Log_event_type event_type,
      const char *event_ptr,
      mysql::binlog::event::enum_binlog_checksum_alg checksum_alg,
      mysql::binlog::event::Log_event_type prev_event_type)
      : m_saved(flag), m_to_set(flag) {
    if (opt_replication_sender_observe_commit_only) {
      switch (event_type) {
        case mysql::binlog::event::TRANSACTION_PAYLOAD_EVENT:
        case mysql::binlog::event::XID_EVENT:
        case mysql::binlog::event::XA_PREPARE_LOG_EVENT: {
          m_to_set = true;
          break;
        }
        case mysql::binlog::event::QUERY_EVENT: {
          bool first_event_after_gtid =
              mysql::binlog::event::Log_event_type_helper::is_any_gtid_event(
                  prev_event_type);

          Format_description_log_event fd_ev;
          fd_ev.common_footer->checksum_alg = checksum_alg;
          Query_log_event ev(event_ptr, &fd_ev,
                             mysql::binlog::event::QUERY_EVENT);
          if (first_event_after_gtid)
            m_to_set = (strcmp("BEGIN", ev.query) != 0);
          else
            m_to_set = (strncmp("XA COMMIT", ev.query, 9) == 0) ||
                       (strncmp("XA ABORT", ev.query, 8) == 0) ||
                       (strncmp("COMMIT", ev.query, 6) == 0);
          break;
        }
        default: {
          m_to_set = false;
          break;
        }
      }
    }
  }

  /**
    Destructor for the sentry class. It will instantiate the guarded flag with
    the value prior to the creation of this object.
  */
  ~Observe_transmission_guard() { m_to_set = m_saved; }

 private:
  /** The value of the guarded flag upon this object creation */
  bool m_saved;
  /** The flag variable to guard */
  bool &m_to_set;
};

/**
  @class Sender_context_guard

  Sentry class that guards the Binlog_sender context and, at destruction, will
  prepare it for the next event to be processed.
*/
class Sender_context_guard {
 public:
  /**
    Class constructor that simply stores, internally, the reference for the
    `Binlog_sender` to be guarded and the values to be set upon destruction.

    @param target     The `Binlog_sender` object to be guarded.
    @param event_type The currently processed event type, to be used for context
                      of the next event processing round.
  */
  Sender_context_guard(Binlog_sender &target,
                       mysql::binlog::event::Log_event_type event_type)
      : m_target(target), m_event_type(event_type) {}

  /**
    Class destructor that will set the proper context of the guarded
    `Binlog_sender` object.
  */
  virtual ~Sender_context_guard() {
    m_target.set_prev_event_type(m_event_type);
  }

 private:
  /** The object to be guarded */
  Binlog_sender &m_target;
  /** The currently being processed event type */
  mysql::binlog::event::Log_event_type m_event_type;
};

/**
  Simple function to help readability w.r.t. chrono operations.

  This function SHALL return a nanoseconds duration representing
  the current time. It is just a convenience function to interface
  with std::chrono::high_resolution_clock::now().

  @return a std::chrono::nanoseconds duration since the epoch.
 */
static std::chrono::nanoseconds now_in_nanosecs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::high_resolution_clock::now().time_since_epoch());
}

/**
  Binlog_sender reads events one by one. It uses the preallocated memory
  (A String object) to store all event_data instead of allocating memory when
  reading each event_data. So event should not free the memory at destructor.
*/
class Binlog_sender::Event_allocator {
 public:
  enum { DELEGATE_MEMORY_TO_EVENT_OBJECT = false };

  void set_sender(Binlog_sender *sender) { m_sender = sender; }
  unsigned char *allocate(size_t size) {
    my_off_t event_offset = m_sender->m_packet.length();
    if (m_sender->grow_packet(size)) return nullptr;

    m_sender->m_packet.length(event_offset + size);
    return pointer_cast<unsigned char *>(m_sender->m_packet.ptr() +
                                         event_offset);
  }

  void deallocate(unsigned char *ptr [[maybe_unused]]) {}

 private:
  Binlog_sender *m_sender = nullptr;
};

Binlog_sender::Binlog_sender(THD *thd, const char *start_file,
                             my_off_t start_pos, Gtid_set *exclude_gtids,
                             uint32 flag)
    : m_thd(thd),
      m_packet(*thd->get_protocol_classic()->get_output_packet()),
      m_start_file(start_file),
      m_start_pos(start_pos),
      m_exclude_gtid(exclude_gtids),
      m_using_gtid_protocol(exclude_gtids != nullptr),
      m_check_previous_gtid_event(exclude_gtids != nullptr),
      m_gtid_clear_fd_created_flag(exclude_gtids == nullptr),
      m_diag_area(false),
      m_errmsg(nullptr),
      m_errno(0),
      m_last_file(nullptr),
      m_last_pos(0),
      m_half_buffer_size_req_counter(0),
      m_new_shrink_size(PACKET_MIN_SIZE),
      m_flag(flag),
      m_observe_transmission(false),
      m_transmit_started(false),
      m_prev_event_type(mysql::binlog::event::UNKNOWN_EVENT) {}

void Binlog_sender::init() {
  DBUG_TRACE;
  THD *thd = m_thd;

  thd->push_diagnostics_area(&m_diag_area);
  init_heartbeat_period();
  m_last_event_sent_ts = now_in_nanosecs();

  m_linfo.thread_id = thd->thread_id();
  mysql_bin_log.register_log_info(&m_linfo);

  /* Initialize the buffer only once. */
  m_packet.mem_realloc(PACKET_MIN_SIZE);  // size of the buffer
  m_new_shrink_size = PACKET_MIN_SIZE;
  DBUG_PRINT("info", ("Initial packet->alloced_length: %zu",
                      m_packet.alloced_length()));

  if (!mysql_bin_log.is_open()) {
    set_fatal_error("Binary log is not open");
    return;
  }

  if (DBUG_EVALUATE_IF("simulate_no_server_id", true, server_id == 0)) {
    set_fatal_error("Misconfigured source - source server_id is 0");
    return;
  }

  if (m_using_gtid_protocol) {
    auto gtid_mode = global_gtid_mode.get();
    if (gtid_mode != Gtid_mode::ON) {
      char buf[MYSQL_ERRMSG_SIZE];
      sprintf(buf,
              "The replication sender thread cannot start in "
              "AUTO_POSITION mode: this server has GTID_MODE = %.192s "
              "instead of ON.",
              Gtid_mode::to_string(gtid_mode));
      set_fatal_error(buf);
      return;
    }
  }

  if (check_start_file()) return;

  LogErr(INFORMATION_LEVEL, ER_RPL_BINLOG_STARTING_DUMP, thd->thread_id(),
         thd->server_id, m_start_file, m_start_pos);

  if (RUN_HOOK(
          binlog_transmit, transmit_start,
          (thd, m_flag, m_start_file, m_start_pos, &m_observe_transmission))) {
    set_unknown_error("Failed to run hook 'transmit_start'");
    return;
  }
  m_transmit_started = true;

  init_checksum_alg();
  /*
    There are two ways to tell the server to not block:

    - Set the BINLOG_DUMP_NON_BLOCK flag.
      This is official, documented, not used by any mysql
      client, but used by some external users.

    - Set server_id=0.
      This is unofficial, undocumented, and used by
      mysqlbinlog -R since the beginning of time.

    When mysqlbinlog --stop-never is used, it sets a 'fake'
    server_id that defaults to 1 but can be set to anything
    else using stop-never-slave-server-id. This has the
    drawback that if the server_id conflicts with any other
    running slave, or with any other instance of mysqlbinlog
    --stop-never, then that other instance will be killed.  It
    is also an unnecessary burden on the user to have to
    specify a server_id different from all other server_ids
    just to avoid conflicts.

    As of MySQL 5.6.20 and 5.7.5, mysqlbinlog redundantly sets
    the BINLOG_DUMP_NONBLOCK flag when one or both of the
    following holds:
    - the --stop-never option is *not* specified

    In a far future, this means we can remove the unofficial
    functionality that server_id=0 implies nonblocking
    behavior. That will allow mysqlbinlog to use server_id=0
    always. That has the advantage that mysqlbinlog
    --stop-never cannot cause any running dump threads to be
    killed.
  */
  m_wait_new_events =
      !((thd->server_id == 0) || ((m_flag & BINLOG_DUMP_NON_BLOCK) != 0));
  /* Binary event can be vary large. So set it to max allowed packet. */
  thd->variables.max_allowed_packet = mysql::binlog::event::max_log_event_size;

#ifndef NDEBUG
  if (opt_sporadic_binlog_dump_fail && (binlog_dump_count++ % 2))
    set_unknown_error(
        "Source fails in COM_BINLOG_DUMP because of "
        "--sporadic-binlog-dump-fail");
  m_event_count = 0;
#endif
}

void Binlog_sender::cleanup() {
  DBUG_TRACE;

  THD *thd = m_thd;

  if (m_transmit_started)
    (void)RUN_HOOK(binlog_transmit, transmit_stop, (thd, m_flag));

  mysql_bin_log.unregister_log_info(&m_linfo);

  thd->variables.max_allowed_packet =
      global_system_variables.max_allowed_packet;

  thd->pop_diagnostics_area();
  if (has_error())
    my_message(m_errno, m_errmsg, MYF(0));
  else
    my_eof(thd);
}

void Binlog_sender::run() {
  DBUG_TRACE;

  init();

  unsigned int max_event_size =
      std::max(m_thd->variables.max_allowed_packet,
               binlog_row_event_max_size + MAX_LOG_EVENT_HEADER);
  File_reader reader(opt_source_verify_checksum, max_event_size);
  my_off_t start_pos = m_start_pos;
  const char *log_file = m_linfo.log_file_name;
  bool is_index_file_reopened_on_binlog_disable = false;

  reader.allocator()->set_sender(this);
  while (!has_error() && !m_thd->killed) {
    /*
      Faked rotate event is only required in a few cases(see comment of the
      function). But even so, a faked rotate event is always sent before sending
      event log file, even if a rotate log event exists in last binlog and
      was already sent. The slave then gets an extra rotation and records
      two Rotate_log_events.

      The main issue here are some dependencies on mysqlbinlog, that should be
      solved in the future.
    */
    if (unlikely(fake_rotate_event(log_file, start_pos))) break;

    if (reader.open(log_file)) {
      set_fatal_error(log_read_error_msg(reader.get_error_type()));
      break;
    }

    THD_STAGE_INFO(m_thd, stage_sending_binlog_event_to_replica);
    if (send_binlog(reader, start_pos)) break;

    /* Will go to next file, need to copy log file name */
    set_last_file(log_file);

    THD_STAGE_INFO(m_thd,
                   stage_finished_reading_one_binlog_switching_to_next_binlog);
    DBUG_EXECUTE_IF("waiting_for_disable_binlog", {
      const char act[] =
          "now "
          "signal dump_thread_reached_wait_point "
          "wait_for continue_dump_thread no_clear_event";
      assert(!debug_sync_set_action(m_thd, STRING_WITH_LEN(act)));
    };);
    mysql_bin_log.lock_index();
    if (!mysql_bin_log.is_open()) {
      if (mysql_bin_log.open_index_file(mysql_bin_log.get_index_fname(),
                                        log_file, false)) {
        set_fatal_error(
            "Binary log is not open and failed to open index file "
            "to retrieve next file.");
        mysql_bin_log.unlock_index();
        break;
      }
      is_index_file_reopened_on_binlog_disable = true;
    }
    int error = mysql_bin_log.find_next_log(&m_linfo, false);
    mysql_bin_log.unlock_index();
    if (unlikely(error)) {
      DBUG_EXECUTE_IF("waiting_for_disable_binlog", {
        const char act[] = "now signal consumed_binlog";
        assert(!debug_sync_set_action(m_thd, STRING_WITH_LEN(act)));
      };);
      if (is_index_file_reopened_on_binlog_disable)
        mysql_bin_log.close(LOG_CLOSE_INDEX, true /*need_lock_log=true*/,
                            true /*need_lock_index=true*/);
      set_fatal_error("could not find next log");
      break;
    }

    start_pos = BIN_LOG_HEADER_SIZE;
    reader.close();
  }

  THD_STAGE_INFO(m_thd, stage_waiting_to_finalize_termination);
  char error_text[MAX_SLAVE_ERRMSG + 100];

  /*
    If the dump thread was killed because of a duplicate slave UUID we
    will fail throwing an error to the slave so it will not try to
    reconnect anymore.
  */
  mysql_mutex_lock(&m_thd->LOCK_thd_data);
  bool was_killed_by_duplicate_slave_id = m_thd->duplicate_slave_id;
  mysql_mutex_unlock(&m_thd->LOCK_thd_data);
  if (was_killed_by_duplicate_slave_id)
    set_fatal_error(
        "A replica with the same server_uuid/server_id as this replica "
        "has connected to the source");

  if (reader.is_open()) {
    if (is_fatal_error()) {
      /* output events range to error message */
      snprintf(error_text, sizeof(error_text),
               "%s; the first event '%s' at %lld, "
               "the last event read from '%s' at %lld, "
               "the last byte read from '%s' at %lld.",
               m_errmsg, m_start_file, m_start_pos, m_last_file, m_last_pos,
               log_file, reader.position());
      set_fatal_error(error_text);
    }

    reader.close();
  }

  cleanup();
}

int Binlog_sender::send_binlog(File_reader &reader, my_off_t start_pos) {
  if (unlikely(send_format_description_event(reader, start_pos))) return 1;

  if (start_pos == BIN_LOG_HEADER_SIZE) start_pos = reader.position();

  if (m_check_previous_gtid_event) {
    bool has_prev_gtid_ev;
    if (has_previous_gtid_log_event(reader, &has_prev_gtid_ev)) return 1;

    if (!has_prev_gtid_ev) return 0;
  }

  /*
    Slave is requesting a position which is in the middle of a file,
    so seek to the correct position.
  */
  if (reader.position() != start_pos && reader.seek(start_pos)) return 1;

  while (!m_thd->killed) {
    auto [end_pos, code] = get_binlog_end_pos(reader);

    if (code) return 1;
    if (send_events(reader, end_pos)) return 1;
    /*
      It is not active binlog, send_events should not return unless
      it reads all events.
    */
    if (end_pos == 0) return 0;

    m_thd->killed.store(DBUG_EVALUATE_IF(
        "simulate_kill_dump", THD::KILL_CONNECTION, m_thd->killed.load()));

    DBUG_EXECUTE_IF("wait_after_binlog_EOF", {
      const char act[] = "now wait_for signal.rotate_finished no_clear_event";
      assert(!debug_sync_set_action(m_thd, STRING_WITH_LEN(act)));
    };);
  }
  return 1;
}

std::pair<my_off_t, int> Binlog_sender::get_binlog_end_pos(
    File_reader &reader) {
  DBUG_TRACE;
  my_off_t read_pos = reader.position();

  std::pair<my_off_t, int> result = std::make_pair(read_pos, 1);

  if (m_wait_new_events) {
    if (unlikely(wait_new_events(read_pos))) return result;
  }

  result.first = mysql_bin_log.get_binlog_end_pos();

  DBUG_PRINT("info", ("Reading file %s, seek pos %llu, end_pos is %llu",
                      m_linfo.log_file_name, read_pos, result.first));
  DBUG_PRINT("info", ("Active file is %s", mysql_bin_log.get_log_fname()));

  /* If this is a cold binlog file, we are done getting the end pos */
  if (unlikely(!mysql_bin_log.is_active(m_linfo.log_file_name))) {
    return std::make_pair(0, 0);
  }
  if (read_pos < result.first) {
    result.second = 0;
    return result;
  }
  flush_net();
  return result;
}

int Binlog_sender::send_heartbeat_event(my_off_t log_pos) {
  uint32 hb_version_flag = m_flag & USE_HEARTBEAT_EVENT_V2;
  DBUG_EXECUTE_IF("use_old_heartbeat_version", { hb_version_flag = 0; });
  return (hb_version_flag ? send_heartbeat_event_v2(log_pos)
                          : send_heartbeat_event_v1(log_pos));
}
int Binlog_sender::send_events(File_reader &reader, my_off_t end_pos) {
  DBUG_TRACE;

  THD *thd = m_thd;
  const char *log_file = m_linfo.log_file_name;
  my_off_t log_pos = reader.position();
  my_off_t exclude_group_end_pos = 0;
  bool in_exclude_group = false;

  while (likely(log_pos < end_pos) || end_pos == 0) {
    uchar *event_ptr = nullptr;
    uint32 event_len = 0;

    if (unlikely(thd->killed)) return 1;

    if (unlikely(read_event(reader, &event_ptr, &event_len))) return 1;

    if (event_ptr == nullptr) {
      if (end_pos == 0) return 0;  // Arrive the end of inactive file

      /*
        It is reading events before end_pos of active binlog file. In theory,
        it should never return nullptr. But RESET BINARY LOGS AND GTIDS doesn't
        check if there is any dump thread working. So it is possible that the
        active binlog file is reopened and truncated to 0 after
        RESET BINARY LOGS AND GTIDS.
      */
      set_fatal_error(log_read_error_msg(Binlog_read_error::SYSTEM_IO));
      return 1;
    }

    Log_event_type event_type = (Log_event_type)event_ptr[EVENT_TYPE_OFFSET];
    if (unlikely(check_event_type(event_type, log_file, log_pos))) return 1;

    DBUG_EXECUTE_IF("dump_thread_wait_before_send_xid", {
      if (event_type == mysql::binlog::event::XID_EVENT) {
        thd->get_protocol()->flush();
        const char act[] =
            "now "
            "wait_for signal.continue";
        assert(opt_debug_sync_timeout > 0);
        assert(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
      }
    });

    Sender_context_guard ctx_guard(*this, event_type);

    log_pos = reader.position();

    /*
      TODO: Set m_exclude_gtid to NULL if all gtids in m_exclude_gtid has
      be skipped. and maybe removing the gtid from m_exclude_gtid will make
      skip_event has better performance.
    */
    if (m_exclude_gtid &&
        (in_exclude_group = skip_event(event_ptr, in_exclude_group))) {
      /*
        If we have not send any event from past 'heartbeat_period' time
        period, then it is time to send a packet before skipping this group.
       */
      DBUG_EXECUTE_IF("inject_2sec_sleep_when_skipping_an_event",
                      { my_sleep(2000000); });
      auto now = now_in_nanosecs();
      assert(now >= m_last_event_sent_ts);

      // if enough time has elapsed so that we should send another heartbeat
      if (m_heartbeat_period > std::chrono::nanoseconds(0) &&
          (now - m_last_event_sent_ts) >= m_heartbeat_period) {
        if (send_heartbeat_event(log_pos)) return 1;
        exclude_group_end_pos = 0;
      } else {
        exclude_group_end_pos = log_pos;
      }
      DBUG_PRINT("info", ("Event of type %s is skipped",
                          Log_event::get_type_str(event_type)));
    } else {
      /*
        A heartbeat is required before sending a event, If some events are
        skipped. It notifies the slave to increase master_log_pos for
        excluded events.
      */
      if (exclude_group_end_pos) {
        /* Save a copy of the buffer content. */
        String tmp;
        tmp.copy(m_packet);
        tmp.length(m_packet.length());

        if (send_heartbeat_event(exclude_group_end_pos)) return 1;
        exclude_group_end_pos = 0;

        /* Restore the copy back. */
        m_packet.copy(tmp);
        m_packet.length(tmp.length());
      }

      Observe_transmission_guard obs_guard(
          m_observe_transmission, event_type,
          const_cast<const char *>(reinterpret_cast<char *>(event_ptr)),
          m_event_checksum_alg, m_prev_event_type);

      if (before_send_hook(log_file, log_pos)) return 1;
      if (unlikely(send_packet())) return 1;
      if (unlikely(after_send_hook(log_file, in_exclude_group ? log_pos : 0)))
        return 1;
    }
  }

  /*
    A heartbeat is needed before waiting for more events, if some
    events are skipped. This is needed so that the slave can increase
    master_log_pos correctly.
  */
  if (unlikely(in_exclude_group)) {
    if (send_heartbeat_event(log_pos)) return 1;
  }
  return 0;
}

bool Binlog_sender::check_event_type(Log_event_type type, const char *log_file,
                                     my_off_t log_pos) {
  if (type == mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT) {
    /*
      Normally, there will not be any anonymous events when
      auto_position is enabled, since both the master and the slave
      refuse to connect if the master is not using GTID_MODE=ON.
      However, if the master changes GTID_MODE after the connection
      was initialized, or if the slave requests to replicate
      transactions that appear before the last anonymous event, then
      this can happen. Then we generate this error to prevent sending
      anonymous transactions to the slave.
    */
    if (m_using_gtid_protocol) {
      DBUG_EXECUTE_IF("skip_sender_anon_autoposition_error",
                      { return false; };);
      char buf[MYSQL_ERRMSG_SIZE];
      snprintf(buf, MYSQL_ERRMSG_SIZE,
               ER_THD(m_thd, ER_CANT_REPLICATE_ANONYMOUS_WITH_AUTO_POSITION),
               log_file, log_pos);
      set_fatal_error(buf);
      return true;
    }
    /*
      Normally, there will not be any anonymous events when master has
      GTID_MODE=ON, since anonymous events are not generated when
      GTID_MODE=ON.  However, this can happen if the master changes
      GTID_MODE to ON when the slave has not yet replicated all
      anonymous transactions.
    */
    else if (global_gtid_mode.get() == Gtid_mode::ON) {
      char buf[MYSQL_ERRMSG_SIZE];
      snprintf(buf, MYSQL_ERRMSG_SIZE,
               ER_THD(m_thd, ER_CANT_REPLICATE_ANONYMOUS_WITH_GTID_MODE_ON),
               log_file, log_pos);
      set_fatal_error(buf);
      return true;
    }
  } else if (mysql::binlog::event::Log_event_type_helper::
                 is_assigned_gtid_event(type)) {
    /*
      Normally, there will not be any GTID events when master has
      GTID_MODE=OFF, since GTID events are not generated when
      GTID_MODE=OFF.  However, this can happen if the master changes
      GTID_MODE to OFF when the slave has not yet replicated all GTID
      transactions.
    */
    if (global_gtid_mode.get() == Gtid_mode::OFF) {
      char buf[MYSQL_ERRMSG_SIZE];
      snprintf(buf, MYSQL_ERRMSG_SIZE,
               ER_THD(m_thd, ER_CANT_REPLICATE_GTID_WITH_GTID_MODE_OFF),
               log_file, log_pos);
      set_fatal_error(buf);
      return true;
    }
  }
  return false;
}

inline bool Binlog_sender::skip_event(const uchar *event_ptr,
                                      bool in_exclude_group) {
  DBUG_TRACE;

  uint8 event_type = (Log_event_type)event_ptr[LOG_EVENT_OFFSET];
  switch (event_type) {
    case mysql::binlog::event::GTID_LOG_EVENT:
    case mysql::binlog::event::GTID_TAGGED_LOG_EVENT: {
      Format_description_log_event fd_ev;
      fd_ev.common_footer->checksum_alg = m_event_checksum_alg;
      Gtid_log_event gtid_ev(reinterpret_cast<const char *>(event_ptr), &fd_ev);
      Gtid gtid;
      gtid.sidno = gtid_ev.get_sidno(m_exclude_gtid->get_tsid_map());
      gtid.gno = gtid_ev.get_gno();
      return m_exclude_gtid->contains_gtid(gtid);
    }
    case mysql::binlog::event::ROTATE_EVENT:
      return false;
  }
  return in_exclude_group;
}

int Binlog_sender::wait_new_events(my_off_t log_pos) {
  int ret = 0;
  PSI_stage_info old_stage;

  /*
    MYSQL_BIN_LOG::binlog_end_pos is atomic. We should only acquire the
    LOCK_binlog_end_pos if we reached the end of the hot log and are going
    to wait for updates on the binary log (Binlog_sender::wait_new_event()).
  */
  if (stop_waiting_for_update(log_pos)) {
    return 0;
  }

  /* Some data may be in net buffer, it should be flushed before waiting */
  if (flush_net()) return 1;

  mysql_bin_log.lock_binlog_end_pos();

  m_thd->ENTER_COND(mysql_bin_log.get_log_cond(),
                    mysql_bin_log.get_binlog_end_pos_lock(),
                    &stage_source_has_sent_all_binlog_to_replica, &old_stage);

  if (m_heartbeat_period.count() > 0)
    ret = wait_with_heartbeat(log_pos);
  else
    ret = wait_without_heartbeat(log_pos);

  mysql_bin_log.unlock_binlog_end_pos();
  m_thd->EXIT_COND(&old_stage);

  return ret;
}

bool Binlog_sender::stop_waiting_for_update(my_off_t log_pos) const {
  if (mysql_bin_log.get_binlog_end_pos() > log_pos ||
      !mysql_bin_log.is_active(m_linfo.log_file_name) || m_thd->killed) {
    return true;
  }
  return false;
}

inline int Binlog_sender::wait_with_heartbeat(my_off_t log_pos) {
#ifndef NDEBUG
  ulong hb_info_counter = 0;
#endif

  while (!stop_waiting_for_update(log_pos)) {
    // ignoring timeout on conditional variable
    mysql_bin_log.wait_for_update(m_heartbeat_period);

    if (stop_waiting_for_update(log_pos)) {
      return 0;
    }
    mysql_bin_log.unlock_binlog_end_pos();
    Scope_guard lock([]() { mysql_bin_log.lock_binlog_end_pos(); });
#ifndef NDEBUG
    if (hb_info_counter < 3) {
      LogErr(INFORMATION_LEVEL, ER_RPL_BINLOG_SOURCE_SENDS_HEARTBEAT);
      hb_info_counter++;
      if (hb_info_counter == 3)
        LogErr(INFORMATION_LEVEL,
               ER_RPL_BINLOG_SKIPPING_REMAINING_HEARTBEAT_INFO);
    }
#endif
    if (send_heartbeat_event(log_pos)) return 1;
  }

  return 0;
}

inline int Binlog_sender::wait_without_heartbeat(my_off_t log_pos) {
  int res = 0;
  while (!stop_waiting_for_update(log_pos)) {
    res = mysql_bin_log.wait_for_update();
  }
  return res;
}

void Binlog_sender::init_heartbeat_period() {
  /* Protects m_thd->user_vars. */
  mysql_mutex_lock(&m_thd->LOCK_thd_data);

  // Get user_var.
  const auto &uv = get_user_var_from_alternatives(
      m_thd, "source_heartbeat_period", "master_heartbeat_period");
  // Get value of user_var.
  bool null_value;
  m_heartbeat_period =
      std::chrono::nanoseconds(uv ? uv->val_int(&null_value) : 0);

  mysql_mutex_unlock(&m_thd->LOCK_thd_data);
}

int Binlog_sender::check_start_file() {
  char index_entry_name[FN_REFLEN];
  char *name_ptr = nullptr;
  std::string errmsg;

  if (m_start_file[0] != '\0') {
    mysql_bin_log.make_log_name(index_entry_name, m_start_file);
    name_ptr = index_entry_name;
  } else if (m_using_gtid_protocol) {
    /*
      In normal scenarios, it is not possible that Replica will
      contain more gtids than Source with respective to Source's
      UUID. But it could be possible case if Source's binary log
      is truncated(due to raid failure) or Source's binary log is
      deleted but GTID_PURGED was not set properly. That scenario
      needs to be validated, i.e., it should *always* be the case that
      Replica's gtid executed set (+retrieved set) is a subset of
      Source's gtid executed set with respective to Source's UUID.
      If it happens, dump thread will be stopped during the handshake
      with Replica (thus the Replica's I/O thread will be stopped with the
      error. Otherwise, it can lead to data inconsistency between Source
      and Replica.
    */
    global_tsid_lock->wrlock();
    const auto &server_tsid = gtid_state->get_server_tsid();
    Gtid_set gtid_executed_and_owned(
        gtid_state->get_executed_gtids()->get_tsid_map());

    // gtids = executed_gtids & owned_gtids
    if (gtid_executed_and_owned.add_gtid_set(
            gtid_state->get_executed_gtids()) != RETURN_STATUS_OK) {
      assert(0);
    }
    gtid_state->get_owned_gtids()->get_gtids(gtid_executed_and_owned);

    if (!m_exclude_gtid->is_subset_for_sid(&gtid_executed_and_owned,
                                           server_tsid.get_uuid())) {
      global_tsid_lock->unlock();
      set_fatal_error(ER_THD(m_thd, ER_REPLICA_HAS_MORE_GTIDS_THAN_SOURCE));
      return 1;
    }
    /*
      Setting GTID_PURGED (when GTID_EXECUTED set is empty i.e., when
      previous_gtids are also empty) will make binlog rotate. That
      leaves first binary log with empty previous_gtids and second
      binary log's previous_gtids with the value of gtid_purged.
      In find_first_log_not_in_gtid_set() while we search for a binary
      log whose previous_gtid_set is subset of slave_gtid_executed,
      in this particular case, server will always find the first binary
      log with empty previous_gtids which is subset of any given
      slave_gtid_executed. Thus Master thinks that it found the first
      binary log which is actually not correct and unable to catch
      this error situation. Hence adding below extra if condition
      to check the situation. Slave should know about Master's purged GTIDs.
      If Slave's GTID executed + retrieved set does not contain Master's
      complete purged GTID list, that means Slave is requesting(expecting)
      GTIDs which were purged by Master. We should let Slave know about the
      situation. i.e., throw error if slave's GTID executed set is not
      a superset of Master's purged GTID set.
      The other case, where user deleted binary logs manually
      (without using 'PURGE BINARY LOGS' command) but gtid_purged
      is not set by the user, the following if condition cannot catch it.
      But that is not a problem because in find_first_log_not_in_gtid_set()
      while checking for subset previous_gtids binary log, the logic
      will not find one and an error ER_SOURCE_HAS_PURGED_REQUIRED_GTIDS
      is thrown from there.
    */
    if (!gtid_state->get_lost_gtids()->is_subset(m_exclude_gtid)) {
      mysql_bin_log.report_missing_purged_gtids(m_exclude_gtid, errmsg);
      global_tsid_lock->unlock();
      set_fatal_error(errmsg.c_str());
      return 1;
    }
    global_tsid_lock->unlock();
    Gtid first_gtid = {0, 0};
    if (mysql_bin_log.find_first_log_not_in_gtid_set(
            index_entry_name, m_exclude_gtid, &first_gtid, errmsg)) {
      set_fatal_error(errmsg.c_str());
      return 1;
    }
    name_ptr = index_entry_name;
    /*
      find_first_log_not_in_gtid_set() guarantees the file it found has
      Previous_gtids_log_event as all following binlogs. So the variable is
      set to false which tells not to check the event again when starting to
      dump binglogs.
    */
    m_check_previous_gtid_event = false;
    /*
      If we are skipping at least the first transaction of the binlog,
      we must clear the "created" field of the FD event (set it to 0)
      to avoid cleaning up temp tables on slave.
    */
    m_gtid_clear_fd_created_flag =
        (first_gtid.sidno >= 1 && first_gtid.gno >= 1 &&
         m_exclude_gtid->contains_gtid(first_gtid));
  }

  /*
    Index entry name is saved into m_linfo. If name_ptr is NULL,
    then starts from the first file in index file.
  */

  if (mysql_bin_log.find_log_pos(&m_linfo, name_ptr, true)) {
    set_fatal_error(
        "Could not find first log file name in binary log "
        "index file");
    return 1;
  }

  if (m_start_pos < BIN_LOG_HEADER_SIZE) {
    set_fatal_error(
        "Client requested source to start replication "
        "from position < 4");
    return 1;
  }

  Binlog_read_error binlog_read_error;
  Binlog_ifile binlog_ifile(&binlog_read_error);
  if (binlog_ifile.open(m_linfo.log_file_name)) {
    set_fatal_error(binlog_read_error.get_str());
    return 1;
  }

  if (m_start_pos > binlog_ifile.length()) {
    set_fatal_error(
        "Client requested source to start replication from "
        "position > file size");
    return 1;
  }
  return 0;
}

extern TYPELIB binlog_checksum_typelib;

void Binlog_sender::init_checksum_alg() {
  DBUG_TRACE;

  m_slave_checksum_alg = mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF;

  /* Protects m_thd->user_vars. */
  mysql_mutex_lock(&m_thd->LOCK_thd_data);

  // Get user_var.
  const auto &uv = get_user_var_from_alternatives(
      m_thd, "source_binlog_checksum", "master_binlog_checksum");
  // Get value of user_var.
  if (uv && uv->ptr()) {
    m_slave_checksum_alg = static_cast<enum_binlog_checksum_alg>(
        find_type(uv->ptr(), &binlog_checksum_typelib, 1) - 1);
    assert(m_slave_checksum_alg <
           mysql::binlog::event::BINLOG_CHECKSUM_ALG_ENUM_END);
  }

  mysql_mutex_unlock(&m_thd->LOCK_thd_data);

  /*
    m_event_checksum_alg should be set to the checksum algorithm in
    Format_description_log_event. But it is used by fake_rotate_event() which
    will be called before reading any Format_description_log_event. In that
    case, m_slave_checksum_alg is set as the value of m_event_checksum_alg.
  */
  m_event_checksum_alg = m_slave_checksum_alg;
}

int Binlog_sender::fake_rotate_event(const char *next_log_file,
                                     my_off_t log_pos) {
  DBUG_TRACE;
  const char *p = next_log_file + dirname_length(next_log_file);
  size_t ident_len = strlen(p);
  size_t event_len = ident_len + LOG_EVENT_HEADER_LEN +
                     Binary_log_event::ROTATE_HEADER_LEN +
                     (event_checksum_on() ? BINLOG_CHECKSUM_LEN : 0);

  /* reset transmit packet for the fake rotate event below */
  if (reset_transmit_packet(0, event_len)) return 1;

  size_t event_offset = m_packet.length();
  m_packet.length(event_len + event_offset);
  uchar *header = pointer_cast<uchar *>(m_packet.ptr()) + event_offset;
  uchar *rotate_header = header + LOG_EVENT_HEADER_LEN;
  /*
    'when' (the timestamp) is set to 0 so that slave could distinguish between
    real and fake Rotate events (if necessary)
  */
  int4store(header, 0);
  header[EVENT_TYPE_OFFSET] = mysql::binlog::event::ROTATE_EVENT;
  int4store(header + SERVER_ID_OFFSET, server_id);
  int4store(header + EVENT_LEN_OFFSET, static_cast<uint32>(event_len));
  int4store(header + LOG_POS_OFFSET, 0);
  int2store(header + FLAGS_OFFSET, LOG_EVENT_ARTIFICIAL_F);

  int8store(rotate_header, log_pos);
  memcpy(rotate_header + Binary_log_event::ROTATE_HEADER_LEN, p, ident_len);

  if (event_checksum_on()) calc_event_checksum(header, event_len);

  return send_packet();
}

inline void Binlog_sender::calc_event_checksum(uchar *event_ptr,
                                               size_t event_len) {
  ha_checksum crc = checksum_crc32(0L, nullptr, 0);
  crc = checksum_crc32(crc, event_ptr, event_len - BINLOG_CHECKSUM_LEN);
  int4store(event_ptr + event_len - BINLOG_CHECKSUM_LEN, crc);
}

inline int Binlog_sender::reset_transmit_packet(ushort flags,
                                                size_t event_len) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("event_len: %zu, m_packet->alloced_length: %zu",
                      event_len, m_packet.alloced_length()));
  assert(m_packet.alloced_length() >= PACKET_MIN_SIZE);

  m_packet.length(0);          // size of the content
  qs_append('\0', &m_packet);  // Set this as an OK packet

  /* reserve and set default header */
  if (m_observe_transmission &&
      RUN_HOOK(binlog_transmit, reserve_header, (m_thd, flags, &m_packet))) {
    set_unknown_error("Failed to run hook 'reserve_header'");
    return 1;
  }

  /* Resizes the buffer if needed. */
  if (event_len > 0 && grow_packet(event_len)) return 1;

  DBUG_PRINT("info", ("m_packet.alloced_length: %zu (after potential "
                      "reallocation)",
                      m_packet.alloced_length()));

  return 0;
}

int Binlog_sender::send_format_description_event(File_reader &reader,
                                                 my_off_t start_pos) {
  DBUG_TRACE;
  uchar *event_ptr = nullptr;
  uint32 event_len = 0;

  if (read_event(reader, &event_ptr, &event_len)) return 1;

  DBUG_PRINT(
      "info",
      ("Looked for a Format_description_log_event, found event type %s",
       Log_event::get_type_str((Log_event_type)event_ptr[EVENT_TYPE_OFFSET])));

  if (event_ptr == nullptr ||
      event_ptr[EVENT_TYPE_OFFSET] !=
          mysql::binlog::event::FORMAT_DESCRIPTION_EVENT) {
    set_fatal_error("Could not find format_description_event in binlog file");
    return 1;
  }

  Log_event *ev = nullptr;
  Binlog_read_error binlog_read_error = binlog_event_deserialize(
      event_ptr, event_len, &reader.format_description_event(), false, &ev);
  if (binlog_read_error.has_error()) {
    set_fatal_error(binlog_read_error.get_str());
    return 1;
  }
  reader.set_format_description_event(
      dynamic_cast<Format_description_log_event &>(*ev));
  delete ev;

  assert(event_ptr[LOG_POS_OFFSET] > 0);
  m_event_checksum_alg =
      Log_event_footer::get_checksum_alg((const char *)event_ptr, event_len);

  assert(m_event_checksum_alg <
             mysql::binlog::event::BINLOG_CHECKSUM_ALG_ENUM_END ||
         m_event_checksum_alg ==
             mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF);

  /* Slave does not support checksum, but binary events include checksum */
  if (m_slave_checksum_alg == mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF &&
      event_checksum_on()) {
    set_fatal_error(
        "Replica can not handle replication events with the "
        "checksum that source is configured to log");

    LogErr(WARNING_LEVEL, ER_RPL_BINLOG_SOURCE_USES_CHECKSUM_AND_REPLICA_CANT);
    return 1;
  }

  event_ptr[FLAGS_OFFSET] &= ~LOG_EVENT_BINLOG_IN_USE_F;

  bool event_updated = false;
  if (m_using_gtid_protocol) {
    if (m_gtid_clear_fd_created_flag) {
      /*
        As we are skipping at least the first transaction of the binlog,
        we must clear the "created" field of the FD event (set it to 0)
        to avoid destroying temp tables on slave.
      */
      int4store(event_ptr + LOG_EVENT_MINIMAL_HEADER_LEN + ST_CREATED_OFFSET,
                0);
      event_updated = true;
    }
  } else if (start_pos > BIN_LOG_HEADER_SIZE) {
    /*
      If we are skipping the beginning of the binlog file based on the position
      asked by the slave, we must clear the log_pos and the created flag of the
      Format_description_log_event to be sent. Mark that this event with
      "log_pos=0", so the slave should not increment master's binlog position
      (rli->group_master_log_pos)
    */
    int4store(event_ptr + LOG_POS_OFFSET, 0);
    /*
      Set the 'created' field to 0 to avoid destroying
      temp tables on slave.
    */
    int4store(event_ptr + LOG_EVENT_MINIMAL_HEADER_LEN + ST_CREATED_OFFSET, 0);
    event_updated = true;
  }

  /* fix the checksum due to latest changes in header */
  if (event_checksum_on() && event_updated)
    calc_event_checksum(event_ptr, event_len);

  return send_packet();
}

int Binlog_sender::has_previous_gtid_log_event(File_reader &reader,
                                               bool *found) {
  uchar *event = nullptr;
  uint32 event_len;
  *found = false;

  if (read_event(reader, &event, &event_len) || event == nullptr) {
    if (reader.get_error_type() == Binlog_read_error::READ_EOF) return 0;
    set_fatal_error(log_read_error_msg(reader.get_error_type()));
    return 1;
  }

  *found = (event[EVENT_TYPE_OFFSET] ==
            mysql::binlog::event::PREVIOUS_GTIDS_LOG_EVENT);
  return 0;
}

const char *Binlog_sender::log_read_error_msg(
    Binlog_read_error::Error_type error) {
  switch (error) {
    case Binlog_read_error::BOGUS:
      return "bogus data in log event";
    case Binlog_read_error::EVENT_TOO_LARGE:
      return "log event entry exceeded max_allowed_packet; Increase "
             "max_allowed_packet on source";
    case Binlog_read_error::MEM_ALLOCATE:
      return "memory allocation failed reading log event";
    case Binlog_read_error::TRUNC_EVENT:
      return "binlog truncated in the middle of event; consider out of disk "
             "space on source";
    case Binlog_read_error::CHECKSUM_FAILURE:
      return "event read from binlog did not pass crc check";
    default:
      return Binlog_read_error(error).get_str();
  }
}

inline int Binlog_sender::read_event(File_reader &reader, uchar **event_ptr,
                                     uint32 *event_len) {
  DBUG_TRACE;

  if (reset_transmit_packet(0, 0)) return 1;
#ifndef NDEBUG
  size_t event_offset;
  event_offset = m_packet.length();
#endif

  DBUG_EXECUTE_IF("dump_thread_before_read_event", {
    const char act[] = "now wait_for signal.continue no_clear_event";
    assert(!debug_sync_set_action(m_thd, STRING_WITH_LEN(act)));
  };);

  if (reader.read_event_data(event_ptr, event_len)) {
    if (reader.get_error_type() == Binlog_read_error::READ_EOF) {
      *event_ptr = nullptr;
      *event_len = 0;
      return 0;
    }
    set_fatal_error(log_read_error_msg(reader.get_error_type()));
    return 1;
  }

  set_last_pos(reader.position());

  /*
    As we pre-allocate the buffer to store the event at reset_transmit_packet,
    the buffer should not be changed while calling read_log_event, even knowing
    that it might call functions to replace the buffer by one with the size to
    fit the event.
  */
  assert(reinterpret_cast<char *>(*event_ptr) ==
         (m_packet.ptr() + event_offset));

  DBUG_PRINT("info", ("Read event %s", Log_event::get_type_str(Log_event_type(
                                           (*event_ptr)[EVENT_TYPE_OFFSET]))));
#ifndef NDEBUG
  if (check_event_count()) return 1;
#endif
  return 0;
}

int Binlog_sender::send_heartbeat_event_v1(my_off_t log_pos) {
  DBUG_TRACE;
  const char *filename = m_linfo.log_file_name;
  const char *p = filename + dirname_length(filename);
  size_t ident_len = strlen(p);
  size_t event_len = ident_len + LOG_EVENT_HEADER_LEN +
                     (event_checksum_on() ? BINLOG_CHECKSUM_LEN : 0);

  DBUG_PRINT("info", ("log_file_name %s, log_pos %llu", p, log_pos));
  if (reset_transmit_packet(0, event_len)) return 1;

  size_t event_offset = m_packet.length();
  m_packet.length(event_len + event_offset);
  uchar *header = pointer_cast<uchar *>(m_packet.ptr()) + event_offset;

  /* Timestamp field */
  int4store(header, 0);
  header[EVENT_TYPE_OFFSET] = mysql::binlog::event::HEARTBEAT_LOG_EVENT;
  int4store(header + SERVER_ID_OFFSET, server_id);
  int4store(header + EVENT_LEN_OFFSET, event_len);
  int4store(header + LOG_POS_OFFSET, static_cast<uint32>(log_pos));
  int2store(header + FLAGS_OFFSET, 0);
  memcpy(header + LOG_EVENT_HEADER_LEN, p, ident_len);
  if (event_checksum_on()) calc_event_checksum(header, event_len);
  return send_packet_and_flush();
}
int Binlog_sender::send_heartbeat_event_v2(my_off_t log_pos) {
  DBUG_TRACE;
  DBUG_EXECUTE_IF("heartbeat_event_with_position_greater_than_4_gb",
                  { assert(log_pos > 4294967296); };);
  auto codec = mysql::binlog::event::codecs::Factory::build_codec(
      mysql::binlog::event::HEARTBEAT_LOG_EVENT_V2);
  const char *filename = m_linfo.log_file_name;
  const char *p = filename + dirname_length(filename);
  const std::string log_filename{p, strlen(p)};
  mysql::binlog::event::Heartbeat_event_v2 hb{};
  const size_t binlog_checksum_size =
      (event_checksum_on() ? BINLOG_CHECKSUM_LEN : 0);
  const size_t max_event_len =
      LOG_EVENT_HEADER_LEN + hb.max_encoding_length() + binlog_checksum_size;

  DBUG_PRINT("info", ("log_file_name %s, log_pos %llu", p, log_pos));
  if (reset_transmit_packet(0, max_event_len)) return 1;

  size_t packet_header_len = m_packet.length();
  uchar *header = pointer_cast<uchar *>(m_packet.ptr()) + packet_header_len;
  uchar *payload = header + LOG_EVENT_HEADER_LEN;

  // encode the payload of the HB event
  hb.set_log_filename(log_filename);
  hb.set_log_position(log_pos);
  auto result = codec->encode(hb, payload, hb.max_encoding_length());
  if (result.second) return 1;

  auto event_len{LOG_EVENT_HEADER_LEN + result.first + binlog_checksum_size};

  // craft the header by hand
  /* Timestamp field */
  int4store(header, 0);
  header[EVENT_TYPE_OFFSET] = mysql::binlog::event::HEARTBEAT_LOG_EVENT_V2;
  int4store(header + SERVER_ID_OFFSET, server_id);
  int4store(header + EVENT_LEN_OFFSET, event_len);
  int4store(header + LOG_POS_OFFSET, static_cast<uint32>(log_pos));
  int2store(header + FLAGS_OFFSET, 0);

  // set the effective length
  m_packet.length(event_len + packet_header_len);

  if (event_checksum_on()) calc_event_checksum(header, event_len);

  return send_packet_and_flush();
}

inline int Binlog_sender::flush_net() {
  if (DBUG_EVALUATE_IF("simulate_flush_error", 1,
                       m_thd->get_protocol()->flush())) {
    set_unknown_error("failed on flush_net()");
    return 1;
  }
  return 0;
}

inline int Binlog_sender::send_packet() {
  DBUG_TRACE;
  DBUG_PRINT("info",
             ("Sending event of type %s",
              Log_event::get_type_str(
                  (Log_event_type)m_packet.ptr()[1 + EVENT_TYPE_OFFSET])));
  // We should always use the same buffer to guarantee that the reallocation
  // logic is not broken.
  if (DBUG_EVALUATE_IF("simulate_send_error", true,
                       my_net_write(m_thd->get_protocol_classic()->get_net(),
                                    pointer_cast<const uchar *>(m_packet.ptr()),
                                    m_packet.length()))) {
    set_unknown_error("Failed on my_net_write()");
    return 1;
  }

  /* Shrink the packet if needed. */
  int ret = shrink_packet() ? 1 : 0;
  m_last_event_sent_ts = now_in_nanosecs();
  return ret;
}

inline int Binlog_sender::send_packet_and_flush() {
  return (send_packet() || flush_net());
}

inline int Binlog_sender::before_send_hook(const char *log_file,
                                           my_off_t log_pos) {
  if (m_observe_transmission &&
      RUN_HOOK(binlog_transmit, before_send_event,
               (m_thd, m_flag, &m_packet, log_file, log_pos))) {
    set_unknown_error("run 'before_send_event' hook failed");
    return 1;
  }
  return 0;
}

inline int Binlog_sender::after_send_hook(const char *log_file,
                                          my_off_t log_pos) {
  if (m_observe_transmission &&
      RUN_HOOK(binlog_transmit, after_send_event,
               (m_thd, m_flag, &m_packet, log_file, log_pos))) {
    set_unknown_error("Failed to run hook 'after_send_event'");
    return 1;
  }

  /*
    semisync after_send_event hook doesn't return and error when net error
    happens.
  */
  if (m_thd->get_protocol_classic()->get_net()->last_errno != 0) {
    set_unknown_error("Found net error");
    return 1;
  }
  return 0;
}

#ifndef NDEBUG
extern int max_binlog_dump_events;

inline int Binlog_sender::check_event_count() {
  if (max_binlog_dump_events != 0 &&
      (++m_event_count > max_binlog_dump_events)) {
    set_unknown_error("Debugging binlog dump abort");
    return 1;
  }
  return 0;
}
#endif

inline bool Binlog_sender::grow_packet(size_t extra_size) {
  DBUG_TRACE;
  size_t cur_buffer_size = m_packet.alloced_length();
  size_t cur_buffer_used = m_packet.length();
  size_t needed_buffer_size = cur_buffer_used + extra_size;

  if (extra_size > (PACKET_MAX_SIZE - cur_buffer_used))
    /*
       Not enough memory: requesting packet to be bigger than the max
       allowed - PACKET_MAX_SIZE.
    */
    return true;

  /* Grow the buffer if needed. */
  if (needed_buffer_size > cur_buffer_size) {
    size_t new_buffer_size;
    new_buffer_size =
        calc_grow_buffer_size(cur_buffer_size, needed_buffer_size);

    if (!new_buffer_size) return true;

    if (m_packet.mem_realloc(new_buffer_size)) return true;

    /*
     Calculates the new, smaller buffer, size to use the next time
     one wants to shrink the buffer.
    */
    calc_shrink_buffer_size(new_buffer_size);
  }

  return false;
}

inline bool Binlog_sender::shrink_packet() {
  DBUG_TRACE;
  bool res = false;
  size_t cur_buffer_size = m_packet.alloced_length();
  size_t buffer_used = m_packet.length();

  assert(!(cur_buffer_size < PACKET_MIN_SIZE));

  /*
     If the packet is already at the minimum size, just
     do nothing. Otherwise, check if we should shrink.
   */
  if (cur_buffer_size > PACKET_MIN_SIZE) {
    /* increment the counter if we used less than the new shrink size. */
    if (buffer_used < m_new_shrink_size) {
      m_half_buffer_size_req_counter++;

      /* Check if we should shrink the buffer. */
      if (m_half_buffer_size_req_counter == PACKET_SHRINK_COUNTER_THRESHOLD) {
        /*
         The last PACKET_SHRINK_COUNTER_THRESHOLD consecutive packets
         required less than half of the current buffer size. Lets shrink
         it to not hold more memory than we potentially need.
        */
        m_packet.shrink(m_new_shrink_size);

        /*
           Calculates the new, smaller buffer, size to use the next time
           one wants to shrink the buffer.
         */
        calc_shrink_buffer_size(m_new_shrink_size);

        /* Reset the counter. */
        m_half_buffer_size_req_counter = 0;
      }
    } else
      m_half_buffer_size_req_counter = 0;
  }
#ifndef NDEBUG
  if (res == false) {
    assert(m_new_shrink_size <= cur_buffer_size);
    assert(m_packet.alloced_length() >= PACKET_MIN_SIZE);
  }
#endif
  return res;
}

inline size_t Binlog_sender::calc_grow_buffer_size(size_t current_size,
                                                   size_t min_size) {
  /* Check that a sane minimum buffer size was requested.  */
  assert(min_size > PACKET_MIN_SIZE);
  if (min_size > PACKET_MAX_SIZE) return 0;

  /*
     Even if this overflows (PACKET_MAX_SIZE == UINT_MAX32) and
     new_size wraps around, the min_size will always be returned,
     i.e., it is a safety net.

     Also, cap new_size to PACKET_MAX_SIZE (in case
     PACKET_MAX_SIZE < UINT_MAX32).
   */
  size_t new_size = static_cast<size_t>(
      std::min(static_cast<double>(PACKET_MAX_SIZE),
               static_cast<double>(current_size * PACKET_GROW_FACTOR)));

  new_size = ALIGN_SIZE(std::max(new_size, min_size));

  return new_size;
}

void Binlog_sender::calc_shrink_buffer_size(size_t current_size) {
  size_t new_size = static_cast<size_t>(
      std::max(static_cast<double>(PACKET_MIN_SIZE),
               static_cast<double>(current_size * PACKET_SHRINK_FACTOR)));

  m_new_shrink_size = ALIGN_SIZE(new_size);
}
