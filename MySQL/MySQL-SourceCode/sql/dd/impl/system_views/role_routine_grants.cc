/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include "sql/dd/impl/system_views/role_routine_grants.h"
#include "sql/dd/impl/system_views/applicable_roles.h"

namespace dd::system_views {

const Role_routine_grants &Role_routine_grants::instance() {
  static auto *s_instance = new Role_routine_grants();
  return *s_instance;
}

Role_routine_grants::Role_routine_grants() {
  m_target_def.set_view_name(view_name());

  m_target_def.add_distinct();

  m_target_def.add_cte_expression(Applicable_roles::cte_expression());

  m_target_def.add_field(FIELD_GRANTOR, "GRANTOR",
                         "INTERNAL_GET_USERNAME(Grantor)");
  m_target_def.add_field(FIELD_GRANTOR_HOST, "GRANTOR_HOST",
                         "INTERNAL_GET_HOSTNAME(Grantor)");
  m_target_def.add_field(FIELD_GRANTEE, "GRANTEE", "pp.User");
  m_target_def.add_field(FIELD_GRANTEE_HOST, "GRANTEE_HOST", "pp.Host");
  m_target_def.add_field(FIELD_SPECIFIC_CATALOG, "SPECIFIC_CATALOG", "'def'");
  m_target_def.add_field(FIELD_SPECIFIC_SCHEMA, "SPECIFIC_SCHEMA", "Db");
  m_target_def.add_field(FIELD_SPECIFIC_NAME, "SPECIFIC_NAME", "Routine_name");
  m_target_def.add_field(FIELD_ROUTINE_CATALOG, "ROUTINE_CATALOG", "'def'");
  m_target_def.add_field(FIELD_ROUTINE_SCHEMA, "ROUTINE_SCHEMA", "Db");
  m_target_def.add_field(FIELD_ROUTINE_NAME, "ROUTINE_NAME", "Routine_name");
  m_target_def.add_field(FIELD_PRIVILEGE_TYPE, "PRIVILEGE_TYPE", "Proc_priv");
  m_target_def.add_field(FIELD_IS_GRANTABLE, "IS_GRANTABLE",
                         "IF(FIND_IN_SET('Grant',Proc_priv)>0, 'YES', 'NO')");

  m_target_def.add_from("mysql.procs_priv pp");
  m_target_def.add_from(
      " JOIN role_graph rg ON "
      " pp.User = rg.c_from_user AND "
      "  CONVERT(pp.Host using utf8mb4) = rg.c_from_host");

  m_target_def.add_where("pp.Proc_priv > 0");
  m_target_def.add_where(" AND c_to_user != ''");
  m_target_def.add_where(" AND c_enabled = TRUE");
}

}  // namespace dd::system_views
