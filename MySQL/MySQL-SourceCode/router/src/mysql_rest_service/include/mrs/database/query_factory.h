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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_FACTORY_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_FACTORY_H_

#include "mrs/interface/query_factory.h"

namespace mrs {
namespace database {

using QueryEntryObjectBase = mrs::database::QueryEntryObject;

namespace v2 {

class QueryFactory : public mrs::interface::QueryFactory {
 public:
  std::shared_ptr<QueryAuditLogEntries> create_query_audit_log() override;
  std::shared_ptr<mrs::database::QueryEntriesAuthPrivileges>
  create_query_auth_privileges() override;
  std::shared_ptr<QueryEntryContentFile> create_query_content_file() override;
  std::shared_ptr<QueryRestSPMedia> create_query_sp_media() override;
  std::shared_ptr<QueryEntryGroupRowSecurity> create_query_group_row_security()
      override;
  std::shared_ptr<QueryEntryAuthUser> create_query_auth_user() override;
  std::shared_ptr<QueryEntryObjectBase> create_query_object() override;

  std::shared_ptr<QueryUserGroups> create_query_user_groups() override;
  std::shared_ptr<QueryRestTable> create_query_table() override;
  std::shared_ptr<QueryRestTableSingleRow> create_query_table_single_row(
      bool encode_bigints_as_string) override;
  std::shared_ptr<QueryRestSP> create_query_sp() override;
  std::shared_ptr<database::QueryEntryFields> create_query_fields() override;
};

}  // namespace v2

namespace v3 {

class QueryFactory : public v2::QueryFactory {
 public:
  std::shared_ptr<QueryEntryObjectBase> create_query_object() override;
  std::shared_ptr<QueryEntryAuthUser> create_query_auth_user() override;
};

}  // namespace v3

namespace v4 {

class QueryFactory : public v3::QueryFactory {
 public:
  std::shared_ptr<mrs::database::QueryEntriesAuthPrivileges>
  create_query_auth_privileges() override;

  std::shared_ptr<QueryEntryAuthUser> create_query_auth_user() override;
};

}  // namespace v4
}  // namespace database
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_FACTORY_H_
