/*
  Copyright (c) 2022, 2025, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mrs/database/query_factory.h"

#include <memory>

#include "mrs/database/helper/query_table_columns.h"
#include "mrs/database/query_changes_auth_app.h"
#include "mrs/database/query_changes_content_file.h"
#include "mrs/database/query_changes_db_object.h"
#include "mrs/database/query_entries_audit_log.h"
#include "mrs/database/query_entries_auth_app.h"
#include "mrs/database/query_entries_auth_privileges.h"
#include "mrs/database/query_entries_content_file.h"
#include "mrs/database/query_entries_db_object.h"
#include "mrs/database/query_entry_auth_user.h"
#include "mrs/database/query_entry_content_file.h"
#include "mrs/database/query_entry_fields.h"
#include "mrs/database/query_entry_group_row_security.h"
#include "mrs/database/query_entry_object.h"
#include "mrs/database/query_rest_sp.h"
#include "mrs/database/query_rest_sp_media.h"
#include "mrs/database/query_rest_table.h"
#include "mrs/database/query_rest_table_single_row.h"
#include "mrs/database/query_user_groups.h"

namespace mrs {
namespace database {
namespace v2 {

std::shared_ptr<QueryAuditLogEntries> QueryFactory::create_query_audit_log() {
  return std::make_shared<QueryAuditLogEntries>();
}

std::shared_ptr<QueryEntriesAuthPrivileges>
QueryFactory::create_query_auth_privileges() {
  return std::make_shared<v_2_3::QueryEntriesAuthPrivileges>();
}

std::shared_ptr<QueryRestSPMedia> QueryFactory::create_query_sp_media() {
  return std::make_shared<QueryRestSPMedia>();
}

std::shared_ptr<QueryEntryGroupRowSecurity>
QueryFactory::create_query_group_row_security() {
  return std::make_shared<QueryEntryGroupRowSecurity>();
}

std::shared_ptr<QueryEntryContentFile>
QueryFactory::create_query_content_file() {
  return std::make_shared<QueryEntryContentFile>();
}

std::shared_ptr<QueryEntryAuthUser> QueryFactory::create_query_auth_user() {
  return std::make_shared<QueryEntryAuthUser>(std::make_shared<QueryFactory>());
}

std::shared_ptr<QueryEntryObjectBase> QueryFactory::create_query_object() {
  return std::make_shared<mrs::database::v2::QueryEntryObject>();
}

std::shared_ptr<QueryUserGroups> QueryFactory::create_query_user_groups() {
  return std::make_shared<QueryUserGroups>();
}

std::shared_ptr<QueryRestTable> QueryFactory::create_query_table() {
  return std::make_shared<QueryRestTable>();
}

std::shared_ptr<QueryRestTableSingleRow>
QueryFactory::create_query_table_single_row(bool encode_bigints_as_string) {
  return std::make_shared<QueryRestTableSingleRow>(nullptr,
                                                   encode_bigints_as_string);
}

std::shared_ptr<QueryRestSP> QueryFactory::create_query_sp() {
  return std::make_shared<QueryRestSP>();
}

std::shared_ptr<database::QueryEntryFields>
QueryFactory::create_query_fields() {
  return std::make_shared<QueryEntryFields>();
}

}  // namespace v2

namespace v3 {

std::shared_ptr<QueryEntryObjectBase> QueryFactory::create_query_object() {
  return std::make_shared<QueryEntryObject>();
}

std::shared_ptr<QueryEntryAuthUser> QueryFactory::create_query_auth_user() {
  return std::make_shared<QueryEntryAuthUser>(std::make_shared<QueryFactory>());
}

}  // namespace v3

namespace v4 {

std::shared_ptr<mrs::database::QueryEntriesAuthPrivileges>
QueryFactory::create_query_auth_privileges() {
  return std::make_shared<v4::QueryEntriesAuthPrivileges>();
}

std::shared_ptr<QueryEntryAuthUser> QueryFactory::create_query_auth_user() {
  return std::make_shared<QueryEntryAuthUser>(std::make_shared<QueryFactory>());
}

}  // namespace v4

}  // namespace database
}  // namespace mrs
