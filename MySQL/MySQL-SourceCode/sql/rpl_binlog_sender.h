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

#ifndef DEFINED_RPL_BINLOG_SENDER
#define DEFINED_RPL_BINLOG_SENDER

#include <string.h>
#include <sys/types.h>
#include <chrono>

#include "my_inttypes.h"
#include "my_io.h"
#include "mysql/binlog/event/binlog_event.h"
#include "mysql_com.h"
#include "mysqld_error.h"  // ER_*
#include "sql/binlog.h"    // Log_info
#include "sql/binlog_reader.h"
#include "sql/rpl_gtid.h"
#include "sql/sql_error.h"  // Diagnostics_area

class String;
class THD;

/**
  The major logic of dump thread is implemented in this class. It sends
  required binlog events to clients according to their requests.
*/
class Binlog_sender {
  class Event_allocator;
  typedef Basic_binlog_file_reader<Binlog_ifile, Binlog_event_data_istream,
                                   Binlog_event_object_istream, Event_allocator>
      File_reader;

 public:
  Binlog_sender(THD *thd, const char *start_file, my_off_t start_pos,
                Gtid_set *exclude_gtids, uint32 flag);

  ~Binlog_sender() = default;

  /**
    It checks the dump request and sends events to the client until it finish
    all events(for mysqlbinlog) or encounters an error.
  */
  void run();

  /**
    Sets the value of the previously processed event.

    @param type The last processed event type.
  */
  inline void set_prev_event_type(mysql::binlog::event::Log_event_type type) {
    m_prev_event_type = type;
  }

 private:
  /**
    Checks whether thread should continue awaiting new events
    @param log_pos Last processed (sent) event id
  */
  bool stop_waiting_for_update(my_off_t log_pos) const;

  THD *m_thd;
  String &m_packet;

  /* Requested start binlog file and position */
  const char *m_start_file;
  my_off_t m_start_pos;

  /*
    For COM_BINLOG_DUMP_GTID, It may include a GTID set. All events in the set
    should not be sent to the client.
  */
  Gtid_set *m_exclude_gtid;
  bool m_using_gtid_protocol;
  bool m_check_previous_gtid_event;
  bool m_gtid_clear_fd_created_flag;

  /* The binlog file it is reading */
  Log_info m_linfo;

  mysql::binlog::event::enum_binlog_checksum_alg m_event_checksum_alg;
  mysql::binlog::event::enum_binlog_checksum_alg m_slave_checksum_alg;
  std::chrono::nanoseconds m_heartbeat_period;
  std::chrono::nanoseconds m_last_event_sent_ts;
  /*
    For mysqlbinlog(server_id is 0), it will stop immediately without waiting
    if it already reads all events.
  */
  bool m_wait_new_events;

  Diagnostics_area m_diag_area;
  char m_errmsg_buf[MYSQL_ERRMSG_SIZE];
  const char *m_errmsg;
  int m_errno;
  /*
    The position of the event it reads most recently is stored. So it can report
    the exact position after where an error happens.

    m_last_file will point to m_info.log_file_name, if it is same to
    m_info.log_file_name. Otherwise the file name is copied to m_last_file_buf
    and m_last_file will point to it.
  */
  char m_last_file_buf[FN_REFLEN];
  const char *m_last_file;
  my_off_t m_last_pos;

  /*
    Needed to be able to evaluate if buffer needs to be resized (shrunk).
  */
  ushort m_half_buffer_size_req_counter;

  /*
   * The size of the buffer next time we shrink it.
   * This variable is updated once every time we shrink or grow the buffer.
   */
  size_t m_new_shrink_size;

