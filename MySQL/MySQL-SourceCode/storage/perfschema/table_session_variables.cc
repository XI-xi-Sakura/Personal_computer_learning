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

/**
  @file storage/perfschema/table_session_variables.cc
  Table SESSION_VARIABLES (implementation).
*/

#include "storage/perfschema/table_session_variables.h"

#include <assert.h>
#include <stddef.h>
#include <new>

#include "my_thread.h"
#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/mysqld.h"
#include "sql/plugin_table.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_column_values.h"
#include "storage/perfschema/pfs_global.h"
#include "storage/perfschema/pfs_instr_class.h"

bool PFS_index_session_variables::match(const System_variable *pfs) {
  if (m_fields >= 1) {
    if (!m_key.match(pfs)) {
      return false;
    }
  }

  return true;
}

THR_LOCK table_session_variables::m_table_lock;

Plugin_table table_session_variables::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "session_variables",
    /* Definition */
    "  VARIABLE_NAME VARCHAR(64) not null,\n"
    "  VARIABLE_VALUE VARCHAR(1024),\n"
    "  PRIMARY KEY (VARIABLE_NAME ) USING HASH\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_session_variables::m_share = {
    &pfs_readonly_world_acl,
    table_session_variables::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_session_variables::get_row_count,
    sizeof(pos_t),
    &m_table_lock,
    &m_table_def,
    true, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_session_variables::create(PFS_engine_table_share *) {
  return new table_session_variables();
}

ha_rows table_session_variables::get_row_count() {
  mysql_mutex_lock(&LOCK_plugin_delete);
#ifndef NDEBUG
  mysql_mutex_assert_not_owner(&LOCK_plugin);
#endif
  mysql_rwlock_rdlock(&LOCK_system_variables_hash);
  const ha_rows system_var_count = get_system_variable_count();
  mysql_rwlock_unlock(&LOCK_system_variables_hash);
  mysql_mutex_unlock(&LOCK_plugin_delete);
  return system_var_count;
}

table_session_variables::table_session_variables()
    : PFS_engine_table(&m_share, &m_pos),
      m_sysvar_cache(false),
      m_pos(0),
      m_next_pos(0),
      m_opened_index(nullptr) {}

void table_session_variables::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

int table_session_variables::rnd_init(bool /* scan */) {
  /* Build a cache of system variables for this thread. */
  m_sysvar_cache.materialize_all(current_thd);

  return 0;
}

int table_session_variables::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < m_sysvar_cache.size();
       m_pos.next()) {
    if (m_sysvar_cache.is_materialized()) {
      const System_variable *system_var = m_sysvar_cache.get(m_pos.m_index);
      if (system_var != nullptr) {
        if (!make_row(system_var)) {
          m_next_pos.set_after(&m_pos);
          return 0;
        }
      }
    }
  }
  return HA_ERR_END_OF_FILE;
}

int table_session_variables::rnd_pos(const void *pos) {
  set_position(pos);
  assert(m_pos.m_index < m_sysvar_cache.size());

  if (m_sysvar_cache.is_materialized()) {
    const System_variable *system_var = m_sysvar_cache.get(m_pos.m_index);
    if (system_var != nullptr) {
      return make_row(system_var);
    }
  }
  return HA_ERR_RECORD_DELETED;
}

int table_session_variables::index_init(uint idx [[maybe_unused]], bool) {
  /*
    Build a cache of system variables for this thread.
  */
  m_sysvar_cache.materialize_all(current_thd);

  assert(idx == 0);
  auto *result = PFS_NEW(PFS_index_session_variables);
  m_opened_index = result;
  m_index = result;

  return 0;
}

int table_session_variables::index_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < m_sysvar_cache.size();
       m_pos.next()) {
    if (m_sysvar_cache.is_materialized()) {
      const System_variable *system_var = m_sysvar_cache.get(m_pos.m_index);
      if (system_var != nullptr) {
        if (m_opened_index->match(system_var)) {
          if (!make_row(system_var)) {
            m_next_pos.set_after(&m_pos);
            return 0;
          }
        }
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_session_variables::make_row(const System_variable *system_var) {
  if (m_row.m_variable_name.make_row(system_var->m_name,
                                     system_var->m_name_length)) {
    return HA_ERR_RECORD_DELETED;
  }

  if (m_row.m_variable_value.make_row(system_var)) {
    return HA_ERR_RECORD_DELETED;
  }

  return 0;
}

int table_session_variables::read_row_values(TABLE *table, unsigned char *buf,
                                             Field **fields, bool read_all) {
  Field *f;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0] = 0;

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /* VARIABLE_NAME */
          set_field_varchar_utf8mb4(f, m_row.m_variable_name.m_str,
                                    m_row.m_variable_name.m_length);
          break;
        case 1: /* VARIABLE_VALUE */
          m_row.m_variable_value.set_field(f);
          break;
        default:
          assert(false);
      }
    }
  }

  return 0;
}
