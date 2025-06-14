/* Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

/**
  @file regexp/errors.cc

  This file contains the map from ICU warning and error codes and those in
  MySQL.
*/

#include "errors.h"

#include <cassert>
#include <cstddef>
#include <unordered_map>

#include "my_inttypes.h"     // MYF
#include "my_sys.h"          // my_error
#include "mysqld_error.h"    // Error codes
#include "unicode/utypes.h"  // UErrorCode

#include "sql/current_thd.h"
#include "sql/sql_error.h"

namespace regexp {

struct UErrorCodeHash {
  std::size_t operator()(UErrorCode t) const { return t; }
};

/**
  Map from ICU error codes to MySQL dittos. We strive to keep this list in the
  same order as the enum UErrorCode in common/unicode/utypes.h.

  ICU version 67 introduced a new implementation for '@\X'
  "Match a Grapheme Cluster". This means that our bundled version will
  return ER_REGEXP_MISSING_RESOURCE while for system ICU we return
  ER_WARN_REGEXP_USING_DEFAULT as a Note.

  TODO(tdidriks) Bundle data files in
    icudt69b/brkitr/
    icudt69l/brkitr/
  so that bundled and system ICU results are aligned.
*/
std::unordered_map<UErrorCode, int, UErrorCodeHash> error_map = {
    // ICU Error code                 MySQL error code
    {U_USING_DEFAULT_WARNING, ER_WARN_REGEXP_USING_DEFAULT},
    {U_ILLEGAL_ARGUMENT_ERROR, ER_REGEXP_ILLEGAL_ARGUMENT},
    {U_MISSING_RESOURCE_ERROR, ER_REGEXP_MISSING_RESOURCE},
    {U_FILE_ACCESS_ERROR, ER_REGEXP_MISSING_FILE},

    // Recent versions of ICU are returning "Incorrect Unicode property".
    // Map it to the same as the more generic U_ILLEGAL_ARGUMENT_ERROR.
    {U_REGEX_PROPERTY_SYNTAX, ER_REGEXP_ILLEGAL_ARGUMENT},

    {U_INDEX_OUTOFBOUNDS_ERROR, ER_REGEXP_INDEX_OUTOFBOUNDS_ERROR},
    {U_BUFFER_OVERFLOW_ERROR, ER_REGEXP_BUFFER_OVERFLOW},
    {U_REGEX_INTERNAL_ERROR, ER_REGEXP_INTERNAL_ERROR},
    {U_REGEX_RULE_SYNTAX, ER_REGEXP_RULE_SYNTAX},
    {U_REGEX_BAD_ESCAPE_SEQUENCE, ER_REGEXP_BAD_ESCAPE_SEQUENCE},
    {U_REGEX_UNIMPLEMENTED, ER_REGEXP_UNIMPLEMENTED},
    {U_REGEX_MISMATCHED_PAREN, ER_REGEXP_MISMATCHED_PAREN},
    {U_REGEX_BAD_INTERVAL, ER_REGEXP_BAD_INTERVAL},
    {U_REGEX_MAX_LT_MIN, ER_REGEXP_MAX_LT_MIN},
    {U_REGEX_INVALID_BACK_REF, ER_REGEXP_INVALID_BACK_REF},
    {U_REGEX_LOOK_BEHIND_LIMIT, ER_REGEXP_LOOK_BEHIND_LIMIT},
    {U_REGEX_MISSING_CLOSE_BRACKET, ER_REGEXP_MISSING_CLOSE_BRACKET},
    {U_REGEX_INVALID_RANGE, ER_REGEXP_INVALID_RANGE},
    {U_REGEX_STACK_OVERFLOW, ER_REGEXP_STACK_OVERFLOW},
    {U_REGEX_STOPPED_BY_CALLER, ER_QUERY_INTERRUPTED},
    {U_REGEX_TIME_OUT, ER_REGEXP_TIME_OUT},
    {U_REGEX_PATTERN_TOO_BIG, ER_REGEXP_PATTERN_TOO_BIG},
    {U_REGEX_INVALID_CAPTURE_GROUP_NAME, ER_REGEXP_INVALID_CAPTURE_GROUP_NAME},
    {U_REGEX_INVALID_FLAG, ER_REGEXP_INVALID_FLAG},
    {U_REGEX_NUMBER_TOO_BIG, ER_REGEX_NUMBER_TOO_BIG}};

bool check_icu_status(UErrorCode status, const UParseError *parse_error) {
  if (status == U_ZERO_ERROR) return false;

  int const error_code = error_map[status];

  // Add notification for select status codes.
  // E.g. we have lots of U_STRING_NOT_TERMINATED_WARNING,
  // don't generate any NOTE for those.
  if (U_SUCCESS(status)) {
    if (error_code != 0) {
      push_warning(current_thd, Sql_condition::SL_NOTE, error_code, nullptr);
    }
    return false;
  }

  if (error_code == 0) {
    /*
      If this fires, there is no translation from this ICU status code to a
      MySQL error/warning. This means an new error should then be added. In
      release builds, we just get e.g. "Got error 'U_REGEX_TIME_OUT' from
      regexp".
    */
    assert(false);
    my_error(ER_REGEXP_ERROR, MYF(0), u_errorName(status));
    return true;
  }

  // The UParseError is only written to in case of U_REGEX_RULE_SYNTAX errors.
  if (error_code == ER_REGEXP_RULE_SYNTAX && parse_error != nullptr)
    my_error(error_code, MYF(0), parse_error->line, parse_error->offset);
  else
    my_error(error_code, MYF(0));

  return true;
}

}  // namespace regexp
