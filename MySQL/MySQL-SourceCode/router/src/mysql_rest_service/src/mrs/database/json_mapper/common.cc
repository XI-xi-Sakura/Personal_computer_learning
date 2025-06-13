/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is designed to work with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms,
 * as designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have either included with
 * the program or referenced in the documentation.
 *
 * This program is distributed in the hope that it will be useful,  but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 * the GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "router/src/mysql_rest_service/src/mrs/database/json_mapper/common.h"
#include "mrs/interface/rest_error.h"
#include "mysqlrouter/base64.h"

namespace mrs {
namespace database {
namespace dv {

using interface::RestError;

mysqlrouter::sqlstring join_sqlstrings(
    const std::vector<mysqlrouter::sqlstring> &strings,
    const std::string &sep) {
  mysqlrouter::sqlstring str;
  for (const auto &s : strings) {
    str.append_preformatted_sep(sep, s);
  }
  return str;
}

PrimaryKeyColumnValues ref_primary_key(const ForeignKeyReference &ref,
                                       const rapidjson::Value &value,
                                       bool throw_if_missing_or_null) {
  PrimaryKeyColumnValues pk;
  bool found = true;

  auto object = value.GetObject();
  ref.ref_table->foreach_field<Column, bool>(
      [&pk, &found, object](const Column &column) {
        if (column.is_primary) {
          auto m = object.FindMember(column.name.c_str());
          if (m == object.MemberEnd() || m->value.IsNull()) {
            found = false;
            return false;
          }
          auto tmp = mysqlrouter::sqlstring("?");
          if (column.type == entry::ColumnType::BINARY && m->value.IsString())
            tmp << Base64::decode(m->value.GetString());
          else
            tmp << m->value;
          pk[column.column_name] = std::move(tmp);
        }
        return false;
      });

  if (!found) {
    if (throw_if_missing_or_null) {
      throw_missing_id(ref.ref_table->table);
    }
    return {};
  }

  return pk;
}

void validate_primary_key_values(const entry::JsonMapping &view,
                                 const ObjectRowOwnership &row_ownership,
                                 PrimaryKeyColumnValues &pk_values) {
  auto pk_cols = view.primary_key();

  for (const auto &col : pk_cols) {
    if (pk_values.find(col->column_name) == pk_values.end()) {
      if (!row_ownership.is_owner_id(view, *col))
        throw RestError("Missing primary key column value for " +
                        col->column_name);
      else
        pk_values[col->column_name] = row_ownership.owner_user_id();
    }
  }

  if (std::any_of(pk_values.begin(), pk_values.end(), [pk_cols](const auto &c) {
        return std::find_if(pk_cols.begin(), pk_cols.end(),
                            [c](const auto &col) {
                              return c.first == col->column_name;
                            }) == pk_cols.end();
      })) {
    throw RestError("Invalid primary key column");
  }
}

}  // namespace dv
}  // namespace database
}  // namespace mrs
