/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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

#include "NdbConfig.hpp"
#include <NdbConfig.h>
#include <mgmapi.h>
#include <mgmapi_config_parameters.h>
#include <ndb_global.h>
#include <ndb_version.h>
#include <ConfigRetriever.hpp>
#include <NDBT_Output.hpp>
#include <NdbOut.hpp>
#include <mgmapi_configuration.hpp>

bool NdbConfig::getHostName(unsigned int node_id, const char **hostname) {
  const ndb_mgm_configuration *conf = getConfig();
  if (conf == nullptr) {
    return false;
  }

  /**
   * Setup cluster configuration data
   */
  ndb_mgm_configuration_iterator iter(conf, CFG_SECTION_NODE);
  if (iter.find(CFG_NODE_ID, node_id)) {
    ndbout << "Invalid configuration fetched, DB missing" << endl;
    return false;
  }

  if (iter.get(CFG_NODE_HOST, hostname)) {
    ndbout << "Host not found" << endl;
    return false;
  }

  return true;
}

bool NdbConfig::getProperty(unsigned nodeid, unsigned type, unsigned key,
                            Uint32 *val) {
  const ndb_mgm_configuration *conf = getConfig();
  if (conf == nullptr) {
    return false;
  }

  /**
   * Setup cluster configuration data
   */
  ndb_mgm_configuration_iterator iter(conf, CFG_SECTION_NODE);
  if (iter.find(CFG_NODE_ID, nodeid)) {
    ndbout << "Invalid configuration fetched, DB missing" << endl;
    return false;
  }

  unsigned _type;
  if (iter.get(CFG_TYPE_OF_SECTION, &_type) || type != _type) {
    ndbout << "No such node in configuration" << endl;
    return false;
  }

  if (iter.get(key, val)) {
    ndbout << "No such key: " << key << " in configuration" << endl;
    return false;
  }

  return true;
}