  /*
     Max size of the buffer is 4GB (UINT_MAX32). It is UINT_MAX32 since the
     threshold is set to (@c Log_event::read_log_event):

       max(max_allowed_packet,
           binlog_row_event_max_size + MAX_LOG_EVENT_HEADER)

     - binlog_row_event_max_size is defined as an unsigned long,
       thence in theory row events can be bigger than UINT_MAX32.

     - max_allowed_packet is set to mysql::binlog::event::max_log_event_size
       which is in turn defined as 1GB (i.e., 1024*1024*1024). (@c
       Binlog_sender::init()).

     Therefore, anything bigger than UINT_MAX32 is not loadable into the
     packet, thus we set the limit to 4GB (which is the value for UINT_MAX32,
     @c PACKET_MAXIMUM_SIZE).

   */
  const static uint32 PACKET_MAX_SIZE;

  /*
   * After these consecutive times using less than half of the buffer
   * the buffer is shrunk.
   */
  const static ushort PACKET_SHRINK_COUNTER_THRESHOLD;

  /**
   * The minimum size of the buffer.
   */
  const static uint32 PACKET_MIN_SIZE;

  /**
   * How much to grow the buffer each time we need to accommodate more bytes
   * than it currently can hold.
   */
  const static float PACKET_GROW_FACTOR;

  /**
   * The dual of PACKET_GROW_FACTOR. How much to shrink the buffer each time
   * it is deemed to being underused.
   */
  const static float PACKET_SHRINK_FACTOR;

  uint32 m_flag;
  /*
    It is true if any plugin requires to observe the transmission for each
    event. And HOOKs(reserve_header, before_send and after_send) are called when
    transmitting each event. Otherwise, it is false and HOOKs are not called.
  */
  bool m_observe_transmission;

  /* It is true if transmit_start hook is called. If the hook is not called
   * it will be false.
   */
  bool m_transmit_started;
  /**
    Type of the previously processed event.
  */
  mysql::binlog::event::Log_event_type m_prev_event_type;
  /*
    It initializes the context, checks if the dump request is valid and
    if binlog status is correct.
  */
  void init();
  void cleanup();
  void init_heartbeat_period();
  void init_checksum_alg();
  /** Check if the requested binlog file and position are valid */
  int check_start_file();
  /** Transform read error numbers to error messages. */
  const char *log_read_error_msg(Binlog_read_error::Error_type error);

  /**
    It dumps a binlog file. Events are read and sent one by one. If it need
    to wait for new events, it will wait after already reading all events in
    the active log file.

    @param[in] reader     File_reader of binlog will be sent
    @param[in] start_pos  Position requested by the slave's IO thread.
                          Only the events after the position are sent.

    @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_binlog(File_reader &reader, my_off_t start_pos);

  /**
    It sends some events in a binlog file to the client.

    @param[in] reader     File_reader of binlog will be sent
    @param[in] end_pos    Only the events before end_pos are sent

    @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_events(File_reader &reader, my_off_t end_pos);

  /**
    It gets the end position of the binlog file.

    @param[in] reader   File_reader of binlog will be checked
    @returns Pair :
             - Binlog end position - will be set to the end position
             of the reading binlog file. If this is an inactive file,
             it will be set to 0.
             - Status code - 0 (success), 1 (end execution)
    @note Function is responsible for flushing the net buffer, flushes before
          waiting and before returning 1 which means end of the execution
          (normal/error)
  */
  std::pair<my_off_t, int> get_binlog_end_pos(File_reader &reader);

