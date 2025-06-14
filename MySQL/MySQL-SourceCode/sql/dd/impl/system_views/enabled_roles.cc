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

#include "sql/dd/impl/system_views/enabled_roles.h"

namespace dd::system_views {

const Enabled_roles &Enabled_roles::instance() {
  static auto *s_instance = new Enabled_roles();
  return *s_instance;
}

Enabled_roles::Enabled_roles() {
  m_target_def.set_view_name(view_name());

  m_target_def.add_field(FIELD_ROLE_NAME, "ROLE_NAME", "ROLE_NAME");
  m_target_def.add_field(FIELD_ROLE_HOST, "ROLE_HOST", "ROLE_HOST");
  m_target_def.add_field(
      FIELD_IS_DEFAULT, "IS_DEFAULT",
      " (SELECT IF(COUNT(*), 'YES', 'NO') "
      "   FROM mysql.default_roles "
      "   WHERE DEFAULT_ROLE_USER = ROLE_NAME AND "
      "         CONVERT(DEFAULT_ROLE_HOST using utf8mb4) = ROLE_HOST AND "
      "         USER = INTERNAL_GET_USERNAME() AND "
      "         CONVERT(HOST using utf8mb4) = INTERNAL_GET_HOSTNAME()) ");
  m_target_def.add_field(
      FIELD_IS_MANDATORY, "IS_MANDATORY",
      "IF(INTERNAL_IS_MANDATORY_ROLE(ROLE_NAME, ROLE_HOST), 'YES', 'NO') ");

  m_target_def.add_from(
      "JSON_TABLE(INTERNAL_GET_ENABLED_ROLE_JSON(),"
      " '$[*]' COLUMNS ("
      " ROLE_NAME VARCHAR(255) CHARSET utf8mb4 PATH '$.ROLE_NAME', "
      " ROLE_HOST VARCHAR(255) CHARSET utf8mb4 PATH '$.ROLE_HOST') "
      " ) current_user_enabled_roles");
}

}  // namespace dd::system_views
