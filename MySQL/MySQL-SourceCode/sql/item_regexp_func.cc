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
  @file item_regexp_func.cc

  Implementation of the regular expression function object. This file deals
  only with the function objects and their place in the Item hierarchy. All
  interactions with the regular expression library are done through a facade
  object, which is instantiated in fix_fields() and deleted in cleanup().
*/

#include "sql/item_regexp_func.h"

#include <optional>

#include "my_dbug.h"
#include "mysql/strings/m_ctype.h"
#include "mysql_com.h"      // MAX_BLOB_WIDTH
#include "sql/item_func.h"  // agg_arg_charsets_for_comparison()
#include "sql/sql_class.h"  // THD
#include "sql/sql_lex.h"    // Disable_semijoin_flattening

/**
  Transforms a textual option string from the user to a bitmask of ICU flags.
  The match_parameter is a string of characters where each character controls
  some characteristic of the regular expression matching:

  - `'c'` means case-sensitive matching.
  - `'i'` means case-insensitive matching.
  - `'m'` activates multi-line matching.
  - `'n'` lets the period match the newline character.
  - `'u'` means that only UNIX line endings are recognized as line endings.

  @param options_string The options to parse.
  @param case_sensitive_is_default Whether case-sensitive matching is the
  default.
  @param[out] flags These flags will be updated (overridden) with the
  values of flags in options_string.

  @retval false String parsed successfully and *flags is updated accordingly.
  @retval true String could not be parsed, *flags is untouched.
*/
static bool ParseRegexpOptions(const std::string &options_string,
                               bool case_sensitive_is_default,
                               uint32_t *flags) {
  uint32_t result = case_sensitive_is_default ? 0 : UREGEX_CASE_INSENSITIVE;
  for (char c : options_string) {
    switch (c) {
      case 'c':
        result &= ~UREGEX_CASE_INSENSITIVE;
        break;
      case 'i':
        result |= UREGEX_CASE_INSENSITIVE;
        break;
      case 'm':
        result |= UREGEX_MULTILINE;
        break;
      case 'n':
        result |= UREGEX_DOTALL;
        break;
      case 'u':
        result |= UREGEX_UNIX_LINES;
        break;
      default:
        return true;
    }
  }
  *flags = result;
  return false;
}

static bool is_binary_string(Item *item) {
  return item->data_type() == MYSQL_TYPE_VARCHAR &&
         item->type() != Item::PARAM_ITEM &&
         item->type() != Item::NULL_ITEM &&  // NULL literals appear to have the
                                             // binary charset.
         item->charset_for_protocol() == &my_charset_bin;
}

static bool is_binary_compatible(Item *item) {
  if ((item->data_type() == MYSQL_TYPE_BLOB ||
       item->data_type() == MYSQL_TYPE_STRING ||
       item->data_type() == MYSQL_TYPE_VARCHAR) &&
      item->charset_for_protocol() != &my_charset_bin)
    return false;
  return true;
}

bool Item_func_regexp::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 2)) return true;

  const CHARSET_INFO *subject_charset = subject()->charset_for_protocol();
  const CHARSET_INFO *pattern_charset = pattern()->charset_for_protocol();

  if ((is_binary_string(subject()) && !is_binary_compatible(pattern())) ||
      (is_binary_string(pattern()) && !is_binary_compatible(subject()))) {
    my_error(ER_CHARACTER_SET_MISMATCH, myf(0), subject_charset->m_coll_name,
             pattern_charset->m_coll_name, func_name());
    return error_bool();
  }

  return agg_arg_charsets_for_comparison(collation, args, 2);
}

bool Item_func_regexp::fix_fields(THD *thd, Item **arguments) {
  if (Item_func::fix_fields(thd, arguments)) return true;

  m_facade = make_unique_destroy_only<regexp::Regexp_facade>(thd->mem_root);

  fixed = true;

  // There may be errors evaluating arguments.
  return thd->is_error();
}

void Item_func_regexp::cleanup() {
  if (m_facade != nullptr) m_facade->cleanup();
  Item_func::cleanup();
}

bool Item_func_regexp::set_pattern() {
  auto mp = match_parameter();
  if (!mp.has_value()) return true;

  bool is_case_sensitive =
      (((collation.collation->state & (MY_CS_CSSORT | MY_CS_BINSORT)) != 0));

  uint32_t icu_flags = 0;  // Avoids compiler warning on gcc 4.8.5.
  // match_parameter overrides coercion type.
  if (ParseRegexpOptions(mp.value(), is_case_sensitive, &icu_flags)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }

  return m_facade->SetPattern(pattern(), icu_flags);
}

bool Item_func_regexp_instr::fix_fields(THD *thd, Item **arguments) {
  if (Item_func_regexp::fix_fields(thd, arguments)) return true;

  const int the_index = retopt_arg_pos();
  if (the_index != -1 && arg_count >= static_cast<uint>(the_index) + 1) {
    if (!args[the_index]->const_item()) {
      return false;
    }
  }

  std::optional<int> retopt = return_option();
  if (current_thd->is_error()) return true;

  if (retopt.has_value() && retopt != 0 && retopt != 1) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0),
             "regexp_instr: return_option must be 1 or 0.");
    return true;
  }

  return false;
}

