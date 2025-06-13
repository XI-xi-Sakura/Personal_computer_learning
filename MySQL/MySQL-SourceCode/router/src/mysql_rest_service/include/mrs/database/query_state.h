/*
 Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_STATE_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_STATE_H_

#include <string>

#include "mrs/database/entry/db_state.h"
#include "mrs/database/helper/query.h"

namespace mrs {
namespace database {

class QueryState : public Query {
 public:
  using DbState = mrs::database::entry::DbState;

 public:
  QueryState(const std::optional<uint64_t> &router_id);

  virtual void query_state(MySQLSession *session);
  virtual bool was_changed() const;

  const DbState &get_state() const;

  class NoRows : public std::runtime_error {
   public:
    explicit NoRows(const std::string &msg) : std::runtime_error(msg) {}
  };

 protected:
  void query_state_impl(MySQLSession *session,
                        MySQLSession::Transaction *transaction);
  void on_row(const ResultRow &r) override;

  DbState state_;
  bool changed_{true};
  bool has_rows_{false};
  std::optional<uint64_t> router_id_;
};

}  // namespace database
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_STATE_H_
