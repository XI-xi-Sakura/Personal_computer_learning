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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_FACTORY_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_FACTORY_H_

#include <memory>

namespace mrs {
namespace database {

class QueryAuditLogEntries;
class QueryEntriesAuthPrivileges;
class QueryEntriesContentFile;
class QueryEntriesAuthApp;
class QueryRestSPMedia;
class QueryEntryGroupRowSecurity;
class QueryEntryContentFile;
class QueryEntryAuthUser;
class QueryEntryObject;
class QueryChangesContentFile;
class QueryUserGroups;
class QueryRestTable;
class QueryRestTableSingleRow;
class QueryRestObjectInsert;
class QueryRestSP;
class QueryTableColumns;
class QueryEntryFields;

}  // namespace database

namespace interface {
class QueryFactory {
 public:
  virtual ~QueryFactory() = default;

  virtual std::shared_ptr<database::QueryAuditLogEntries>
  create_query_audit_log() = 0;
  virtual std::shared_ptr<database::QueryEntriesAuthPrivileges>
  create_query_auth_privileges() = 0;

  virtual std::shared_ptr<database::QueryEntryContentFile>
  create_query_content_file() = 0;
  virtual std::shared_ptr<database::QueryRestSPMedia>
  create_query_sp_media() = 0;
  virtual std::shared_ptr<database::QueryEntryGroupRowSecurity>
  create_query_group_row_security() = 0;
  virtual std::shared_ptr<database::QueryEntryAuthUser>
  create_query_auth_user() = 0;
  virtual std::shared_ptr<database::QueryEntryObject> create_query_object() = 0;

  virtual std::shared_ptr<database::QueryUserGroups>
  create_query_user_groups() = 0;
  virtual std::shared_ptr<database::QueryRestTable> create_query_table() = 0;
  virtual std::shared_ptr<database::QueryRestTableSingleRow>
  create_query_table_single_row(bool encode_bigints_as_string) = 0;
  // virtual std::shared_ptr<database::QueryRestObjectInsert>
  // create_query_object_insert() = 0;
  virtual std::shared_ptr<database::QueryRestSP> create_query_sp() = 0;
  virtual std::shared_ptr<database::QueryEntryFields> create_query_fields() = 0;
};

}  // namespace interface

}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_FACTORY_H_
