/* Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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

#ifndef CONNECTION_DELAY_API_H
#define CONNECTION_DELAY_API_H

#include <include/my_inttypes.h>

namespace connection_control {

/* constants/variables defined in connection_delay.cc */

extern const int64 MIN_THRESHOLD;
extern const int64 DISABLE_THRESHOLD;
extern const int64 MAX_THRESHOLD;

extern const int64 MIN_DELAY;
extern const int64 MAX_DELAY;

/** Functions being used by connection_control.cc */

class Connection_event_coordinator;

void init_connection_delay_event(Connection_event_coordinator *coordinator);
void deinit_connection_delay_event();

}  // namespace connection_control
#endif  // !CONNECTION_DELAY_API_H