  /**
     It checks if a binlog file has Previous_gtid_log_event

     @param[in]  reader     File_reader of binlog will be checked
     @param[out] found      Found Previous_gtid_log_event or not

     @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int has_previous_gtid_log_event(File_reader &reader, bool *found);

  /**
    It sends a faked rotate event which does not exist physically in any
    binlog to the slave. It contains the name of the binlog we are going to
    send to the slave.

    Faked rotate event is required in a few cases, so slave can know which
    binlog the following events are from.

  - The binlog file slave requested is Empty. E.g.
    "CHANGE REPLICATION SOURCE TO SOURCE_LOG_FILE='', SOURCE_LOG_POS=4", etc.

  - The position slave requested is exactly the end of a binlog file.

  - Previous binlog file does not include a rotate event.
    It happens when server is shutdown and restarted.

  - The previous binary log was GTID-free (does not contain a
    Previous_gtids_log_event) and the slave is connecting using
    the GTID protocol.

    @param[in] next_log_file  The name of the binlog file will be sent after
                              the rotate event.
    @param[in] log_pos        The start position of the binlog file.

    @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int fake_rotate_event(const char *next_log_file, my_off_t log_pos);

  /**
     When starting to dump a binlog file, Format_description_log_event
     is read and sent first. If the requested position is after
     Format_description_log_event, log_pos field in the first
     Format_description_log_event has to be set to 0. So the slave
     will not increment its master's binlog position.

     @param[in] reader    File_reader of the binlog will be dumpped
     @param[in] start_pos Position requested by the slave's IO thread.
                          Only the events after the position are sent.

     @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_format_description_event(File_reader &reader, my_off_t start_pos);
  /**
     It sends a heartbeat to the client.

     @param[in] log_pos  The log position that events before it are sent.

     @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_heartbeat_event_v1(my_off_t log_pos);
  /**
     It sends a heartbeat to the client, for the cases when the
     flag USE_HEARTBEAT_EVENT_V2 is set.
     @param[in] log_pos  The log position that events before it are sent.

     @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_heartbeat_event_v2(my_off_t log_pos);
  /**
     Checks if the heartbeat_version flag is set or not, and call the correct
     send_heartbeat_method accordingly.

     @param[in] log_pos  The log position that events before it are sent.

     @return It returns 0 if succeeds, otherwise 1 is returned.
   */
  int send_heartbeat_event(my_off_t log_pos);
  /**
     It reads an event from binlog file. this function can set event_ptr either
     a valid buffer pointer or nullptr. nullptr means it arrives at the end of
     the binlog file if no error happens.

     @param[in] reader        File_reader of the binlog file.
     @param[out] event_ptr    The buffer used to store the event.
     @param[out] event_len    Length of the event.

     @retval 0 Succeed
     @retval 1 Fail
  */
  int read_event(File_reader &reader, uchar **event_ptr, uint32 *event_len);
  /**
    Check if it is allowed to send this event type.

    The following are disallowed:
    - GTID_MODE=ON and type==ANONYMOUS_GTID_LOG_EVENT
    - AUTO_POSITION=1 and type==ANONYMOUS_GTID_LOG_EVENT
    - GTID_MODE=OFF and type==GTID_LOG_EVENT

    @param type The event type.
    @param log_file The binary log file (used in error messages).
    @param log_pos The binary log position (used in error messages).

    @retval true The event is not allowed. In this case, this function
    calls set_fatal_error().
    @retval false The event is allowed.
  */
  bool check_event_type(mysql::binlog::event::Log_event_type type,
                        const char *log_file, my_off_t log_pos);
  /**
    It checks if the event is in m_exclude_gtid.

    Clients may request to exclude some GTIDs. The events include in the GTID
    groups will be skipped. We call below events sequence as a goup,
    Gtid_log_event
    BEGIN
    ...
    COMMIT or ROLLBACK

    or
    Gtid_log_event
    DDL statement

    @param[in] event_ptr  Buffer of the event
    @param[in] in_exclude_group  If it is in a exclude group

    @return It returns true if it should be skipped, otherwise false is turned.
  */
  bool skip_event(const uchar *event_ptr, bool in_exclude_group);

  void calc_event_checksum(uchar *event_ptr, size_t event_len);
  int flush_net();
  int send_packet();
  int send_packet_and_flush();
  int before_send_hook(const char *log_file, my_off_t log_pos);
  int after_send_hook(const char *log_file, my_off_t log_pos);
  /*
    Reset the thread transmit packet buffer for event sending.

    This function reserves the bytes for event transmission, and
    should be called before storing the event data to the packet buffer.

    @param[in] flags      The flag used in reset_transmit hook.
    @param[in] event_len  If the caller already knows the event length, then
                          it can pass this value so that reset_transmit_packet
                          already reallocates the buffer if needed. Otherwise,
                          if event_len is 0, then the caller needs to extend
                          the buffer itself.
  */
  int reset_transmit_packet(ushort flags, size_t event_len = 0);

