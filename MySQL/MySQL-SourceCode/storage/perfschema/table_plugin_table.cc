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
  @file storage/perfschema/table_plugin_table.cc
  plugins/components tables (implementation).
*/

#include "storage/perfschema/table_plugin_table.h"

#include "sql/field.h"
#include "sql/table.h"
#include "storage/perfschema/pfs_plugin_table.h"

int PFS_plugin_table_index::init(PSI_table_handle *plugin_table, uint idx,
                                 bool sorted) {
  int ret;
  m_idx = idx;

  if (unlikely(m_st_table->index_init == nullptr)) {
    return 0;
  }

  /* Call the plugin to initialize the index */
  ret = m_st_table->index_init(plugin_table, idx, sorted, &m_plugin_index);
  return ret;
}

void PFS_plugin_table_index::read_key(const uchar *key, uint key_len,
                                      enum ha_rkey_function find_flag) {
  if (unlikely(m_st_table->index_read == nullptr)) {
    return;
  }

  PFS_key_reader reader(m_key_info, key, key_len);
  m_st_table->index_read(m_plugin_index, (PSI_key_reader *)&reader, m_idx,
                         find_flag);
}

int PFS_plugin_table_index::index_next(PSI_table_handle *table) {
  if (unlikely(m_st_table->index_next == nullptr)) {
    return HA_ERR_END_OF_FILE;
  }

  return m_st_table->index_next(table);
}

PFS_engine_table *table_plugin_table::create(PFS_engine_table_share *share) {
  return new table_plugin_table((PFS_engine_table_share *)share);
}

table_plugin_table::table_plugin_table(PFS_engine_table_share *share)
    : PFS_engine_table(share, nullptr),
      m_share(share),
      m_table_lock(share->m_thr_lock_ptr),
      m_row_exists(false),
      m_next_pos(nullptr),
      m_opened_index(nullptr) {
  this->m_st_table = &share->m_st_table;
  this->plugin_table_handle = m_st_table->open_table(&m_pos);
  /* Setup the base class position pointer */
  m_pos_ptr = m_pos;
}

void table_plugin_table::reset_position() {
  if (unlikely(m_st_table->reset_position == nullptr)) {
    return;
  }
  m_st_table->reset_position(this->plugin_table_handle);
}

int table_plugin_table::rnd_init(bool scan) {
  if (unlikely(m_st_table->rnd_next == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }
  return m_st_table->rnd_init(this->plugin_table_handle, scan);
}

int table_plugin_table::rnd_next() {
  if (unlikely(m_st_table->rnd_next == nullptr)) {
    return HA_ERR_END_OF_FILE;
  }
  return m_st_table->rnd_next(this->plugin_table_handle);
}

int table_plugin_table::rnd_pos(const void *pos) {
  if (unlikely(m_st_table->rnd_pos == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }
  set_position(pos);
  return m_st_table->rnd_pos(this->plugin_table_handle);
}

int table_plugin_table::index_init(uint idx, bool sorted) {
  /* Create an index instance for plugin table */
  auto *result = new PFS_plugin_table_index(m_st_table);
  const int ret = result->init(plugin_table_handle, idx, sorted);

  m_opened_index = result;
  m_index = result;

  return ret;
}

int table_plugin_table::index_next() {
  return m_opened_index->index_next(this->plugin_table_handle);
}

int table_plugin_table::read_row_values(TABLE *table, unsigned char *buf,
                                        Field **fields, bool read_all) {
  Field *f;
  int result = 0;

  if (unlikely(m_st_table->read_column_value == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }

  /* Set the buf using null_bytes */
  for (uint temp_null_bytes = table->s->null_bytes; temp_null_bytes > 0;
       temp_null_bytes--) {
    buf[temp_null_bytes - 1] = 0;
  }

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      result = m_st_table->read_column_value(this->plugin_table_handle,
                                             (PSI_field *)f, f->field_index());
      if (result) {
        return result;
      }
    }
  }

  return result;
}

int table_plugin_table::delete_all_rows() {
  if (unlikely(m_share->m_delete_all_rows == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }

  return m_share->m_delete_all_rows();
}

int table_plugin_table::update_row_values(TABLE *table, const unsigned char *,
                                          unsigned char *, Field **fields) {
  Field *f;

  if (unlikely(m_st_table->update_column_value == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }

  for (; (f = *fields); fields++) {
    if (bitmap_is_set(table->write_set, f->field_index())) {
      const int result = m_st_table->update_column_value(
          plugin_table_handle, (PSI_field *)f, f->field_index());
      if (result) {
        return result;
      }
    }
  }

  /* After the columns values are updated, update the row */
  return m_st_table->update_row_values(plugin_table_handle);
}

int table_plugin_table::delete_row_values(TABLE *, const unsigned char *,
                                          Field **) {
  if (unlikely(m_st_table->delete_row_values == nullptr)) {
    return HA_ERR_WRONG_COMMAND;
  }

  return m_st_table->delete_row_values(plugin_table_handle);
}
