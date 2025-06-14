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

// Implements
#include "storage/ndb/plugin/ndb_sync_pending_objects_table.h"

#include <cassert>
#include <cstdint>
#include <cstring>  // std::strlen

// assert
#include "storage/ndb/plugin/ha_ndbcluster_binlog.h"  // ndbcluster_binlog_retrieve_sync_pending_objects

static unsigned long long ndb_pending_objects_row_count() {
  // Retrieve row count from the metadata sync implementation
  return ndbcluster_binlog_get_sync_pending_objects_count();
}

static PSI_table_handle *ndb_pending_objects_open_table(PSI_pos **pos) {
  // Constructs a table object and returns an opaque pointer
  auto *row_pos = reinterpret_cast<uint32_t **>(pos);
  /*
    Creates an instance of the table. Note that this is deallocated during the
    table close which is implemented in the base class. See the
    ndb_pfs_close_table() function in ndb_pfs_table.cc
  */
  auto *table = new Ndb_sync_pending_objects_table();
  *row_pos = table->get_position_address();
  auto *handle = reinterpret_cast<PSI_table_handle *>(table);
  return handle;
}

Ndb_sync_pending_objects_table_share::Ndb_sync_pending_objects_table_share()
    : Ndb_pfs_table_share() {
  m_table_name = "ndb_sync_pending_objects";
  m_table_name_length = std::strlen(m_table_name);
  m_table_definition =
      "`SCHEMA_NAME` varchar(64),"
      "`NAME` varchar(64),"
      "`TYPE` enum('LOGFILE GROUP', 'TABLESPACE', 'SCHEMA', 'TABLE') NOT NULL";
  get_row_count = ndb_pending_objects_row_count;

  m_proxy_engine_table.open_table = ndb_pending_objects_open_table;
}

int Ndb_sync_pending_objects_table::rnd_init() {
  // Retrieve information and store it in m_pending_objects
  ndbcluster_binlog_retrieve_sync_pending_objects(this);
  set_num_rows(m_pending_objects.size());
  reset_pos();
  return 0;
}

extern SERVICE_TYPE_NO_CONST(pfs_plugin_column_string_v2) * pfscol_string;
extern SERVICE_TYPE_NO_CONST(pfs_plugin_column_enum_v1) * pfscol_enum;

int Ndb_sync_pending_objects_table::read_column_value(PSI_field *field,
                                                      uint32_t index) {
  assert(!is_empty() && rows_pending_read());
  PSI_ulonglong bigint_value;

  const unsigned int row_index = get_position();
  const Pending_object &obj = m_pending_objects[row_index - 1];

  switch (index) {
    case 0: /* SCHEMA_NAME: Name of the schema */
      pfscol_string->set_varchar_utf8mb4(
          field,
          obj.m_schema_name.empty() ? nullptr : obj.m_schema_name.c_str());
      break;
    case 1: /* NAME: Object name */
      pfscol_string->set_varchar_utf8mb4(
          field, obj.m_name.empty() ? nullptr : obj.m_name.c_str());
      break;
    case 2: /* TYPE */
      // type + 1 since index 0 is used for empty strings in enum
      bigint_value.val = obj.m_type + 1;
      bigint_value.is_null = false;
      pfscol_enum->set(field, bigint_value);
      break;
    default:
      assert(false);
  }
  return 0;
}

void Ndb_sync_pending_objects_table::close() {
  m_pending_objects.clear();
  reset_pos();
}

// Instantiate the table share
Ndb_sync_pending_objects_table_share pending_objects_table_share;
PFS_engine_table_share_proxy *ndb_sync_pending_objects_share =
    &pending_objects_table_share;
