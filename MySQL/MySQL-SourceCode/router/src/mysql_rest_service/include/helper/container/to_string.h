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

#ifndef ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_TO_STRING_H_
#define ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_TO_STRING_H_

#include <cassert>
#include <string>

#include "helper/to_string.h"

namespace helper {

class DummyType {};

inline std::string to_string(const DummyType &) {
  assert(false && "Not usable, defined just to compile the code below.");
  return {};
}

namespace container {

template <typename PairFirst, typename PairSecond>
std::string to_string(const std::pair<PairFirst, PairSecond> &pair) {
  using helper::to_string;
  using helper::container::to_string;
  using std::to_string;
  std::string result;

  result += "(" + to_string(pair.first) + "," + to_string(pair.second) + ")";

  return result;
}

template <typename Container>
std::string to_string(const Container &container) {
  using helper::to_string;
  using helper::container::to_string;
  using std::to_string;
  std::string result;
  bool first = true;
  for (const auto &element : container) {
    if (!first) {
      result += ',';
    }
    result += to_string(element);
    first = false;
  }

  return result;
}

}  // namespace container
}  // namespace helper

#endif  // ROUTER_SRC_REST_MRS_SRC_HELPER_CONTAINER_TO_STRING_H_
