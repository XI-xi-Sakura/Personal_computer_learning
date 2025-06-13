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

#ifndef ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_HELPER_STRING_GENERIC_H_
#define ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_HELPER_STRING_GENERIC_H_

#include <string>

namespace helper {
namespace string {

inline bool is_empty(const std::string &str) { return str.empty(); }
inline bool is_empty(const char *str) { return *str == 0; }
inline const char *cstr(const char *str) { return str; }
inline const char *cstr(const std::string &str) { return str.c_str(); }
inline size_t size(const char *str) { return strlen(str); }
inline size_t size(const std::string &str) { return str.length(); }

}  // namespace string

template <typename Container>
inline std::string as_string(const Container &c) {
  return std::string(c.begin(), c.end());
}

}  // namespace helper

#endif  // ROUTER_SRC_MYSQL_REST_SERVICE_INCLUDE_HELPER_STRING_GENERIC_H_
