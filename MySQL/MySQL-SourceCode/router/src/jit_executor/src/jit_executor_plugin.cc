/*
  Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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

/**
 * connection pool plugin.
 */

#include <array>
#include <chrono>
#include <memory>  // shared_ptr
#include <mutex>
#include <stdexcept>
#include <system_error>

// Harness interface include files
#include "common.h"
#include "mysql/harness/config_option.h"
#include "mysql/harness/config_parser.h"
#include "mysql/harness/dynamic_config.h"
#include "mysql/harness/loader.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/plugin.h"
#include "mysql/harness/plugin_config.h"
#include "mysql/harness/section_config_exposer.h"
#include "mysqlrouter/jit_executor_component.h"
#include "mysqlrouter/jit_executor_options.h"
#include "mysqlrouter/jit_executor_plugin_export.h"
#include "scope_guard.h"

IMPORT_LOG_FUNCTIONS()

template <class T>
using IntOption = mysql_harness::IntOption<T>;

namespace jit_executor {

static constexpr std::string_view kSectionName{"jit_executor"};
static constexpr uint32_t kDefaultMaxGraalVMContexts{8};

class PluginConfig : public mysql_harness::BasePluginConfig {
 public:
  uint32_t max_contexts;

  explicit PluginConfig(const mysql_harness::ConfigSection *section)
      : mysql_harness::BasePluginConfig(section),
        max_contexts(
            get_option(section, kMaxContextPoolSize, IntOption<uint32_t>{})) {}

  std::string get_default(std::string_view option) const override {
    const std::map<std::string_view, std::string> defaults{
        {kMaxContextPoolSize, std::to_string(kDefaultMaxGraalVMContexts)},
    };

    auto it = defaults.find(option);
    return it == defaults.end() ? std::string() : it->second;
  }

  [[nodiscard]] bool is_required(std::string_view /* option */) const override {
    return false;
  }
};

static void init(mysql_harness::PluginFuncEnv *env) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  if (nullptr == info->config) {
    return;
  }

  // assume there is only one section for us
  try {
    int sections{0};

    for (const mysql_harness::ConfigSection *section :
         info->config->sections()) {
      if (section->name != kSectionName) continue;

      if (sections != 0) {
        throw std::invalid_argument("[" + section->name +
                                    (section->key.empty() ? "" : ":") +
                                    section->key + "] already loaded.");
      }

      ++sections;

      PluginConfig config{section};
    }
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());

  } catch (const std::exception &exc) {
    set_error(env, mysql_harness::kRuntimeError, "%s", exc.what());
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");
  }
}

static void deinit(mysql_harness::PluginFuncEnv *) {
  // TODO
}

static constexpr std::array required{
    "logger",
    "io",
};

namespace {

class ConfigExposer : public mysql_harness::SectionConfigExposer {
 public:
  using DC = mysql_harness::DynamicConfig;
  ConfigExposer(bool initial, const PluginConfig &plugin_config,
                const mysql_harness::ConfigSection &default_section)
      : mysql_harness::SectionConfigExposer(initial, default_section,
                                            DC::SectionId{kSectionName, ""}),
        plugin_config_(plugin_config) {}

  void expose() override {
    expose_option(kMaxContextPoolSize, plugin_config_.max_contexts,
                  kDefaultMaxGraalVMContexts, true);
  }

 private:
  const PluginConfig &plugin_config_;
};

}  // namespace

static void expose_configuration(mysql_harness::PluginFuncEnv *env,
                                 const char * /*key*/, bool initial) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  if (!info->config) return;

  for (const mysql_harness::ConfigSection *section : info->config->sections()) {
    if (section->name == kSectionName) {
      PluginConfig config{section};
      ConfigExposer(initial, config, info->config->get_default_section())
          .expose();
    }
  }
}

extern "C" {
mysql_harness::Plugin JIT_EXECUTOR_PLUGIN_EXPORT harness_plugin_jit_executor = {
    mysql_harness::PLUGIN_ABI_VERSION,       // abi-version
    mysql_harness::ARCHITECTURE_DESCRIPTOR,  // arch
    "jit_executor",                          // name
    VERSION_NUMBER(0, 0, 1),
    // requires
    required.size(),
    required.data(),
    // conflicts
    0,
    nullptr,
    init,     // init
    deinit,   // deinit
    nullptr,  // start
    nullptr,  // stop
    false,    // declares_readiness
    options.size(),
    options.data(),
    expose_configuration,
};
}
}  // namespace jit_executor
