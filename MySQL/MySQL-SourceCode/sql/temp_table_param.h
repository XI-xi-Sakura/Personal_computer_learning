/* Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#ifndef TEMP_TABLE_PARAM_INCLUDED
#define TEMP_TABLE_PARAM_INCLUDED

#include <sys/types.h>
#include <vector>

#include "my_base.h"
#include "my_inttypes.h"
#include "sql/field.h"
#include "sql/mem_root_array.h"
#include "sql/thr_malloc.h"

class KEY;
class Item;
class Window;
struct CHARSET_INFO;
struct MEM_ROOT;

enum Copy_func_type : int;

/**
   Helper class for copy_funcs(); represents an Item to copy from table to
   next tmp table.
*/
class Func_ptr {
 public:
  Func_ptr(Item *item, Field *result_field, Item *result_item = nullptr);

  Item *func() const { return m_func; }
  void set_func(Item *func);
  Field *result_field() const { return m_result_field; }
  Item *result_item() const;
  bool should_copy(Copy_func_type type) const {
    return m_func_bits & (1 << type);
  }

 private:
  Item *m_func;
  Field *m_result_field;

  // A premade Item for m_result_field (may be nullptr if allocation failed).
  // This has two purposes:
  //
  //  - It avoids repeated constructions if the field is used multiple times
  //    (e.g., first in a SELECT list, then in a sort order).
  //  - It gives a canonical, unique item, so that we can compare it with ==
  //    (in FindReplacementItem(), where ->eq would have a metadata issues).
  //    This is important if we are to replace it with something else again
  //    later.
  //
  // It is created on-demand to avoid getting into the thd->stmt_arena field
  // list for a temporary table that is freed later anyway.
  // It is usually an Item_field, but if supplied from constructor, can be of
  // any type.
  mutable Item *m_result_item = nullptr;

  // A bitmap where all CFT_* enums are bit indexes, and we have a 1 if m_func
  // is of the type given by that enum. E.g., if m_func is an Item_field,
  // (1 << CFT_FIELDS) will be set here. This is used for quickly finding out
  // which items to copy in copy_funcs(), without having to look at the actual
  // items (which involves virtual function calls).
  int m_func_bits;
};

/// Used by copy_funcs()
typedef Mem_root_array<Func_ptr> Func_ptr_array;

/**
  Object containing parameters used when creating and using temporary
  tables. Temporary tables created with the help of this object are
  used only internally by the query execution engine.
*/

class Temp_table_param {
 public:
  Mem_root_array<Copy_field> copy_fields;

  uchar *group_buff;
  Func_ptr_array *items_to_copy; /* Fields in tmp table */

  /**
    After temporary table creation, points to an index on the table
    created depending on the purpose of the table - grouping,
    duplicate elimination, etc. There is at most one such index.
  */
  KEY *keyinfo;

  /**
    LIMIT (maximum number of rows) for this temp table, or HA_POS_ERROR
    for no limit. Enforced by MaterializeIterator when writing to the table.
   */
  ha_rows end_write_records{HA_POS_ERROR};

  /**
    Number of items in the query. Includes both aggregate functions (e.g., SUM),
    and non-aggregates (e.g., RAND), window functions and fields.
    Also counts functions referred to from windowing or aggregate functions,
    i.e., "SELECT SUM(RAND())" sets this counter to 2.

    @see count_field_types
  */
  uint func_count;
  /**
    Number of fields in the query that have aggregate functions. Note
    that the optimizer may choose to optimize away these fields by
    replacing them with constants, in which case sum_func_count will
    need to be updated.

    @see optimize_aggregated_query, count_field_types
  */
  uint sum_func_count;
  uint hidden_field_count;
  uint group_parts, group_length, group_null_parts;
  /**
    Whether we allow running GROUP BY processing into a temporary table,
    i.e., keeping many different aggregations going at once without
    having ordered input. This is usually the case, but is currently not
    supported for aggregation UDFs, aggregates with DISTINCT, or ROLLUP.

    Note that even if this is true, the optimizer may choose to not use
    a temporary table, as it is often more efficient to just read along
    an index.
   */
  bool allow_group_via_temp_table{true};
  /**
    Number of outer_sum_funcs i.e the number of set functions that are
    aggregated in a query block outer to this subquery.

    @see count_field_types
  */
  uint outer_sum_func_count;
  /**
    Enabled when we have at least one outer_sum_func. Needed when used
    along with distinct.

    @see create_tmp_table
  */
  bool using_outer_summary_function;
  CHARSET_INFO *table_charset;
  bool schema_table;
  /*
    True if GROUP BY and its aggregate functions are already computed
    by a table access method (e.g. by loose index scan). In this case
    query execution should not perform aggregation and should treat
    aggregate functions as normal functions.
  */
  bool precomputed_group_by;
  bool force_copy_fields;
  /**
    true <=> don't actually create table handler when creating the result
    table. This allows range optimizer to add indexes later.
    Used for materialized derived tables/views.
    @see Table_ref::update_derived_keys.
  */
  bool skip_create_table;

