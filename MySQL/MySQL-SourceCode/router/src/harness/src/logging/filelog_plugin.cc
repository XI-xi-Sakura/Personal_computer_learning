/*
  Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#include "filelog_plugin.h"
#include "mysql/harness/logging/logger_plugin.h"
#include "mysql/harness/logging/supported_logger_options.h"

extern "C" {
mysql_harness::Plugin harness_plugin_filelog = {
    mysql_harness::PLUGIN_ABI_VERSION,
    mysql_harness::ARCHITECTURE_DESCRIPTOR,
    "Logging using file",
    VERSION_NUMBER(0, 0, 1),
    0,
    nullptr,  // Requires
    0,
    nullptr,  // Conflicts
    nullptr,  // init
    nullptr,  // deinit
    nullptr,  // start
    nullptr,  // stop
    false,    // declares_readiness
    logger_sink_supported_options.size(),
    logger_sink_supported_options.data(),
    nullptr,  // expose_configuration
};
}
