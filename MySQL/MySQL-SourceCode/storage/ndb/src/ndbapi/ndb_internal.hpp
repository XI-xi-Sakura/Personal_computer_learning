/*
   Copyright (c) 2007, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef NDB_INTERNAL_HPP
#define NDB_INTERNAL_HPP

#include "ndb_types.h"

class Ndb;

/**
 * This class exposes non-public funcionality to various test/utility programs
 */
class Ndb_internal {
 public:
  Ndb_internal() {}
  virtual ~Ndb_internal() {}

  static int send_event_report(bool is_poll_owner, Ndb *ndb, Uint32 *data,
                               Uint32 len);
  static void setForceShortRequests(Ndb *, bool val);
  static void set_TC_COMMIT_ACK_immediate(Ndb *, bool flag);
  static int send_dump_state_all(Ndb *, Uint32 *dumpStateCodeArray, Uint32 len);
};
#endif
