/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#include <memory>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <components/keyrings/keyring_file/keyring_file.h>

#include <components/keyrings/common/config/config_reader.h> /* Config_reader */
#include <include/mysql/components/component_implementation.h>

using keyring_common::config::Config_reader;
using keyring_file::g_config_pod;

/**
  In order to locate a shared library, we need it to export at least
  one symbol. This way dlsym/GetProcAddress will be able to find it.
*/
DLL_EXPORT int keyring_file_component_exported_symbol() { return 0; }

namespace keyring_file::config {

char *g_component_path = nullptr;
char *g_instance_path = nullptr;

/* Component metadata */
static const char *s_component_metadata[][2] = {
    {"Component_name", "component_keyring_file"},
    {"Author", "Oracle Corporation"},
    {"License", "GPL"},
    {"Implementation_name", "component_keyring_file"},
    {"Version", "1.0"}};

/* Config file name */
const std::string config_file_name = "component_keyring_file.cnf";

/* Config names */
const std::string config_options[] = {"read_local_config", "path", "read_only"};

template <typename T>
bool get_mandatory_element(const std::unique_ptr<Config_reader> &config_reader,
                           const std::string &element_name, T &element_value,
                           std::string &err) {
  if (config_reader->get_element(element_name, element_value)) {
    err = "Could not find '" + element_name + "' value in configuration file";
    return true;
  }
  return false;
}

bool find_and_read_config_file(std::unique_ptr<Config_pod> &config_pod,
                               std::string &err) {
  config_pod = std::make_unique<Config_pod>();

  /* Get shared library location */
  std::string path(g_component_path);

  auto set_config_path = [](std::string &full_path) -> bool {
    if (full_path.length() == 0) return true;
#ifdef _WIN32
    full_path += "\\";
#else
    full_path += "/";
#endif
    full_path.append(config_file_name);
    return false;
  };
  if (set_config_path(path)) {
    err = "Failed to set path to configuration file";
    return true;
  }
  /* Read config file that's located at shared library location */
  std::unique_ptr<Config_reader> config_reader(new (std::nothrow)
                                                   Config_reader(path));
  if (!config_reader->is_valid(err)) goto error;
  {
    bool read_local_config = false;
    if (!config_reader->get_element<bool>(config_options[0],
                                          read_local_config)) {
      if (read_local_config) {
        config_reader.reset();
        /*
          Read config file from current working directory
          We assume that when control reaches here, binary has set
          current working directory appropriately.
        */
        std::string instance_path(g_instance_path);
        if (set_config_path(instance_path)) instance_path = config_file_name;
        config_reader = std::make_unique<Config_reader>(instance_path);
        if (!config_reader->is_valid(err)) goto error;
      }
    }
  }
  if (get_mandatory_element(config_reader, config_options[1],
                            config_pod->config_file_path_, err))
    goto error;
  if (get_mandatory_element(config_reader, config_options[2],
                            config_pod->read_only_, err))
    goto error;
  return false;
error:
  config_pod.reset();
  return true;
}

bool create_config(
    std::unique_ptr<std::vector<std::pair<std::string, std::string>>>
        &metadata) {
  metadata =
      std::make_unique<std::vector<std::pair<std::string, std::string>>>();
  if (metadata == nullptr) return true;
  keyring_file::config::Config_pod config_pod;
  bool global_config_available = false;
  if (g_config_pod != nullptr) {
    config_pod = *g_config_pod;
    global_config_available = true;
  }

  for (const auto *entry : keyring_file::config::s_component_metadata) {
    metadata->push_back(std::make_pair(entry[0], entry[1]));
  }

  /* Status */
  metadata->push_back(std::make_pair(
      "Component_status",
      keyring_file::g_component_callbacks->keyring_initialized() ? "Active"
                                                                 : "Disabled"));

  /* Backend file */
  metadata->push_back(std::make_pair(
      "Data_file",
      (global_config_available ? ((config_pod.config_file_path_.length() == 0)
                                      ? "<NONE>"
                                      : config_pod.config_file_path_)
                               : "<NOT APPLICABLE>")));

  /* Read only flag */
  metadata->push_back(std::make_pair(
      "Read_only",
      (global_config_available ? (config_pod.read_only_ ? "Yes" : "No")
                               : "<NOT APPLICABLE>")));

  return false;
}

}  // namespace keyring_file::config
