#ifndef SQL_JOIN_TYPE_H
#define SQL_JOIN_TYPE_H

/* Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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

// Used to describe what kind of join an iterator is executing.
enum class JoinType {
  INNER,
  /// Left outer join.
  OUTER,

  /// Left antijoin, i.e.:
  /// SELECT projection FROM left_releation WHERE NOT EXISTS
  /// (SELECT 1 FROM right_releation WHERE predicate)
  ANTI,

  /// Left semijoin, i.e.:
  /// SELECT projection FROM left_releation WHERE EXISTS
  /// (SELECT 1 FROM right_releation WHERE predicate)
  SEMI,
  FULL_OUTER
};

#endif  // SQL_JOIN_TYPE_H
