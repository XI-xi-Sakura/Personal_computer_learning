#ifndef SQL_TMP_TABLE_INCLUDED
#define SQL_TMP_TABLE_INCLUDED

/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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
  @file

  @brief
  Temporary table handling functions.
*/

#include <sys/types.h>

#include "my_base.h"  // ha_rows
#include "my_inttypes.h"
#include "sql/item.h"  // Item
#include "sql/temp_table_param.h"

class Create_field;
class Field;
class SJ_TMP_TABLE;
class THD;
struct ORDER;
struct TABLE;
template <class T>
class List;

enum enum_internal_tmp_mem_storage_engine {
  TMP_TABLE_MEMORY,
  TMP_TABLE_TEMPTABLE
};

TABLE *create_tmp_table(THD *thd, Temp_table_param *param,
                        const mem_root_deque<Item *> &fields, ORDER *group,
                        bool distinct, bool save_sum_fields,
                        ulonglong select_options, ha_rows rows_limit,
                        const char *table_alias);
bool open_tmp_table(TABLE *table);
TABLE *create_tmp_table_from_fields(THD *thd, List<Create_field> &field_list,
                                    bool is_virtual = true,
                                    ulonglong select_options = 0,
                                    const char *alias = nullptr);
bool create_ondisk_from_heap(THD *thd, TABLE *table, int error,
                             bool insert_last_record, bool ignore_last_dup,
                             bool *is_duplicate);
void close_tmp_table(TABLE *table);
void free_tmp_table(TABLE *table);
TABLE *create_duplicate_weedout_tmp_table(THD *thd, uint uniq_tuple_length_arg,
                                          SJ_TMP_TABLE *sjtbl);
bool setup_tmp_table_handler(THD *thd, TABLE *table, ulonglong select_options,
                             bool force_disk_table = false,
                             bool schema_table = false);
bool instantiate_tmp_table(THD *thd, TABLE *table);
Field *create_tmp_field(THD *thd, TABLE *table, Item *item, Item::Type type,
                        Func_ptr_array *copy_func, Field **from_field,
                        Field **default_field, bool group, bool modify_item,
                        bool make_copy_field);
Field *create_tmp_field_from_field(THD *thd, const Field *org_field,
                                   const char *name, TABLE *table,
                                   Item_field *item);

/**
  Get the minimum of max_key_length and max_key_part_length between
  HEAP engine and internal_tmp_disk_storage_engine.
*/
void get_max_key_and_part_length(uint *max_key_length,
                                 uint *max_key_part_length,
                                 uint *max_key_parts);
void init_cache_tmp_engine_properties();
void encode_innodb_position(uchar *rowid_bytes, uint length, ha_rows row_num);
bool reposition_innodb_cursor(TABLE *table, ha_rows row_num);
#endif /* SQL_TMP_TABLE_INCLUDED */
