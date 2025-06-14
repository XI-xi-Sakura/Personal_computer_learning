/*
  Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef GROUP_REPLICATION_METADATA_INCLUDED
#define GROUP_REPLICATION_METADATA_INCLUDED

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mysqlrouter {
class MySQLSession;
}

struct GroupReplicationMember {
  enum class State {
    Online,
    Recovering,
    Unreachable,
    Offline,
    Error,
    Other,
  };
  enum class Role {
    Primary,
    Secondary,
  };
  std::string member_id;
  std::string host;
  uint16_t port;
  State state;
  Role role;
  std::string version;
};

/** Fetches the list of group replication members known to the instance of the
 * given connection.
 *
 * throws metadata_cache::metadata_error
 */
std::map<std::string, GroupReplicationMember> fetch_group_replication_members(
    mysqlrouter::MySQLSession &connection, bool &single_primary);

const char *to_string(GroupReplicationMember::State member_state);

#endif
