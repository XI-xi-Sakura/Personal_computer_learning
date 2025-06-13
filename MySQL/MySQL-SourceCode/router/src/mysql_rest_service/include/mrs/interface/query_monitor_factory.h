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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_MONITOR_FACTORY_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_MONITOR_FACTORY_H_

#include <memory>

#include "mrs/database/query_changes_auth_user.h"
#include "mrs/database/query_entries_auth_app.h"
#include "mrs/database/query_entries_content_file.h"
#include "mrs/database/query_entries_content_set.h"
#include "mrs/database/query_entries_db_object.h"
#include "mrs/database/query_entries_db_schema.h"
#include "mrs/database/query_entries_db_service.h"
#include "mrs/database/query_entries_url_host.h"
#include "mrs/database/query_state.h"
#include "mrs/interface/query_factory.h"

namespace mrs {
namespace interface {

class QueryMonitorFactory {
 public:
  virtual ~QueryMonitorFactory() = default;

  virtual std::unique_ptr<database::QueryState> create_turn_state_fetcher(
      const std::optional<uint64_t> &router_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesUrlHost>
  create_url_host_fetcher() = 0;
  virtual std::unique_ptr<database::QueryEntriesDbService>
  create_db_service_fetcher(const std::optional<uint64_t> &router_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesDbSchema>
  create_db_schema_fetcher() = 0;
  virtual std::unique_ptr<database::QueryEntriesDbObject>
  create_db_object_fetcher(QueryFactory *query_factory) = 0;
  virtual std::unique_ptr<database::QueryEntriesAuthApp>
  create_authentication_fetcher() = 0;
  virtual std::unique_ptr<database::QueryEntriesContentFile>
  create_content_file_fetcher() = 0;
  virtual std::unique_ptr<database::QueryEntriesContentSet>
  create_content_set_fetcher() = 0;

  virtual std::unique_ptr<database::QueryEntriesUrlHost>
  create_url_host_monitor(const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesDbService>
  create_db_service_monitor(const uint64_t last_audit_log_id,
                            const std::optional<uint64_t> &router_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesDbSchema>
  create_db_schema_monitor(const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesDbObject>
  create_db_object_monitor(QueryFactory *query_factory,
                           const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesAuthApp>
  create_authentication_monitor(const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryChangesAuthUser>
  create_auth_user_monitor(const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesContentFile>
  create_content_file_monitor(const uint64_t last_audit_log_id) = 0;
  virtual std::unique_ptr<database::QueryEntriesContentSet>
  create_content_set_monitor(const uint64_t last_audit_log_id) = 0;
};

}  // namespace interface
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_INTERFACE_QUERY_MONITOR_FACTORY_H_