bool Item_func_regexp_instr::resolve_type(THD *thd) {
  if (Item_func_regexp::resolve_type(thd)) return true;
  if (param_type_is_default(thd, 2, 4, MYSQL_TYPE_LONGLONG)) return true;
  if (param_type_is_rejected(4, 6))  // as we evaluate it in fix_fields
    return true;
  return false;
}

longlong Item_func_regexp_instr::val_int() {
  DBUG_TRACE;
  assert(fixed);
  std::optional<int> pos = position();
  std::optional<int> occ = occurrence();
  std::optional<int> retopt = return_option();

  if (retopt.has_value() && retopt != 0 && retopt != 1) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0),
             "regexp_instr: return_option must be 1 or 0.");
    return error_int();
  }

  if (set_pattern() || !pos.has_value() || !occ.has_value() ||
      !retopt.has_value()) {
    return error_int();
  }

  std::optional<int32_t> result =
      m_facade->Find(subject(), pos.value(), occ.value(), retopt.value());
  if (current_thd->is_error()) return error_int();

  if (result.has_value()) {
    null_value = false;
    return result.value();
  }
  null_value = true;
  return 0;
}

longlong Item_func_regexp_like::val_int() {
  DBUG_TRACE;
  assert(fixed);

  if (set_pattern()) {
    return error_int();
  }

  /*
    REGEXP_LIKE() does not take position and occurrence arguments, so we trust
    that the calls to their accessors below will return the default values.
  */
  std::optional<bool> result =
      m_facade->Matches(subject(), position().value(), occurrence().value());
  if (current_thd->is_error()) return error_int();

  if (result.has_value()) {
    null_value = false;
    return result.value();
  }
  null_value = true;
  return 0;
}

bool Item_func_regexp_like::resolve_type(THD *thd) {
  if (Item_func_regexp::resolve_type(thd)) return true;
  if (param_type_is_rejected(2, 3))  // as we evaluate it in fix_fields
    return true;
  return false;
}

bool Item_func_regexp_replace::resolve_type(THD *thd) {
  if (Item_func_regexp::resolve_type(thd)) return true;
  if (param_type_is_default(thd, 2, 3)) return true;
  if (param_type_is_default(thd, 3, 5, MYSQL_TYPE_LONGLONG)) return true;
  if (param_type_is_rejected(5, 6))  // as we evaluate it in fix_fields
    return true;

  const CHARSET_INFO *resolved_charset = collation.collation;
  const CHARSET_INFO *replacement_charset =
      replacement()->charset_for_protocol();

  // If either of subject, pattern or replacement use the binary charset, the
  // other two must be implicitly castable to binary charset, too.  The other
  // combinations are checked in Item_func_regexp::resolve_type().
  if (((is_binary_string(subject()) || is_binary_string(pattern())) &&
       !is_binary_compatible(replacement())) ||
      (is_binary_string(replacement()) && (!is_binary_compatible(subject()) ||
                                           !is_binary_compatible(pattern())))) {
    my_error(ER_CHARACTER_SET_MISMATCH, myf(0), resolved_charset->m_coll_name,
             replacement_charset->m_coll_name, func_name());
    return error_bool();
  }

  set_data_type_string(ulonglong{MAX_BLOB_WIDTH});
  return false;
}

String *Item_func_regexp_replace::val_str(String *buf) {
  assert(fixed);

  std::optional<int> pos = position();
  std::optional<int> occ = occurrence();

  if (set_pattern() || !pos.has_value() || !occ.has_value()) {
    return error_str();
  }

  if (pos.value() < 1) {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), func_name());
    return error_str();
  }

  buf->set_charset(collation.collation);
  String *result = m_facade->Replace(subject(), replacement(), pos.value(),
                                     occ.value(), buf);
  if (current_thd->is_error()) return error_str();

  null_value = (result == nullptr);
  return result;
}

bool Item_func_regexp_substr::resolve_type(THD *thd) {
  if (Item_func_regexp::resolve_type(thd)) return true;
  if (param_type_is_default(thd, 2, 4, MYSQL_TYPE_LONGLONG)) return true;
  if (param_type_is_rejected(4, 5))  // as we evaluate it in fix_fields
    return true;
  set_data_type_string(subject()->max_char_length());
  set_nullable(true);
  return false;
}

String *Item_func_regexp_substr::val_str(String *buf) {
  assert(fixed);
  std::optional<int> pos = position();
  std::optional<int> occ = occurrence();

  if (set_pattern() || !pos.has_value() || !occ.has_value()) {
    return null_return_str();
  }
  if (pos.value() < 1) {
    my_error(ER_WRONG_PARAMETERS_TO_NATIVE_FCT, MYF(0), func_name());
    return error_str();
  }
  buf->set_charset(collation.collation);
  String *result = m_facade->Substr(subject(), pos.value(), occ.value(), buf);
  if (current_thd->is_error()) return error_str();

  null_value = (result == nullptr);
  return result;
}

using regexp::icu_version_string;

Item_func_icu_version::Item_func_icu_version(const POS &pos)
    : Item_static_string_func(pos, NAME_STRING("icu_version()"),
                              icu_version_string(),
                              strlen(icu_version_string()), system_charset_info,
                              DERIVATION_SYSCONST) {}

bool Item_func_icu_version::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (super::do_itemize(pc, res)) return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  return false;
}
