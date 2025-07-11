/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#include "mysql_component_imp.h"

#include <cstddef>
#include <utility>
#include <vector>

mysql_component::mysql_component(mysql_component_t *component_data,
                                 my_string urn)
    : m_component_data(component_data), m_urn(std::move(urn)) {
  for (const mysql_metadata_ref_t *metadata_iterator = component_data->metadata;
       metadata_iterator->key != nullptr; ++metadata_iterator) {
    this->set_value(metadata_iterator->key, metadata_iterator->value);
  }
}

/**
  Gets name of this component.

  @return Pointer to component name. Pointer is valid till component is not
    unloaded.
*/
const char *mysql_component::name_c_str() const {
  return m_component_data->name;
}

/**
  Gets original URN used to load this component.

  @return Pointer to URN. Pointer is valid till component is not unloaded.
*/
const char *mysql_component::urn_c_str() const { return m_urn.c_str(); }

/**
  Gets original URN used to load this component.

  @return Reference to string object with URN. Pointer is valid till component
    is not unloaded.
*/
const my_string &mysql_component::get_urn() const { return m_urn; }

/**
  Gets list of all service implementations provided by this component.

  @return List of service implementations.
*/
std::vector<const mysql_service_ref_t *>
mysql_component::get_provided_services() const {
  std::vector<const mysql_service_ref_t *> res;
  for (mysql_service_ref_t *implementation_it = m_component_data->provides;
       implementation_it->implementation != nullptr; ++implementation_it) {
    res.push_back(implementation_it);
  }
  return res;
}

/**
  Gets list of services required by this component to work.

  @return List of service names.
*/
std::vector<mysql_service_placeholder_ref_t *>
mysql_component::get_required_services() const {
  std::vector<mysql_service_placeholder_ref_t *> res;
  for (mysql_service_placeholder_ref_t *implementation_it =
           m_component_data->requires_service;
       implementation_it->name != nullptr; ++implementation_it) {
    res.push_back(implementation_it);
  }
  return res;
}

/**
  Gets underlying component data structure.

  @return Component data structure.
*/
const mysql_component_t *mysql_component::get_data() const {
  return m_component_data;
}
