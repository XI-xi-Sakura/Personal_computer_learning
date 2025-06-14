/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mysql_connection_attributes_iterator_imp.h"
#include <sql/sql_class.h>

DEFINE_BOOL_METHOD(mysql_connection_attributes_iterator_imp::init,
                   (MYSQL_THD opaque_thd,
                    my_h_connection_attributes_iterator *iterator)) {
  try {
    THD *thd;
    if (opaque_thd != nullptr)
      thd = reinterpret_cast<THD *>(opaque_thd);
    else
      thd = current_thd;
    if (iterator != nullptr && thd->m_connection_attributes.size() > 0) {
      *iterator = thd->m_connection_attributes.data();
      return false;  // Success
    } else
      return true; /* invalid option */
  } catch (...) {
    return true;
  }
}

DEFINE_BOOL_METHOD(mysql_connection_attributes_iterator_imp::deinit,
                   (my_h_connection_attributes_iterator iterator)) {
  if (iterator != nullptr) {
    // nothing to do
    return false;  // Success
  } else
    return true; /* invalid option */
}

/**
  Helper function to decode lenght-encoded string and advance to the next
  length-encoded part.
*/
std::tuple<const char *, size_t, bool> parse_length_encoded_string(
    const char **str, std::vector<char> *connection_attributes_vector) {
  size_t length;
  const char *result;

  const auto &connection_attributes = connection_attributes_vector->data();
  size_t connection_attributes_length = connection_attributes_vector->size();

  // check if we're in the connection attributes string boundaries
  if (*str < connection_attributes || *str > connection_attributes +
                                                 connection_attributes_length -
                                                 sizeof(char))
    return std::make_tuple(nullptr, 0, false);

  // The first item of the string is the length of the attribute
  length = **str;
  result = *str + 1;

  /* Check if we're still in the connection attributes string boundaries.
     Note: the original condition was:
     if (*str + length > connection_attributes + connection_attributes_length)
     but it lead to pointer overflow. Now it cannot overflow.
         Note2: 5 lines above we make sure *str > connection_attributes
    */
  if (length > connection_attributes_length ||
      static_cast<size_t>(*str - connection_attributes) >
          connection_attributes_length - length)
    return std::make_tuple(nullptr, 0, false);

  // Advance the iterator with the parsed string length, plus the extra
  // length byte
  *str += length + 1;

  return std::make_tuple(result, length, true);
}

DEFINE_BOOL_METHOD(mysql_connection_attributes_iterator_imp::get,
                   (MYSQL_THD opaque_thd,
                    my_h_connection_attributes_iterator *iterator,
                    const char **name, size_t *name_length, const char **value,
                    size_t *value_length, const char **client_charset)) {
  try {
    THD *thd;
    if (opaque_thd != nullptr)
      thd = reinterpret_cast<THD *>(opaque_thd);
    else
      thd = current_thd;

    if (name != nullptr && name_length != nullptr && value != nullptr &&
        value_length != nullptr && iterator != nullptr) {
      bool isParsingSuccessful;
      std::tie(*name, *name_length, isParsingSuccessful) =
          parse_length_encoded_string(iterator, &thd->m_connection_attributes);

      if (!isParsingSuccessful) return true;

      std::tie(*value, *value_length, isParsingSuccessful) =
          parse_length_encoded_string(iterator, &thd->m_connection_attributes);

      if (!isParsingSuccessful) return true;

      *client_charset = thd->variables.character_set_client->m_coll_name;

      return false;  // Success
    } else
      return true; /* invalid option */
  } catch (...) {
    return true;
  }
}
