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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef PLUGIN_X_SRC_VARIABLES_STATUS_VARIABLES_H_
#define PLUGIN_X_SRC_VARIABLES_STATUS_VARIABLES_H_

#include <string>

#include <mysql/psi/psi_metric.h>
#include "mysql/plugin.h"
#include "plugin/x/src/server/server_properties.h"

namespace xpl {

class Plugin_status_variables {
 public:
  static struct SHOW_VAR m_plugin_status_variables[];
  static PSI_meter_info_v1 m_xpl_meter[];
  static ngs::Server_properties m_properties;

  static std::string get_property(const ngs::Server_property_ids id);
  static size_t get_meter_count();
};

}  // namespace xpl

#endif  // PLUGIN_X_SRC_VARIABLES_STATUS_VARIABLES_H_
