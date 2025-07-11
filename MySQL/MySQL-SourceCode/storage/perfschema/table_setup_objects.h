/* Copyright (c) 2008, 2025, Oracle and/or its affiliates.

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

#ifndef TABLE_SETUP_OBJECTS_H
#define TABLE_SETUP_OBJECTS_H

/**
  @file storage/perfschema/table_setup_objects.h
  Table SETUP_OBJECTS (declarations).
*/

#include <sys/types.h>

#include "my_base.h"
#include "mysql_com.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_engine_table.h"
#include "storage/perfschema/table_helper.h"

class Field;
class Plugin_table;
struct PFS_setup_object;
struct TABLE;
struct THR_LOCK;

/**
  @addtogroup performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.SETUP_OBJECTS. */
struct row_setup_objects {
  /** Column OBJECT_TYPE. */
  enum_object_type m_object_type;
  /** Column SCHEMA_NAME. */
  PFS_schema_name m_schema_name;
  /** Column OBJECT_NAME. */
  PFS_object_name m_object_name;
  /** Column ENABLED. */
  bool *m_enabled_ptr{nullptr};
  /** Column TIMED. */
  bool *m_timed_ptr{nullptr};
};

class PFS_index_setup_objects : public PFS_engine_index {
 public:
  PFS_index_setup_objects()
      : PFS_engine_index(&m_key_1, &m_key_2, &m_key_3),
        m_key_1("OBJECT_TYPE"),
        m_key_2("OBJECT_SCHEMA"),
        m_key_3("OBJECT_NAME") {}

  ~PFS_index_setup_objects() override = default;

  virtual bool match(PFS_setup_object *pfs);

 private:
  PFS_key_object_type_enum m_key_1;
  PFS_key_object_schema m_key_2;
  PFS_key_object_name m_key_3;
};

/** Table PERFORMANCE_SCHEMA.SETUP_OBJECTS. */
class table_setup_objects : public PFS_engine_table {
 public:
  /** Table share. */
  static PFS_engine_table_share m_share;
  /** Table builder. */
  static PFS_engine_table *create(PFS_engine_table_share *);
  static int write_row(PFS_engine_table *pfs_table, TABLE *table,
                       unsigned char *buf, Field **fields);
  static int delete_all_rows();
  static ha_rows get_row_count();

  void reset_position() override;

  int rnd_next() override;
  int rnd_pos(const void *pos) override;

  int index_init(uint idx, bool sorted) override;
  int index_next() override;

 protected:
  int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all) override;

  int update_row_values(TABLE *table, const unsigned char *old_buf,
                        unsigned char *new_buf, Field **fields) override;

  int delete_row_values(TABLE *table, const unsigned char *buf,
                        Field **fields) override;

  table_setup_objects();

 public:
  ~table_setup_objects() override = default;

 private:
  int make_row(PFS_setup_object *pfs);

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Table definition. */
  static Plugin_table m_table_def;

  /** Current row. */
  row_setup_objects m_row;
  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;

  PFS_index_setup_objects *m_opened_index;
};

/** @} */
#endif
