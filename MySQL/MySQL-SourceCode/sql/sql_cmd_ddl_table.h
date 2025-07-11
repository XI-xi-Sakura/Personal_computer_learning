/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#ifndef SQL_CMD_CREATE_TABLE_INCLUDED
#define SQL_CMD_CREATE_TABLE_INCLUDED

#include "lex_string.h"
#include "my_sqlcommand.h"
#include "sql/sql_cmd_ddl.h"

class Alter_info;
class THD;
class Table_ref;

/**
  A base class for CREATE/ALTER TABLE commands and friends.

  Child classes deal with SQL statements:
  * ALTER TABLE
  * ANALYZE TABLE
  * CACHE INDEX
  * CHECK TABLE
  * CREATE INDEX
  * CREATE TABLE
  * DROP INDEX
  * LOAD INDEX
  * OPTIMIZE TABLE
  * REPAIR TABLE
*/
class Sql_cmd_ddl_table : public Sql_cmd_ddl {
 public:
  explicit Sql_cmd_ddl_table(Alter_info *alter_info);

  ~Sql_cmd_ddl_table() override = 0;  // force abstract class

 protected:
  Alter_info *const m_alter_info;
};

inline Sql_cmd_ddl_table::~Sql_cmd_ddl_table() = default;

class Sql_cmd_create_table final : public Sql_cmd_ddl_table {
 public:
  Sql_cmd_create_table(Alter_info *alter_info,
                       Table_ref *query_expression_tables)
      : Sql_cmd_ddl_table(alter_info),
        query_expression_tables(query_expression_tables) {}

  enum_sql_command sql_command_code() const override {
    return SQLCOM_CREATE_TABLE;
  }

  /// Need to allow this for CREATE ... AS SELECT ...
  bool are_dynamic_parameters_allowed() const override { return true; }

  const MYSQL_LEX_CSTRING *eligible_secondary_storage_engine(
      THD *thd) const override;

  bool execute(THD *thd) override;
  bool prepare(THD *thd) override;
  bool reprepare_on_execute_required() const override;

 private:
  Table_ref *query_expression_tables;
};

class Sql_cmd_create_or_drop_index_base : public Sql_cmd_ddl_table {
 public:
  using Sql_cmd_ddl_table::Sql_cmd_ddl_table;

  ~Sql_cmd_create_or_drop_index_base() override = 0;  // force abstract class

  bool execute(THD *thd) override;
};

inline Sql_cmd_create_or_drop_index_base::~Sql_cmd_create_or_drop_index_base() =
    default;

class Sql_cmd_create_index final : public Sql_cmd_create_or_drop_index_base {
 public:
  using Sql_cmd_create_or_drop_index_base::Sql_cmd_create_or_drop_index_base;

  enum_sql_command sql_command_code() const override {
    return SQLCOM_CREATE_INDEX;
  }
  bool reprepare_on_execute_required() const override;
};

class Sql_cmd_drop_table final : public Sql_cmd_ddl {
 public:
  enum_sql_command sql_command_code() const override {
    return SQLCOM_DROP_TABLE;
  }

  bool execute(THD *thd [[maybe_unused]]) override { return false; }
};

class Sql_cmd_drop_index final : public Sql_cmd_create_or_drop_index_base {
 public:
  using Sql_cmd_create_or_drop_index_base::Sql_cmd_create_or_drop_index_base;

  enum_sql_command sql_command_code() const override {
    return SQLCOM_DROP_INDEX;
  }
};

class Sql_cmd_cache_index final : public Sql_cmd_ddl_table {
 public:
  Sql_cmd_cache_index(Alter_info *alter_info, const LEX_CSTRING &key_cache_name)
      : Sql_cmd_ddl_table(alter_info), m_key_cache_name(key_cache_name) {}

  enum_sql_command sql_command_code() const override {
    return SQLCOM_ASSIGN_TO_KEYCACHE;
  }

  bool execute(THD *thd) override;

 private:
  bool assign_to_keycache(THD *thd, Table_ref *tables);

 private:
  const LEX_CSTRING m_key_cache_name;
};

class Sql_cmd_load_index final : public Sql_cmd_ddl_table {
 public:
  using Sql_cmd_ddl_table::Sql_cmd_ddl_table;

  enum_sql_command sql_command_code() const override {
    return SQLCOM_PRELOAD_KEYS;
  }

  bool execute(THD *thd) override;

 private:
  bool preload_keys(THD *thd, Table_ref *tables);
};

#endif /* SQL_CMD_CREATE_TABLE_INCLUDED */
