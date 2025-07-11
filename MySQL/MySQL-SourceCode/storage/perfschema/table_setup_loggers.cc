/* Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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
  @file storage/perfschema/table_setup_loggers.cc
  Table SETUP_LOGGERS (implementation).
*/

#include "storage/perfschema/table_setup_loggers.h"

#include <cassert>
#include <cstddef>

#include <mysql/components/services/mysql_server_telemetry_logs_service.h>
#include "my_thread.h"
#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_column_values.h"
#include "storage/perfschema/pfs_global.h"
#include "storage/perfschema/pfs_instr_class.h"

extern std::atomic<log_delivery_callback_t> g_telemetry_log;

THR_LOCK table_setup_loggers::m_table_lock;

Plugin_table table_setup_loggers::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "setup_loggers",
    /* Definition */
    "  NAME VARCHAR(128) not null,\n"
    "  LEVEL ENUM ('none', 'error', 'warn', 'info', 'debug') not null,\n"
    "  DESCRIPTION VARCHAR(1023)\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_setup_loggers::m_share = {
    &pfs_updatable_acl,
    table_setup_loggers::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_setup_loggers::get_row_count,
    sizeof(pos_t),
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_setup_loggers::create(PFS_engine_table_share *) {
  return new table_setup_loggers();
}

ha_rows table_setup_loggers::get_row_count() { return logger_class_count(); }

table_setup_loggers::table_setup_loggers()
    : PFS_engine_table(&m_share, &m_pos), m_pos(1), m_next_pos(1) {}

void table_setup_loggers::reset_position() {
  m_pos.m_index = 1;
  m_next_pos.m_index = 1;
}

int table_setup_loggers::rnd_next() {
  /* Do not advertise meters when disabled. */
  if (!pfs_initialized) {
    return HA_ERR_END_OF_FILE;
  }

  for (m_pos.set_at(&m_next_pos); m_pos.m_index <= logger_class_max;
       m_pos.next()) {
    PFS_logger_class *instr_class = find_logger_class(m_pos.m_index);
    if (instr_class) {
      m_next_pos.set_after(&m_pos);
      return make_row(instr_class);
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_setup_loggers::rnd_pos(const void *pos) {
  /* Do not advertise meters when disabled. */
  if (!pfs_initialized) {
    return HA_ERR_END_OF_FILE;
  }

  set_position(pos);

  PFS_logger_class *instr_class = find_logger_class(m_pos.m_index);

  if (instr_class) {
    return make_row(instr_class);
  }

  return HA_ERR_RECORD_DELETED;
}

int table_setup_loggers::make_row(PFS_logger_class *klass) {
  /* Protect this reader against an instrument delete. */
  pfs_optimistic_state lock = pfs_optimistic_state();
  klass->m_lock.begin_optimistic_lock(&lock);

  m_row.m_instr_class = klass;

  // materialize the row
  m_row.m_logger_name_length = klass->m_name.length();
  memcpy(m_row.m_logger_name, klass->m_name.str(), m_row.m_logger_name_length);
  m_row.m_level = klass->m_level;

  m_row.m_description_length = strlen(klass->m_description);
  memcpy(m_row.m_description, klass->m_description, m_row.m_description_length);

  if (!klass->m_lock.end_optimistic_lock(&lock)) {
    return 1;
  }

  return 0;
}

int table_setup_loggers::read_row_values(TABLE *table, unsigned char *buf,
                                         Field **fields, bool read_all) {
  Field *f;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0] = 0;

  /*
    The row always exist, the instrument classes
    are static and never disappear.
  */

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /* NAME */
          set_field_varchar_utf8mb4(f, m_row.m_logger_name,
                                    m_row.m_logger_name_length);
          break;
        case 1: /* LEVEL */
          set_field_enum(f, m_row.m_level + 1);
          break;
        case 2: /* DESCRIPTION */
          set_field_varchar_utf8mb4(f, m_row.m_description,
                                    m_row.m_description_length);
          break;
        default:
          assert(false);
      }
    }
  }

  return 0;
}

int table_setup_loggers::update_row_values(TABLE *table, const unsigned char *,
                                           unsigned char *, Field **fields) {
  Field *f;
  ulonglong value;

  for (; (f = *fields); fields++) {
    if (bitmap_is_set(table->write_set, f->field_index())) {
      switch (f->field_index()) {
        case 1: /* LEVEL */
          value = get_field_enum(f);
          m_row.m_instr_class->m_level = (OTELLogLevel)(value - 1);
          if (g_telemetry_log == nullptr)
            m_row.m_instr_class->m_effective_level = TLOG_NONE;
          else
            m_row.m_instr_class->m_effective_level =
                m_row.m_instr_class->m_level;
          break;
        default:
          return HA_ERR_WRONG_COMMAND;
      }
    }
  }

  /* No derived flag to update. */

  return 0;
}
