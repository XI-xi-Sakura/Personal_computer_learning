/*
  Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_MRS_DATABASE_ENTRY_DB_SERVICE_H_
#define ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_MRS_DATABASE_ENTRY_DB_SERVICE_H_

#include <optional>
#include <set>
#include <string>

#include "mrs/database/entry/entry.h"
#include "mrs/database/entry/universal_id.h"

namespace mrs {
namespace database {
namespace entry {

struct DbService {
  UniversalId id;
  std::string name;
  UniversalId url_host_id;
  std::string url_context_root;
  std::set<std::string> url_protocols;
  std::optional<std::string> in_development;
  std::optional<std::string> comment;
  std::optional<std::string> options;
  std::optional<std::string> auth_path;
  std::optional<std::string> auth_completed_url;
  std::optional<std::string> auth_completed_url_validation;
  std::optional<std::string> auth_completed_page_content;
  bool enable_sql_endpoint;
  std::optional<std::string> custom_metadata_schema;
  EnabledType enabled;
  bool published{true};
  std::optional<std::string> metadata;
  bool passthrough_db_user{false};

  bool deleted{false};
};

}  // namespace entry
}  // namespace database
}  // namespace mrs

#endif  // ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_MRS_DATABASE_ENTRY_DB_SERVICE_H_
