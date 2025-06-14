/* Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#ifndef PLUGIN_SECONDARY_ENGINE_MOCK_HA_MOCK_H_
#define PLUGIN_SECONDARY_ENGINE_MOCK_HA_MOCK_H_

#include "my_base.h"
#include "sql/handler.h"
#include "sql/partitioning/partition_handler.h"
#include "thr_lock.h"

class THD;
struct TABLE;
struct TABLE_SHARE;

namespace dd {
class Table;
}

namespace mock {

/**
 * The MOCK storage engine does not actually implement partitioning,
 * but we support the Partition_handler interface in order to be able
 * to access tables that are partitioned in the primary engine.
 */
class FakePartitionHandler : public Partition_handler {
 public:
  explicit FakePartitionHandler(const handler *engine) : m_engine(engine) {}

  void get_dynamic_partition_info(ha_statistics *stat_info,
                                  ha_checksum *check_sum,
                                  uint part_id) override {
    auto *primary_part_handler =
        m_engine->ha_get_primary_handler()->get_partition_handler();

    return primary_part_handler->get_dynamic_partition_info(stat_info,
                                                            check_sum, part_id);
  }

  void set_part_info(partition_info *, bool) override {}

  enum row_type get_partition_row_type(const dd::Table *, uint) override {
    return ROW_TYPE_NOT_USED;
  }

 private:
  const handler *m_engine;
};

/**
 * The MOCK storage engine is used for testing MySQL server functionality
 * related to secondary storage engines.
 *
 * There are currently no secondary storage engines mature enough to be merged
 * into mysql-trunk. Therefore, this bare-minimum storage engine, with no
 * actual functionality and implementing only the absolutely necessary handler
 * interfaces to allow setting it as a secondary engine of a table, was created
 * to facilitate pushing MySQL server code changes to mysql-trunk with test
 * coverage without depending on ongoing work of other storage engines.
 *
 * @note This mock storage engine does not support being set as a primary
 * storage engine.
 */
class ha_mock : public handler {
 public:
  ha_mock(handlerton *hton, TABLE_SHARE *table_share);

  /**
   * Partition_handler
   */
  Partition_handler *get_partition_handler() override {
    return &m_part_handler;
  }

 private:
  int create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) override {
    return HA_ERR_WRONG_COMMAND;
  }

  int open(const char *name, int mode, unsigned int test_if_locked,
           const dd::Table *table_def) override;

  int close() override { return 0; }

  int rnd_init(bool) override { return 0; }

  int rnd_next(unsigned char *) override { return HA_ERR_END_OF_FILE; }

  int rnd_pos(unsigned char *, unsigned char *) override {
    return HA_ERR_WRONG_COMMAND;
  }

  int info(unsigned int) override;

  ha_rows records_in_range(unsigned int index, key_range *min_key,
                           key_range *max_key) override;

  void position(const unsigned char *) override {}

  unsigned long index_flags(unsigned int, unsigned int, bool) const override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  Table_flags table_flags() const override;

  const char *table_type() const override { return "MOCK"; }

  /**
   * Load table into the secondary engine.
   *
   * @param[in] table - table to be loaded
   * @param[out] skip_metadata_update - should the DD metadata be updated for
   * the load of this table
   * @return 0 if success
   */
  int load_table(const TABLE &table, bool *skip_metadata_update) override;

  /**
   * Unload the table from secondary engine
   *
   * @param[in] db_name     database name
   * @param[in] table_name  table name
   * @param[in] error_if_not_loaded - whether to report an error if the table is
   * already not present in the secondary engine.
   * @return 0 if success
   */
  int unload_table(const char *db_name, const char *table_name,
                   bool error_if_not_loaded) override;

  THR_LOCK_DATA m_lock;
  FakePartitionHandler m_part_handler;
};

}  // namespace mock

#endif  // PLUGIN_SECONDARY_ENGINE_MOCK_HA_MOCK_H_
