/* Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#include <cstdio>
#include "mysql/components/component_implementation.h"
#include "mysql/components/service_implementation.h"
#include "mysql/components/services/pfs_plugin_table_service.h"

namespace pfs_plugin_table_v1_all_empty {

DEFINE_METHOD(int, add_tables,
              (PFS_engine_table_share_proxy * * /*st_share*/,
               unsigned int /*share_count*/)) {
  return 0;
}

DEFINE_METHOD(int, delete_tables,
              (PFS_engine_table_share_proxy * * /*st_share*/,
               unsigned int /*share_count*/)) {
  return 0;
}
DEFINE_METHOD(unsigned int, get_parts_found, (PSI_key_reader * /*reader*/)) {
  return 0;
}

}  // namespace pfs_plugin_table_v1_all_empty

BEGIN_SERVICE_IMPLEMENTATION(HARNESS_COMPONENT_NAME, pfs_plugin_table_v1)
pfs_plugin_table_v1_all_empty::add_tables,
    pfs_plugin_table_v1_all_empty::delete_tables,
    pfs_plugin_table_v1_all_empty::get_parts_found END_SERVICE_IMPLEMENTATION();
