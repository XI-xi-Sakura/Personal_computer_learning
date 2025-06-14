/*
  Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQL_HARNESS_STDX_TYPE_TRAITS_H_
#define MYSQL_HARNESS_STDX_TYPE_TRAITS_H_

#include <type_traits>

namespace stdx {

// from C++23
// wg21.link/P1048

// all non-enums are also non-scoped-enums
template <class T, bool B = std::is_enum_v<T>>
struct __is_scoped_enum_helper : std::false_type {};

// scoped enums are enum's that can't be automatically be converted into its
// underlying type.
template <class T>
struct __is_scoped_enum_helper<T, true>
    : std::bool_constant<!std::is_convertible_v<T, std::underlying_type_t<T>>> {
};

template <class T>
struct is_scoped_enum : __is_scoped_enum_helper<T> {};

template <class E>
inline constexpr bool is_scoped_enum_v = is_scoped_enum<E>::value;

// C++20
template <class T>
struct type_identity {
  using type = T;
};

template <class T>
using type_identity_t = typename type_identity<T>::type;

}  // namespace stdx

#endif