  /// Whether the UNIQUE index can be promoted to PK
  bool can_use_pk_for_unique;

  /// Whether UNIQUE keys should always be implemented by way of a hidden hash
  /// field, never a unique index. Needed for materialization of mixed
  /// UNION ALL / UNION DISTINCT queries (see comments in create_result_table())
  /// and for DISTINCT deduplication using materialization (See
  /// CreateTemporaryTableFromSelectList()).
  bool force_hash_field_for_unique{false};

  /// This tmp table is used for a window's frame buffer
  bool m_window_frame_buffer{false};

  /// For INTERSECT and EXCEPT computation
  enum {
    TTP_UNION_OR_TABLE,
    TTP_EXCEPT,
    TTP_INTERSECT
  } m_operation{TTP_UNION_OR_TABLE};
  /// The tempoary table rows need a counter to keep track of its
  /// duplicates: needed for EXCEPT and INTERSECT computation.
  bool needs_set_counter() { return m_operation != TTP_UNION_OR_TABLE; }
  /// For INTERSECT and EXCEPT computation.
  /// Cf. TABLE::m_last_operation_is_distinct.
  bool m_last_operation_is_distinct{false};

  /// If this is the out table of a window: the said window
  Window *m_window;

  explicit Temp_table_param(MEM_ROOT *mem_root = *THR_MALLOC)
      : copy_fields(mem_root),
        group_buff(nullptr),
        items_to_copy(nullptr),
        keyinfo(nullptr),
        func_count(0),
        sum_func_count(0),
        hidden_field_count(0),
        group_parts(0),
        group_length(0),
        group_null_parts(0),
        outer_sum_func_count(0),
        using_outer_summary_function(false),
        table_charset(nullptr),
        schema_table(false),
        precomputed_group_by(false),
        force_copy_fields(false),
        skip_create_table(false),
        can_use_pk_for_unique(true),
        m_window(nullptr) {}

  Temp_table_param(MEM_ROOT *mem_root, const Temp_table_param &other)
      : copy_fields(mem_root),
        group_buff(other.group_buff),
        items_to_copy(other.items_to_copy),
        keyinfo(other.keyinfo),
        end_write_records(other.end_write_records),
        func_count(other.func_count),
        sum_func_count(other.sum_func_count),
        hidden_field_count(other.hidden_field_count),
        group_parts(other.group_parts),
        group_length(other.group_length),
        group_null_parts(other.group_null_parts),
        allow_group_via_temp_table(other.allow_group_via_temp_table),
        outer_sum_func_count(other.outer_sum_func_count),
        using_outer_summary_function(other.using_outer_summary_function),
        table_charset(other.table_charset),
        schema_table(other.schema_table),
        precomputed_group_by(other.precomputed_group_by),
        force_copy_fields(other.force_copy_fields),
        skip_create_table(other.skip_create_table),
        can_use_pk_for_unique(other.can_use_pk_for_unique),
        force_hash_field_for_unique(other.force_hash_field_for_unique),
        m_window_frame_buffer(other.m_window_frame_buffer),
        m_window(other.m_window) {}

  // Used by CTE derived table clones to set correct info, see
  // Common_table_expr::clone_tmp_table. The info may be consulted e.g.
  // by get_hidden_field_count_for_derived(), e.g. by HW.
  Temp_table_param &operator=(const Temp_table_param &other) {
    if (this == &other) {
      return *this;
    }
    for (const auto &cf : other.copy_fields) copy_fields.push_back(cf);
    group_buff = other.group_buff;
    items_to_copy = other.items_to_copy;
    keyinfo = other.keyinfo;
    end_write_records = other.end_write_records;
    func_count = other.func_count;
    sum_func_count = other.sum_func_count;
    hidden_field_count = other.hidden_field_count;
    group_parts = other.group_parts;
    group_length = other.group_length;
    group_null_parts = other.group_null_parts;
    allow_group_via_temp_table = other.allow_group_via_temp_table;
    outer_sum_func_count = other.outer_sum_func_count;
    using_outer_summary_function = other.using_outer_summary_function;
    table_charset = other.table_charset;
    schema_table = other.schema_table;
    precomputed_group_by = other.precomputed_group_by;
    force_copy_fields = other.force_copy_fields;
    skip_create_table = other.skip_create_table;
    can_use_pk_for_unique = other.can_use_pk_for_unique;
    force_hash_field_for_unique = other.force_hash_field_for_unique;
    m_window_frame_buffer = other.m_window_frame_buffer;
    m_window = other.m_window;
    return *this;
  }

  void cleanup() { copy_fields.clear(); }
};

#endif  // TEMP_TABLE_PARAM_INCLUDED
