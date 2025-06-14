#ifndef RPL_SOURCE_H_INCLUDED
#define RPL_SOURCE_H_INCLUDED

/* Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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

#include <stddef.h>
#include <string>  // std::string

#include "my_hostname.h"  // HOSTNAME_LENGTH
#include "my_inttypes.h"
#include "my_thread_local.h"       // my_thread_id
#include "mysql/gtid/uuid.h"       // UUID
#include "mysql_com.h"             // USERNAME_LENGTH
#include "sql/resource_blocker.h"  // resource_blocker::User and Resource
#include "sql/sql_const.h"         // MAX_PASSWORD_LENGTH

class Gtid_set;
class String;
class THD;

extern bool server_id_supplied;
extern int max_binlog_dump_events;
extern bool opt_sporadic_binlog_dump_fail;
extern bool opt_show_replica_auth_info;

// Returns the rpl_resource
resource_blocker::Resource &get_dump_thread_resource();

struct REPLICA_INFO {
  uint32 server_id;
  uint32 rpl_recovery_rank, master_id;
  char host[HOSTNAME_LENGTH + 1];
  char user[USERNAME_LENGTH + 1];
  char password[MAX_PASSWORD_LENGTH + 1];
  uint16 port;
  my_thread_id thd_id;
  mysql::gtid::Uuid replica_uuid;
  bool valid_replica_uuid;
};

int register_replica(THD *thd, uchar *packet, size_t packet_length);
void unregister_replica(THD *thd, bool only_mine, bool need_lock_slave_list);
bool show_replicas(THD *thd);
String *get_replica_uuid(THD *thd, String *value);
bool show_binary_log_status(THD *thd);
bool show_binlogs(THD *thd);
void kill_zombie_dump_threads(THD *thd);

/**
  Process a COM_BINLOG_DUMP_GTID packet.

  This function parses the packet and then calls mysql_binlog_send.

  @param thd The dump thread.
  @param packet The COM_BINLOG_DUMP_GTID packet.
  @param packet_length The length of the packet in bytes.
  @retval true Error
  @retval false Success
*/
bool com_binlog_dump_gtid(THD *thd, char *packet, size_t packet_length);

/**
  Process a COM_BINLOG_DUMP packet.

  This function parses the packet and then calls mysql_binlog_send.

  @param thd The dump thread.
  @param packet The COM_BINLOG_DUMP packet.
  @param packet_length The length of the packet in bytes.
  @retval true Error
  @retval false Success
*/
bool com_binlog_dump(THD *thd, char *packet, size_t packet_length);

/**
  Low-level function where the dump thread iterates over the binary
  log and sends events to the slave.  This function is common for both
  COM_BINLOG_DUMP and COM_BINLOG_DUMP_GTID.

  @param thd The dump thread.

  @param log_ident The filename of the binary log, as given in the
  COM_BINLOG_DUMP[_GTID] packet.  If this is is an empty string (first
  character is '\0'), we start with the oldest binary log.

  @param pos The offset in the binary log, as given in the
  COM_BINLOG_DUMP[_GTID] packet.  This must be at least 4 and at most
  the size of the binary log file.

  @param gtid_set The gtid_set that the slave sent, or NULL if the
  protocol is COM_BINLOG_DUMP.

  @param flags flags in COM_BINLOG_DUMP[_GTID] packets.

  @note This function will start reading at the given (filename,
  offset), or from the oldest log if filename[0]==0.  It will send all
  events from that position; but if gtid_set!=NULL, it will skip all
  events in that set.
*/
void mysql_binlog_send(THD *thd, char *log_ident, my_off_t pos,
                       Gtid_set *gtid_set, uint32 flags);

bool reset_binary_logs_and_gtids(THD *thd, bool unlock_read_lock);

class user_var_entry;
/**
  Read a user variable that may exist under two different names.

  @param thd The session to read from.

  @param alt1 The first variable name alternative.

  @param alt2 The second variable name alternative.

  @retval If there exists a user variable in the current session with
  the first name, return that. Otherwise, if the second one exists,
  return that. Otherwise, return NULL.
*/
const user_var_entry *get_user_var_from_alternatives(const THD *thd,
                                                     const std::string alt1,
                                                     const std::string alt2);

#endif /* RPL_SOURCE_H_INCLUDED */
