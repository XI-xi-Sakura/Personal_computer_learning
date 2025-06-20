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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MY_DOUBLE2ULONGLONG_INCLUDED
#define MY_DOUBLE2ULONGLONG_INCLUDED

/**
  @file include/my_double2ulonglong.h
  Utility functions for converting between ulonglong and double.
*/

#include "my_inttypes.h"

#ifdef _WIN32
inline unsigned long long my_double2ulonglong(double d) {
  const double t = d - (double)0x8000000000000000ULL;

  if (t >= 0) return ((unsigned long long)t) + 0x8000000000000000ULL;
  return (unsigned long long)d;
}
#define double2ulonglong my_double2ulonglong
#endif /* _WIN32 */

#ifndef ulonglong2double
#define ulonglong2double(A) ((double)(ulonglong)(A))
#define my_off_t2double(A) ((double)(my_off_t)(A))
#endif
#ifndef double2ulonglong
#define double2ulonglong(A) ((ulonglong)(double)(A))
#endif

// The largest longlong that will fix into a double (LLONG_MAX is not
// exactly convertible to double, so for large double x, the test
// x <= LLONG_MAX does not guarantee x will fit in a longlong,
// and may give a compiler warning). LLONG_MIN is exact.
static constexpr double LLONG_MAX_DOUBLE = 9223372036854774784.0;

// Similar, for ulonglong.
static constexpr double ULLONG_MAX_DOUBLE = 18446744073709549568.0;

// The largest and smallest integer that is guaranteed to fix exactly into a
// double (there are 53 mantissa bits).
static constexpr int64_t MAX_EXACT_INTEGER_DOUBLE = (1ULL << 53);
static constexpr int64_t MIN_EXACT_INTEGER_DOUBLE = -MAX_EXACT_INTEGER_DOUBLE;

// Same, for float.
static constexpr int64_t MAX_EXACT_INTEGER_FLOAT = (1ULL << 23);
static constexpr int64_t MIN_EXACT_INTEGER_FLOAT = -MAX_EXACT_INTEGER_FLOAT;

#endif  // MY_DOUBLE2ULONGLONG_INCLUDED
