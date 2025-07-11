#ifndef SQL_REGEXP_REGEXP_FACADE_H_
#define SQL_REGEXP_REGEXP_FACADE_H_

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
  @file regexp_facade.h

  This file hides most of ICU from the Item_func_regexp subclasses.
*/

#include <stdint.h>

#include <optional>
#include <string>

#include "sql/item.h"
#include "sql/regexp/regexp_engine.h"
#include "sql_string.h"

namespace regexp {

extern CHARSET_INFO *regexp_lib_charset;

/**
  This class handles

  - Conversion to the regexp library's character set, and buffers the
    converted strings during matching.

  - Re-compilation of the regular expression in case the pattern is a field
    reference or otherwise non-constant.

  - `NULL` handling.

  - Conversion between indexing conventions. Clients of this class can use
    one-based indexing, while the classes used by this class use zero-based
    indexing.
*/
class Regexp_facade {
 public:
  /**
    Sets the pattern if called for the first time or the pattern_expr is
    non-constant. This function is meant to be called for every row in a
    command such as

      SELECT regexp_like( column, 'a+' ) FROM table;

    In this case, the client of this class may call SetPattern() for every
    row without paying any penalty, as this becomes a no-op for all
    consecutive calls. In cases such as

      SELECT regexp_like( column, regexp_column ) FROM table;

    The `regexp_column` expression is non-constant and hence we have to
    recompile the regular expression for each row.
  */
  bool SetPattern(Item *pattern_expr, uint32_t flags);

  /**
    Tries to match the subject against the compiled regular expression.

    @param subject_expr Is evaluated into a string to search.
    @param start Start position, 1-based.
    @param occurrence Which occurrence of the pattern should be searched for.

    @retval true A match was found.
    @retval false A match was not found.

    @retval nullptr Either the engine was not compiled, or subject_expr
    evaluates to NULL. This is useful for the Item_func_regexp object, since it
    doesn't have to make a special case for when the regular expression is
    NULL. Instead, the case is handled here in the facade.
  */
  std::optional<bool> Matches(Item *subject_expr, int start, int occurrence);

  /**
    Searches the subject for a match of the compiled regular expression and
    returns a position.

    @param subject_expr The string to search.
    @param start Start position, 1-based.
    @param occurrence Which occurrence of the pattern should be searched for.
    @param after_match If true, the position following the end of the match
    is returned. If false, the position before the match is returned.

    @return The first character of the match, or a null value if not found.
  */
  std::optional<int> Find(Item *subject_expr, int start, int occurrence,
                          bool after_match);

  /**
    @param subject_expr The string to search.
    @param replacement_expr The string to replace the match with.
    @param start Start position, 1-based.
    @param occurrence Which occurrence of the pattern should be searched for.
    @param[in,out] result Holds the buffer for writing the result.
  */
  String *Replace(Item *subject_expr, Item *replacement_expr, int start,
                  int occurrence, String *result);

  String *Substr(Item *subject_expr, int start, int occurrence, String *result);

  /// Delete the "engine" data structure after execution.
  void cleanup() { m_engine = nullptr; }

  /// Did any operation return a warning? For unit testing.
  bool EngineHasWarning() const {
    return m_engine != nullptr && m_engine->HasWarning();
  }

 private:
  /**
    Resets the compiled regular expression with a new string.

    @param subject_expr The new string to search.
    @param start If present, start on this code point.

    @retval false OK.
    @retval true Either there is no compiled regular expression, or the
    expression evaluated to `NULL`.
  */
  bool Reset(Item *subject_expr, int start = 1);

  /**
    Actually compiles the regular expression.
  */
  bool SetupEngine(Item *pattern_expr, uint flags);

  /**
    Converts a string position in m_current_subject.
    @param position One-based code point position.
    @return Zero-based byte position.
  */
  int ConvertCodePointToLibPosition(int position) const;

  /**
    Converts a string position in m_current_subject.
    @param position Zero-based UTF-16 position.
    @return Zero-based code point position.
  */
  int ConvertLibPositionToCodePoint(int position) const;

  /**
    Helper function for setting the result from SQL regular expression
    functions that return a string value. Depending on character sets used by
    arguments and result, this function may copy, convert or just set the
    result. In particular, it handles the special case of the BINARY character
    set being interpreted as CP-1252.

     @param str The result string from the regexp function.
     @param length Length in bytes.
     @param[out] result The result string.
     @return A pointer to the same string as the argument, or nullptr in case of
    failure.
   */
  String *AssignResult(const char *str, size_t length, String *result);

  /**
    Used for all the actual regular expression matching, search-and-replace,
    and positional and string information. If either the regular expression
    pattern or the subject is `NULL`, this pointer is empty.
  */
  unique_ptr_destroy_only<Regexp_engine> m_engine;

  /**
    ICU does not copy the subject string, so we keep the subject buffer
    here. A call to Reset() causes it to be overwritten.

    @see Regexp_engine::reset()
  */
  std::u16string m_current_subject;
};

}  // namespace regexp

#endif  // SQL_REGEXP_REGEXP_FACADE_H_
