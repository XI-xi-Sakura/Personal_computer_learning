/* Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

#include "plugin/x/src/udf/registrator.h"

#include <memory>
#include <stdexcept>

#include "plugin/x/src/services/service_udf_registration.h"
#include "plugin/x/src/xpl_log.h"

namespace xpl::udf {

Registrator::Registrator() {
  m_udf_registrator = std::make_unique<Service_udf_registration>(&m_registry);
}

void Registrator::registration(const Record &r, Name_registry *udf_names) {
  if (!m_udf_registrator->udf_register(r.m_name, r.m_result, r.m_func,
                                       r.m_func_init, r.m_func_deinit))
    throw std::runtime_error(std::string("Can't register '") + r.m_name +
                             "' user defined function");
  udf_names->insert(r.m_name);
}

bool Registrator::unregistration(const std::string &udf_name) {
  int was_present = 0;
  if (!m_udf_registrator->udf_unregister(udf_name.c_str(), &was_present)) {
    log_error(ER_XPLUGIN_FAILED_TO_UNREGISTER_UDF, udf_name.c_str());
    return false;
  }
  return true;
}

void Registrator::unregistration(Name_registry *udf_names) {
  for (auto i = udf_names->begin(); i != udf_names->end();)
    if (unregistration(*i))
      i = udf_names->erase(i);
    else
      ++i;
}

}  // namespace xpl::udf
