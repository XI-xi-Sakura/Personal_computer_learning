/*
 Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_REST_FUNCTION_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_REST_FUNCTION_H_

#include <memory>
#include <string>
#include <vector>

#include "helper/json/serializer_to_text.h"
#include "helper/mysql_column.h"
#include "mrs/database/entry/object.h"
#include "mrs/database/helper/query.h"
#include "mysqlrouter/utils_sqlstring.h"

namespace mrs {
namespace database {

class QueryRestFunction : private Query {
  using Row = Query::Row;
  using JsonType = helper::JsonType;
  using ParametersValues = std::vector<mysqlrouter::sqlstring>;

 public:
  using CustomMetadata = std::map<std::string, std::string>;
  QueryRestFunction(bool encode_bigints_as_strings = false)
      : encode_bigints_as_strings_{encode_bigints_as_strings} {}

  virtual void query_raw(MySQLSession *session,
                         std::shared_ptr<entry::Object> object,
                         const ParametersValues &values = {});
  virtual void query_entries(MySQLSession *session,
                             std::shared_ptr<entry::Object> object,
                             const ParametersValues &values = {});
  virtual void serialize_response(const CustomMetadata &custom_metadata = {});

  const char *get_sql_state();
  std::string response;
  uint64_t items{0};
  bool store_raw_{false};

 protected:
  enum_field_types mysql_type_{MYSQL_TYPE_NULL};
  JsonType json_type_{JsonType::kNull};
  bool encode_bigints_as_strings_;
  helper::json::SerializerToText serializer_;
  helper::json::SerializerToText::Object json_root_;

  void query_entries_impl(MySQLSession *session,
                          std::shared_ptr<entry::Object> object,
                          const ParametersValues &values = {});
  void on_row(const ResultRow &r) override;
  void on_metadata(unsigned int number, MYSQL_FIELD *fields) override;
};

}  // namespace database
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_DATABASE_QUERY_REST_FUNCTION_H_
