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

#include "mysql/binlog/event/binlog_event.h"

#include "mysql/binlog/event/byteorder.h"
#include "mysql/binlog/event/statement_events.h"

#include <stdint.h>
#include <algorithm>
#include <unordered_map>

const unsigned char checksum_version_split[3] = {5, 6, 1};
const unsigned long checksum_version_product =
    (checksum_version_split[0] * 256 + checksum_version_split[1]) * 256 +
    checksum_version_split[2];

namespace mysql::binlog::event::debug {
bool debug_query_mts_corrupt_db_names = false;
bool debug_checksum_test = false;
bool debug_simulate_invalid_address = false;

}  // namespace mysql::binlog::event::debug

namespace mysql::binlog::event {

static const std::unordered_map<Log_event_type, const std::string>
    event_type_to_string = {{STOP_EVENT, "Stop"},
                            {QUERY_EVENT, "Query"},
                            {ROTATE_EVENT, "Rotate"},
                            {INTVAR_EVENT, "Intvar"},
                            {APPEND_BLOCK_EVENT, "Append_block"},
                            {DELETE_FILE_EVENT, "Delete_file"},
                            {RAND_EVENT, "RAND"},
                            {USER_VAR_EVENT, "User var"},
                            {XID_EVENT, "Xid"},
                            {FORMAT_DESCRIPTION_EVENT, "Format_desc"},
                            {TABLE_MAP_EVENT, "Table_map"},
                            {OBSOLETE_WRITE_ROWS_EVENT_V1, "Write_rows_v1"},
                            {OBSOLETE_UPDATE_ROWS_EVENT_V1, "Update_rows_v1"},
                            {OBSOLETE_DELETE_ROWS_EVENT_V1, "Delete_rows_v1"},
                            {BEGIN_LOAD_QUERY_EVENT, "Begin_load_query"},
                            {EXECUTE_LOAD_QUERY_EVENT, "Execute_load_query"},
                            {INCIDENT_EVENT, "Incident"},
                            {IGNORABLE_LOG_EVENT, "Ignorable"},
                            {ROWS_QUERY_LOG_EVENT, "Rows_query"},
                            {WRITE_ROWS_EVENT, "Write_rows"},
                            {UPDATE_ROWS_EVENT, "Update_rows"},
                            {DELETE_ROWS_EVENT, "Delete_rows"},
                            {GTID_LOG_EVENT, "Gtid"},
                            {ANONYMOUS_GTID_LOG_EVENT, "Anonymous_Gtid"},
                            {PREVIOUS_GTIDS_LOG_EVENT, "Previous_gtids"},
                            {HEARTBEAT_LOG_EVENT, "Heartbeat"},
                            {TRANSACTION_CONTEXT_EVENT, "Transaction_context"},
                            {VIEW_CHANGE_EVENT, "View_change"},
                            {XA_PREPARE_LOG_EVENT, "XA_prepare"},
                            {PARTIAL_UPDATE_ROWS_EVENT, "Update_rows_partial"},
                            {TRANSACTION_PAYLOAD_EVENT, "Transaction_payload"},
                            {GTID_TAGGED_LOG_EVENT, "Gtid_tagged_log_event"},
                            {UNKNOWN_EVENT, "Unknown"}};

const std::string &get_event_type_as_string(Log_event_type type) {
  try {
    return event_type_to_string.at(type);
  } catch (const std::out_of_range &) {
    return event_type_to_string.at(UNKNOWN_EVENT);
  }
}

Log_event_footer::Log_event_footer(Event_reader &reader,
                                   Log_event_type event_type,
                                   const Format_description_event *fde)
    : checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF) {
  BAPI_ENTER("Log_event_footer::Log_event_footer(Event_reader& , ...)");
  if (event_type == mysql::binlog::event::FORMAT_DESCRIPTION_EVENT) {
    /* This is a FDE. We will get the checksum_alg from its buffer directly. */
    checksum_alg = Log_event_footer::get_checksum_alg(reader);
  } else {
    /* This is an ordinary event other than FDE. Using FDE's checksum_alg. */
    checksum_alg = fde->footer()->checksum_alg;

    if (checksum_alg != mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF &&
        checksum_alg != mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF)
      reader.shrink_limit(BINLOG_CHECKSUM_LEN);
  }
  BAPI_VOID_RETURN;
}

enum_binlog_checksum_alg Log_event_footer::get_checksum_alg(
    Event_reader &reader) {
  enum_binlog_checksum_alg alg =
      get_checksum_alg(reader.buffer(), reader.length());
  if (alg != mysql::binlog::event::BINLOG_CHECKSUM_ALG_UNDEF)
    reader.shrink_limit(BINLOG_CHECKSUM_LEN);
  return alg;
}

