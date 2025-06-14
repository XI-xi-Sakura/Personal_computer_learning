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

#ifndef PARSE_LOCATION_INCLUDED
#define PARSE_LOCATION_INCLUDED

#include <cstdlib>  // size_t

/**
  Helper class for the MY_SQL_PARSER_LTYPE
*/
struct Symbol_location {
  const char *start = nullptr;  // token start
  const char *end = nullptr;    // the 1st byte after the token

  bool is_empty() const { return length() == 0; }
  size_t length() const { return static_cast<size_t>(end - start); }
};

/**
  Bison "location" class
*/
struct MY_SQL_PARSER_LTYPE {
  Symbol_location cpp;  // token location in the preprocessed buffer
  Symbol_location raw;  // token location in the raw buffer

  bool is_empty() const { return cpp.is_empty(); }
};

/*
  Note: MY_SQL_PARSER_LTYPE doesn't overload a default constructor (as
  well an underlying Symbol_location).  OTOH if we need a
  zero-initialized POS, MY_SQL_PARSER_LTYPE or Symbol_location object,
  we can simply call POS(), MY_SQL_PARSER_LTYPE() or
  Symbol_location(): C++ does value-initialization in that case.
*/
using POS = MY_SQL_PARSER_LTYPE;

// signal Bison that we have our own MY_SQL_PARSER_LTYPE
#define MY_SQL_PARSER_LTYPE_IS_DECLARED 1

/**
  Bison calls this macro:
  1. each time a rule is matched and
  2. to compute a syntax error location.

  @param [out] Current location of the whole matched rule
  @param Rhs           locations of all right hand side elements in the rule
  @param N             number of right hand side elements in the rule
*/
#define YYLLOC_DEFAULT(Current, Rhs, N)                                   \
  do                                                                      \
    if (N) {                                                              \
      (Current).cpp.start = YYRHSLOC(Rhs, 1).cpp.start;                   \
      (Current).cpp.end = YYRHSLOC(Rhs, N).cpp.end;                       \
      (Current).raw.start = YYRHSLOC(Rhs, 1).raw.start;                   \
      (Current).raw.end = YYRHSLOC(Rhs, N).raw.end;                       \
    } else {                                                              \
      (Current).cpp.start = (Current).cpp.end = YYRHSLOC(Rhs, 0).cpp.end; \
      (Current).raw.start = (Current).raw.end = YYRHSLOC(Rhs, 0).raw.end; \
    }                                                                     \
  while (0)

#endif /* PARSE_LOCATION_INCLUDED */