  /**
    It waits until receiving an update_cond signal. It will send heartbeat
    periodically if m_heartbeat_period is set.

    @param[in] log_pos  The end position of the last event it already sent.
    It is required by heartbeat events.

    @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int wait_new_events(my_off_t log_pos);
  int wait_with_heartbeat(my_off_t log_pos);
  int wait_without_heartbeat(my_off_t log_pos);

#ifndef NDEBUG
  /* It is used to count the events that have been sent. */
  int m_event_count;
  /*
    It aborts dump thread with an error if m_event_count exceeds
    max_binlog_dump_events.
  */
  int check_event_count();
#endif

  inline bool has_error() { return m_errno != 0; }
  inline void set_error(int errorno, const char *errmsg) {
    snprintf(m_errmsg_buf, sizeof(m_errmsg_buf), "%.*s", MYSQL_ERRMSG_SIZE - 1,
             errmsg);
    m_errmsg = m_errmsg_buf;
    m_errno = errorno;
  }

  inline void set_unknown_error(const char *errmsg) {
    set_error(ER_UNKNOWN_ERROR, errmsg);
  }

  inline void set_fatal_error(const char *errmsg) {
    set_error(ER_SOURCE_FATAL_ERROR_READING_BINLOG, errmsg);
  }

  inline bool is_fatal_error() {
    return m_errno == ER_SOURCE_FATAL_ERROR_READING_BINLOG;
  }

  inline bool event_checksum_on() {
    return m_event_checksum_alg >
               mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF &&
           m_event_checksum_alg <
               mysql::binlog::event::BINLOG_CHECKSUM_ALG_ENUM_END;
  }

  inline void set_last_pos(my_off_t log_pos) {
    m_last_file = m_linfo.log_file_name;
    m_last_pos = log_pos;
  }

  inline void set_last_file(const char *log_file) {
    strcpy(m_last_file_buf, log_file);
    m_last_file = m_last_file_buf;
  }

  /**
   * This function SHALL grow the buffer of the packet if needed.
   *
   * If the buffer used for the packet is large enough to accommodate
   * the requested extra bytes, then this function does not do anything.
   *
   * On the other hand, if the requested size is bigger than the available
   * free bytes in the buffer, the buffer is extended by a constant factor
   * (@c PACKET_GROW_FACTOR).
   *
   * @param extra_size  The size in bytes that the caller wants to add to the
   * buffer.
   * @return true if an error occurred, false otherwise.
   */
  bool grow_packet(size_t extra_size);

  /**
   * This function SHALL shrink the size of the buffer used.
   *
   * If less than half of the buffer was used in the last N
   * (@c PACKET_SHRINK_COUNTER_THRESHOLD) consecutive times this function
   * was called, then the buffer gets shrunk by a constant factor
   * (@c PACKET_SHRINK_FACTOR).
   *
   * The buffer is never shrunk less than a minimum size (@c PACKET_MIN_SIZE).
   */
  bool shrink_packet();

  /**
   Helper function to recalculate a new size for the growing buffer.

   @param current_size The baseline (for instance, the current buffer size).
   @param min_size The resulting buffer size, needs to be at least as large
                   as this parameter states.
   @return The new buffer size, or 0 in the case of an error.
  */
  size_t calc_grow_buffer_size(size_t current_size, size_t min_size);

  /**
   Helper function to recalculate the new size for the m_new_shrink_size.

   @param current_size The baseline (for instance, the current buffer size).
  */
  void calc_shrink_buffer_size(size_t current_size);
};

#endif  // DEFINED_RPL_BINLOG_SENDER
