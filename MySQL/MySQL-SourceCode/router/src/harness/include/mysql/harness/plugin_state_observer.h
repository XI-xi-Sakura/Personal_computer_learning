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

#ifndef ROUTER_SRC_HARNESS_INCLUDE_MYSQL_HARNESS_PLUGIN_STATE_OBSERVER_H_
#define ROUTER_SRC_HARNESS_INCLUDE_MYSQL_HARNESS_PLUGIN_STATE_OBSERVER_H_

#include <string>
#include <vector>

namespace mysql_harness {

class PluginState;

class PluginStateObserver {
 public:
  virtual ~PluginStateObserver() = default;

  virtual void on_begin_observation(
      [[maybe_unused]] const std::vector<std::string> &active_plugins,
      [[maybe_unused]] const std::vector<std::string> &stopped_plugins) {}
  virtual void on_end_observation() {}

  virtual void on_plugin_register_waitable(
      [[maybe_unused]] const PluginState *state,
      [[maybe_unused]] const std::string &name) {}
  virtual void on_plugin_startup([[maybe_unused]] const PluginState *state,
                                 [[maybe_unused]] const std::string &name) {}
  virtual void on_plugin_shutdown([[maybe_unused]] const PluginState *state,
                                  [[maybe_unused]] const std::string &name) {}
};

}  // namespace mysql_harness

#endif  // ROUTER_SRC_HARNESS_INCLUDE_MYSQL_HARNESS_PLUGIN_STATE_OBSERVER_H_
