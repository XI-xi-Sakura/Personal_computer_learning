/* Copyright (c) 2006, 2025, Oracle and/or its affiliates.

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

#ifndef SQL_LOAD_INCLUDED
#define SQL_LOAD_INCLUDED

#include <assert.h>
#include <sys/types.h>

#include "lex_string.h"

#include "my_compress.h"
#include "my_sqlcommand.h"
#include "mysql/components/services/bulk_load_service.h"
#include "mysql/strings/m_ctype.h"
#include "sql/current_thd.h"
#include "sql/sql_cmd.h"         /* Sql_cmd */
#include "sql/sql_data_change.h" /* enum_duplicates */
#include "sql/sql_exchange.h"    /* sql_exchange */
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql_string.h"

class Item;
class READ_INFO;
class THD;
class Table_ref;

class Sql_cmd_load_table final : public Sql_cmd {
 public:
  Sql_cmd_load_table(
      enum_filetype filetype, bool is_local_file, enum_source_type source_type,
      const LEX_STRING &filename, ulong file_count, bool in_key_order,
      On_duplicate on_duplicate, Table_ident *table,
      List<String> *opt_partitions, const CHARSET_INFO *opt_charset,
      LEX_CSTRING compression_algorithm, String *opt_xml_rows_identified_by,
      const Field_separators *field_separators,
      const Line_separators *line_separators, ulong skip_lines,
      mem_root_deque<Item *> *opt_fields_or_vars,
      mem_root_deque<Item *> *opt_set_fields,
      mem_root_deque<Item *> *opt_set_exprs, List<String> *opt_set_expr_strings,
      ulong concurrency, ulonglong memory_size, bool is_bulk_operation)
      : m_exchange(filename.str, UNDEFINED_DEST, filetype),
        m_is_local_file(is_local_file),
        m_bulk_source(source_type),
        m_file_count(file_count),
        m_ordered_data(in_key_order),
        m_concurrency(concurrency),
        m_memory_size(memory_size),
        m_on_duplicate(on_duplicate),
        m_table(table),
        m_opt_partitions(opt_partitions),
        m_opt_fields_or_vars(*THR_MALLOC),
        m_opt_set_fields(*THR_MALLOC),
        m_opt_set_exprs(*THR_MALLOC),
        m_opt_set_expr_strings(opt_set_expr_strings),
        m_is_bulk_operation(is_bulk_operation),
        m_compression_algorithm_string(compression_algorithm) {
    if (opt_fields_or_vars)
      m_opt_fields_or_vars = std::move(*opt_fields_or_vars);
    assert((opt_set_fields == nullptr) ^ (opt_set_exprs != nullptr));
    if (opt_set_fields) {
      assert(opt_set_fields->size() == opt_set_exprs->size());
      m_opt_set_fields = std::move(*opt_set_fields);
      m_opt_set_exprs = std::move(*opt_set_exprs);
    }

    m_exchange.file_info.cs = opt_charset;

    if (opt_xml_rows_identified_by != nullptr)
      m_exchange.line.line_term = opt_xml_rows_identified_by;

    m_exchange.field.merge_field_separators(field_separators);
    m_exchange.line.merge_line_separators(line_separators);
    m_exchange.assign_default_values();
    m_exchange.skip_lines = skip_lines;
  }

  enum_sql_command sql_command_code() const override { return SQLCOM_LOAD; }

  bool execute(THD *thd) override;

  bool is_bulk_load() const override { return m_is_bulk_operation; }

 public:
  sql_exchange m_exchange;
  const bool m_is_local_file;

 private:
  /// Source for bulk data.
  enum_source_type m_bulk_source;

  /// Number of input files.
  ulong m_file_count;

  /// true if input data is in order of Primary Key.
  bool m_ordered_data;

  /// Number of concurrent loaders to use
  ulong m_concurrency;

  /// Maximum memory size to be used in bytes
  ulonglong m_memory_size;

 public:
  const On_duplicate m_on_duplicate;
  Table_ident *const m_table;
  List<String> *const m_opt_partitions;
  mem_root_deque<Item *> m_opt_fields_or_vars;
  mem_root_deque<Item *> m_opt_set_fields;
  mem_root_deque<Item *> m_opt_set_exprs;

  /**
    A list of strings is maintained to store the SET clause command user strings
    which are specified in load data operation.  This list will be used
    during the reconstruction of "load data" statement at the time of writing
    to binary log.
  */
  List<String> *const m_opt_set_expr_strings;

 private:
  bool execute_inner(THD *thd, enum enum_duplicates handle_duplicates);

  bool execute_bulk(THD *thd);

  bool truncate_table_for_bulk_load(THD *thd, Table_ref *const table_ref,
                                    dd::Table *table_def);

  bool check_bulk_load_parameters(THD *thd);

  bool validate_table_for_bulk_load(THD *thd, Table_ref *const table_ref,
                                    dd::Table *table_def, handlerton **hton);

  bool bulk_driver_service(THD *thd, const TABLE *table, size_t &affected_rows);

  bool read_fixed_length(THD *thd, COPY_INFO &info, Table_ref *table_list,
                         READ_INFO &read_info, ulong skip_lines);

  bool read_sep_field(THD *thd, COPY_INFO &info, Table_ref *table_list,
                      READ_INFO &read_info, const String &enclosed,
                      ulong skip_lines);

  bool read_xml_field(THD *thd, COPY_INFO &info, Table_ref *table_list,
                      READ_INFO &read_info, ulong skip_lines);

  bool write_execute_load_query_log_event(
      THD *thd, const char *db, const char *table_name, bool is_concurrent,
      enum enum_duplicates duplicates, bool transactional_table, int errocode);

 private:
  /// true if BULK LOAD.
  bool m_is_bulk_operation;
  LEX_CSTRING m_compression_algorithm_string;
};

#endif /* SQL_LOAD_INCLUDED */