/**
   The method returns the checksum algorithm used to checksum the binary log.
   For MySQL server versions < 5.6, the algorithm is undefined. For the higher
   versions, the type is decoded from the FORMAT_DESCRIPTION_EVENT.

   @param buf buffer holding serialized FD event
   @param len netto (possible checksum is stripped off) length of the event buf

   @return  the version-safe checksum alg descriptor where zero
            designates no checksum, 255 - the orginator is
            checksum-unaware (effectively no checksum) and the actual
            [1-254] range alg descriptor.
*/
enum_binlog_checksum_alg Log_event_footer::get_checksum_alg(const char *buf,
                                                            unsigned long len) {
  BAPI_ENTER("Log_event_footer::get_checksum_alg(const char*, unsigned long)");
  enum_binlog_checksum_alg ret = BINLOG_CHECKSUM_ALG_UNDEF;
  char version[ST_SERVER_VER_LEN];
  unsigned char version_split[3];
  BAPI_ASSERT(buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT);
  if (len > LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET + 1) {
    uint8_t common_header_len =
        buf[LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET];
    if (len >=
        static_cast<unsigned long>(common_header_len + ST_SERVER_VER_OFFSET +
                                   ST_SERVER_VER_LEN)) {
      memcpy(version, buf + common_header_len + ST_SERVER_VER_OFFSET,
             ST_SERVER_VER_LEN);
      version[ST_SERVER_VER_LEN - 1] = 0;

      do_server_version_split(version, version_split);
      if (version_product(version_split) < checksum_version_product)
        ret = BINLOG_CHECKSUM_ALG_UNDEF;
      else {
        size_t checksum_alg_offset =
            len - (BINLOG_CHECKSUM_ALG_DESC_LEN + BINLOG_CHECKSUM_LEN);
        ret =
            static_cast<enum_binlog_checksum_alg>(*(buf + checksum_alg_offset));
      }
    }
  }
  BAPI_RETURN(ret);
}

/**
  Tests the checksum algorithm used for the binary log, and asserts in case
  if the checksum algorithm is invalid.

  @param   event_buf       point to the buffer containing serialized event
  @param   event_len       length of the event accounting possible
                           checksum alg
  @param   alg             checksum algorithm used for the binary log

  @retval  true            if test fails
  @retval  false           as success
*/
bool Log_event_footer::event_checksum_test(unsigned char *event_buf,
                                           unsigned long event_len,
                                           enum_binlog_checksum_alg alg) {
  bool res = false;
  unsigned short flags = 0;  // to store in FD's buffer flags orig value

  if (alg != BINLOG_CHECKSUM_ALG_OFF && alg != BINLOG_CHECKSUM_ALG_UNDEF) {
    uint32_t incoming;
    uint32_t computed;

    if (event_buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT) {
#ifndef NDEBUG
      unsigned char fd_alg = event_buf[event_len - BINLOG_CHECKSUM_LEN -
                                       BINLOG_CHECKSUM_ALG_DESC_LEN];
#endif
      /*
        FD event is checksummed and therefore verified w/o
        the binlog-in-use flag.
      */
      memcpy(&flags, event_buf + FLAGS_OFFSET, sizeof(flags));
      flags = le16toh(flags);
      if (flags & LOG_EVENT_BINLOG_IN_USE_F)
        event_buf[FLAGS_OFFSET] &= ~LOG_EVENT_BINLOG_IN_USE_F;
        /*
           The only algorithm currently is CRC32. Zero indicates
           the binlog file is checksum-free *except* the FD-event.
        */
#ifndef NDEBUG
      BAPI_ASSERT(fd_alg == BINLOG_CHECKSUM_ALG_CRC32 || fd_alg == 0);
#endif
      BAPI_ASSERT(alg == BINLOG_CHECKSUM_ALG_CRC32);
      /*
        Compile time guard to watch over the max number of alg
      */
      static_assert(BINLOG_CHECKSUM_ALG_ENUM_END <= 0x80, "");
    }
    memcpy(&incoming, event_buf + event_len - BINLOG_CHECKSUM_LEN,
           sizeof(incoming));
    incoming = le32toh(incoming);

    computed = checksum_crc32(0L, nullptr, 0);
    /* checksum the event content but not the checksum part itself */
    computed = mysql::binlog::event::checksum_crc32(
        computed, (const unsigned char *)event_buf,
        event_len - BINLOG_CHECKSUM_LEN);

    if (flags != 0) {
      /* restoring the orig value of flags of FD */
      BAPI_ASSERT(event_buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT);
      event_buf[FLAGS_OFFSET] = static_cast<unsigned char>(flags);
    }

    res = !(computed == incoming);
  }
#ifndef NDEBUG
  if (mysql::binlog::event::debug::debug_checksum_test) return true;
#endif
  return res;
}

