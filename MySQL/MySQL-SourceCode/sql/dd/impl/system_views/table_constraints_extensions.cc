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

#include "sql/dd/impl/system_views/table_constraints_extensions.h"

#include <string>

#include "sql/dd/string_type.h"
#include "string_with_len.h"

namespace {
enum {
  FIELD_CONSTRAINT_CATALOG,
  FIELD_CONSTRAINT_SCHEMA,
  FIELD_CONSTRAINT_NAME,
  FIELD_TABLE_NAME,
  FIELD_ENGINE_ATTRIBUTE,
  FIELD_SECONDARY_ENGINE_ATTRIBUTE
};

const dd::String_type s_view_name{
    STRING_WITH_LEN("TABLE_CONSTRAINTS_EXTENSIONS")};
const dd::system_views::Table_constraints_extensions *s_instance =
    new dd::system_views::Table_constraints_extensions(s_view_name);

}  // namespace

namespace dd::system_views {

const Table_constraints_extensions &Table_constraints_extensions::instance() {
  return *s_instance;
}

Table_constraints_extensions::Table_constraints_extensions(
    const dd::String_type &n) {
  m_target_def.set_view_name(n);

  // SELECT Identifier
  m_target_def.add_field(FIELD_CONSTRAINT_CATALOG, "CONSTRAINT_CATALOG",
                         "cat.name" + m_target_def.fs_name_collation());
  m_target_def.add_field(FIELD_CONSTRAINT_SCHEMA, "CONSTRAINT_SCHEMA",
                         "sch.name" + m_target_def.fs_name_collation());
  m_target_def.add_field(FIELD_TABLE_NAME, "TABLE_NAME",
                         "tbl.name" + m_target_def.fs_name_collation());
  m_target_def.add_field(FIELD_CONSTRAINT_NAME, "CONSTRAINT_NAME", "idx.name");

  // SELECT extension fields
  m_target_def.add_field(FIELD_ENGINE_ATTRIBUTE, "ENGINE_ATTRIBUTE",
                         "idx.engine_attribute");

  m_target_def.add_field(FIELD_SECONDARY_ENGINE_ATTRIBUTE,
                         "SECONDARY_ENGINE_ATTRIBUTE",
                         "idx.secondary_engine_attribute");

  // FROM
  m_target_def.add_from("mysql.indexes idx");
  m_target_def.add_from("JOIN mysql.tables tbl ON idx.table_id=tbl.id");
  m_target_def.add_from("JOIN mysql.schemata sch ON tbl.schema_id=sch.id");
  m_target_def.add_from(
      "JOIN mysql.catalogs cat ON "
      "cat.id=sch.catalog_id"
      // Need to drop this condition since we want engine attributes
      // for all indexes, not just those that are table
      // constraints.
      //" AND idx.type IN ('PRIMARY', 'UNIQUE')"
  );

  // WHERE
  m_target_def.add_where("CAN_ACCESS_TABLE(sch.name, tbl.name)");
  m_target_def.add_where(
      "AND IS_VISIBLE_DD_OBJECT(tbl.hidden, FALSE, idx.options)");
}

const dd::String_type &Table_constraints_extensions::view_name() {
  return s_view_name;
}
}  // namespace dd::system_views
