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

#ifndef ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_MAP_H_
#define ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_MAP_H_

namespace helper {
namespace container {

template <typename Container>
typename Container::mapped_type get_value_default(
    const Container &container, const typename Container::key_type &key,
    typename Container::mapped_type &&default_value) {
  auto it = container.find(key);

  if (it == container.end()) {
    return default_value;
  }

  return it->second;
}

template <typename Container>
bool get_value(Container &container, const typename Container::key_type &key,
               typename Container::mapped_type *out) {
  auto it = container.find(key);

  if (it == container.end()) {
    return false;
  }

  *out = it->second;

  return true;
}

template <typename Container, typename Output>
bool get_value_other(Container &container,
                     const typename Container::key_type &key, Output *out) {
  auto it = container.find(key);

  if (it == container.end()) {
    return false;
  }

  *out = it->second;

  return true;
}

template <typename Container, typename Output>
bool get_value_other(Container &container,
                     const typename Container::key_type &key, Output out) {
  auto it = container.find(key);

  if (it == container.end()) {
    return false;
  }

  out = it->second;

  return true;
}

}  // namespace container
}  // namespace helper

#endif  // ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_MAP_H_