Log_event_header::Log_event_header(Event_reader &reader)
    : data_written(0), log_pos(0), m_is_valid(false) {
  BAPI_ENTER("Log_event_header::Log_event_header(Event_reader &)");

  /*
   The caller should always pass an Event_reader with a buffer allowing to
   read at least LOG_EVENT_MINIMAL_HEADER_LEN bytes.
  */
  BAPI_ASSERT(reader.can_read(LOG_EVENT_MINIMAL_HEADER_LEN));

  /**
    @verbatim
    The first 19 bytes in the header is as follows:
      +============================================+
      | member_variable               offset : len |
      +============================================+
      | when.tv_sec                        0 : 4   |
      +--------------------------------------------+
      | type_code       EVENT_TYPE_OFFSET(4) : 1   |
      +--------------------------------------------+
      | server_id       SERVER_ID_OFFSET(5)  : 4   |
      +--------------------------------------------+
      | data_written    EVENT_LEN_OFFSET(9)  : 4   |
      +--------------------------------------------+
      | log_pos           LOG_POS_OFFSET(13) : 4   |
      +--------------------------------------------+
      | flags               FLAGS_OFFSET(17) : 2   |
      +--------------------------------------------+
      | extra_headers                     19 : x-19|
      +============================================+
    @endverbatim
   */
  BAPI_ASSERT(reader.position() == 0);
  when.tv_sec = reader.read<uint32_t>();
  when.tv_usec = 0;

  BAPI_ASSERT(reader.position() == EVENT_TYPE_OFFSET);
  type_code = static_cast<Log_event_type>(reader.read<unsigned char>());

  BAPI_ASSERT(reader.position() == SERVER_ID_OFFSET);
  unmasked_server_id = reader.read<uint32_t>();

  BAPI_ASSERT(reader.position() == EVENT_LEN_OFFSET);
  data_written = reader.read<uint64_t>(4);
  reader.set_length(data_written);

  BAPI_ASSERT(reader.position() == LOG_POS_OFFSET);
  log_pos = reader.read<uint64_t>(4);

  BAPI_ASSERT(reader.position() == FLAGS_OFFSET);
  flags = reader.read<uint16_t>();

  set_is_valid(!reader.has_error() &&
               (type_code < ENUM_END_EVENT || flags & LOG_EVENT_IGNORABLE_F));

  BAPI_VOID_RETURN;
}

const Event_decoding_error &Log_event_header::get_decoding_error() const {
  return m_decoding_error;
}

void Log_event_header::set_decoding_error(
    const Event_decoding_error &decoding_error) {
  m_decoding_error = decoding_error;
}

Binary_log_event::Binary_log_event(const char **buf,
                                   const Format_description_event *fde)
    : m_reader(*buf, LOG_EVENT_MINIMAL_HEADER_LEN), m_header(m_reader) {
  BAPI_ENTER("Binary_log_event::Binary_log_event(const char **, ...)");
  /*
    m_reader cursor is advanced in Log_event_reader constructor to after the
    event common header.
  */
  m_footer = Log_event_footer(m_reader, m_header.type_code, fde);
  BAPI_VOID_RETURN;
}

/*
  The destructor is pure virtual to prevent instantiation of the class.
*/
Binary_log_event::~Binary_log_event() = default;

/**
  This event type should never occur. It is never written to a binary log.
  If an event is read from a binary log that cannot be recognized as something
  else, it is treated as Unknown_event.

  @param buf  Contains the serialized event.
  @param fde  An FDE required by Binary_log_event constructor.
*/
Unknown_event::Unknown_event(const char *buf,
                             const Format_description_event *fde)
    : Binary_log_event(&buf, fde) {
  BAPI_ENTER("Unknown_event::Unknown_event(const char *, ...)");
  BAPI_VOID_RETURN;
}
#ifndef HAVE_MYSYS
void Binary_log_event::print_event_info(std::ostream &) {}
void Binary_log_event::print_long_info(std::ostream &) {}
/**
  This method is used by the binlog_browser to print short and long
  information about the event. Since the body of Stop_event is empty
  the relevant information contains only the timestamp.
  Please note this is different from the print_event_info methods
  used by mysqlbinlog.cc.

  @param info std output stream to which the event data is appended.
*/
void Stop_event::print_long_info(std::ostream &info) {
  info << "Timestamp: " << header()->when.tv_sec;
  this->print_event_info(info);
}

void Unknown_event::print_event_info(std::ostream &info) {
  info << "Unhandled event";
}

void Unknown_event::print_long_info(std::ostream &info) {
  info << "Timestamp: " << header()->when.tv_sec;
  this->print_event_info(info);
}

#endif
}  // end namespace mysql::binlog::event
