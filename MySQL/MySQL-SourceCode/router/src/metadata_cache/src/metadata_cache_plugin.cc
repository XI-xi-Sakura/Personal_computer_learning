/*
  Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif
#include <array>
#include <stdexcept>
#include <string>

#include "keyring/keyring_manager.h"
#include "metadata_cache.h"
#include "my_thread.h"  // my_thread_self_setname
#include "mysql/harness/config_parser.h"
#include "mysql/harness/dynamic_config.h"
#include "mysql/harness/loader_config.h"
#include "mysql/harness/logging/logging.h"
#include "mysqlrouter/mysql_client_thread_token.h"
#include "mysqlrouter/mysql_session.h"  // kSslModePreferred
#include "mysqlrouter/supported_metadata_cache_options.h"
#include "mysqlrouter/uri.h"
#include "plugin_config.h"

IMPORT_LOG_FUNCTIONS()

static const mysql_harness::AppInfo *g_app_info;
static const std::string kSectionName = "metadata_cache";
static const char *kKeyringAttributePassword = "password";

metadata_cache::RouterAttributes g_router_attributes;

static metadata_cache::RouterAttributes get_router_attributes(
    const mysql_harness::Config *cfg) {
  metadata_cache::RouterAttributes result;

  if (!cfg->has_any("routing")) return result;
  for (const auto *routing_section : cfg->get("routing")) {
    if (routing_section->has("bind_port") &&
        routing_section->has("destinations") &&
        routing_section->has("protocol")) {
      const auto port = routing_section->get("bind_port");
      const auto protocol = routing_section->get("protocol");
      const auto destinations = routing_section->get("destinations");

      const bool is_rw_split = routing_section->has("access_mode") &&
                               routing_section->get("access_mode") == "auto";
      const bool is_rw = !is_rw_split && destinations.ends_with("PRIMARY");
      const bool is_ro = !is_rw_split && destinations.ends_with("SECONDARY");

      if (protocol == "classic") {
        if (is_rw_split)
          result.rw_split_classic_port = port;
        else if (is_rw)
          result.rw_classic_port = port;
        else if (is_ro)
          result.ro_classic_port = port;
      } else if (protocol == "x") {
        if (is_rw)
          result.rw_x_port = port;
        else if (is_ro)
          result.ro_x_port = port;
      }
    }
  }

  return result;
}

static void init(mysql_harness::PluginFuncEnv *env) {
  g_app_info = get_app_info(env);
  // If a valid configuration object was found.
  if (g_app_info && g_app_info->config) {
    g_router_attributes = get_router_attributes(g_app_info->config);
    // if a valid metadata_cache section was found in the router
    // configuration.
    if (g_app_info->config->get(kSectionName).empty()) {
      set_error(env, mysql_harness::kConfigInvalidArgument,
                "[metadata_cache] section is empty");
    }
  }
}

class MetadataServersStateListener
    : public metadata_cache::ClusterStateListenerInterface {
 public:
  MetadataServersStateListener(ClusterMetadataDynamicState &dynamic_state)
      : dynamic_state_(dynamic_state) {}

  ~MetadataServersStateListener() override {
    metadata_cache::MetadataCacheAPI::instance()->remove_state_listener(this);
  }

  void notify_instances_changed(const bool md_servers_reachable,
                                const uint64_t view_id) override {
    if (!md_servers_reachable) return;

    const auto &cluster_topology =
        metadata_cache::MetadataCacheAPI::instance()->get_cluster_topology();
    if (cluster_topology.metadata_servers.empty()) {
      // This happens for example when the router could connect to one of the
      // metadata servers but failed to fetch metadata because the connection
      // went down while querying metadata
      log_warning(
          "Got empty list of metadata servers; refusing to store to the state "
          "file");
      return;
    }

    // need to convert from ManagedInstance to uri string
    std::vector<std::string> metadata_servers_str;
    for (const auto &md_server : cluster_topology.metadata_servers) {
      mysqlrouter::URI uri;
      uri.scheme = "mysql";
      uri.host = md_server.hostname();
      uri.port = md_server.port();
      metadata_servers_str.emplace_back(uri.str());
    }

    dynamic_state_.set_metadata_servers(metadata_servers_str);
    dynamic_state_.set_view_id(view_id);
    dynamic_state_.save();
  }

 private:
  ClusterMetadataDynamicState &dynamic_state_;
};

/**
 * Initialize the metadata cache for fetching the information from the
 * metadata servers.
 *
 * @param env plugin's environment
 */
static void start(mysql_harness::PluginFuncEnv *env) {
  my_thread_self_setname("MDC Main");

  mysqlrouter::MySQLClientThreadToken api_token;

  const mysql_harness::ConfigSection *section = get_config_section(env);
  std::unique_ptr<ClusterMetadataDynamicState> md_cache_dynamic_state;
  std::unique_ptr<MetadataServersStateListener> md_servers_state_listener;

  // launch metadata cache
  try {
    using namespace std::string_literals;

    const MetadataCachePluginConfig config(section);

    if (config.metadata_servers_addresses.size() == 0 &&
        (!config.metadata_cache_dynamic_state ||
         config.metadata_cache_dynamic_state->get_metadata_servers().empty())) {
      throw std::runtime_error(
          "list of metadata-servers is empty: "s +
          (!config.metadata_cache_dynamic_state
               ? "no known 'dynamic_config'-file."
               : "list of 'cluster-metadata-servers' in 'dynamic_config'-file "
                 "is empty.") +
          " Bootstrap the Router again to fix this issue.");
    }

    const metadata_cache::MetadataCacheTTLConfig ttl_config{
        config.ttl, config.auth_cache_ttl, config.auth_cache_refresh_interval};

    std::string password;
    try {
      password = mysql_harness::get_keyring()
                     ? mysql_harness::get_keyring()->fetch(
                           config.user, kKeyringAttributePassword)
                     : "";
    } catch (const std::out_of_range &) {
      std::string msg = "Could not find the password for user '" + config.user +
                        "' in the keyring. "
                        "metadata_cache not initialized properly.";
      throw std::runtime_error(msg);
    }

    log_info("Starting Metadata Cache");
    // Initialize the metadata cache.
    auto md_cache = metadata_cache::MetadataCacheAPI::instance();

    md_cache->instance_name(section->key);

    const std::string cluster_type_specific_id =
        config.get_cluster_type_specific_id();

    const std::string clusterset_id = config.get_clusterset_id();

    const metadata_cache::MetadataCacheMySQLSessionConfig session_config{
        {config.user, password},
        (int)config.connect_timeout,
        (int)config.read_timeout,
        1};

    // we currently support only single metadata-cache instance so there is no
    // need for locking here
    g_router_attributes.metadata_user_name = config.user;

    using TargetType = mysqlrouter::TargetCluster::TargetType;

    // very old state.json files will not have UUID, in that case we fallback to
    // using cluster name from static configuration
    const bool identify_cluster_by_name = cluster_type_specific_id.empty();
    mysqlrouter::TargetCluster target_cluster{
        identify_cluster_by_name ? TargetType::ByName : TargetType::ByUUID,
        identify_cluster_by_name ? config.cluster_name
                                 : cluster_type_specific_id};

    md_cache->cache_init(config.cluster_type, config.router_id, clusterset_id,
                         config.metadata_servers_addresses, ttl_config,
                         config.ssl_options, target_cluster, session_config,
                         g_router_attributes, config.thread_stack_size,
                         config.use_gr_notifications, config.get_view_id(),
                         config.close_connection_after_refresh);

    // register callback
    md_cache_dynamic_state = std::move(config.metadata_cache_dynamic_state);
    if (md_cache_dynamic_state) {
      md_servers_state_listener.reset(
          new MetadataServersStateListener(*md_cache_dynamic_state.get()));
      md_cache->add_state_listener(md_servers_state_listener.get());
    }

    // start metadata cache
    md_cache->cache_start();
  } catch (const std::runtime_error &exc) {  // metadata_cache::metadata_error
                                             // inherits from runtime_error
    set_error(env, mysql_harness::kRuntimeError, "%s", exc.what());
    clear_running(env);
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());
    clear_running(env);
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");
    clear_running(env);
  }

  // keep it running until Harness tells us to shut down
  wait_for_stop(env, 0);
  metadata_cache::MetadataCacheAPI::instance()->cache_stop();
}

static constexpr std::array required{
    "logger",
    "router_protobuf",
};

static void expose_configuration(mysql_harness::PluginFuncEnv *env,
                                 const char * /*key*/, bool initial) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  if (!info->config) return;

  for (const mysql_harness::ConfigSection *section : info->config->sections()) {
    if (section->name == kSectionName) {
      MetadataCachePluginConfig config{section};
      config.expose_configuration(info->config->get_default_section(), initial);
    }
  }
}

extern "C" {

mysql_harness::Plugin METADATA_CACHE_PLUGIN_EXPORT
    harness_plugin_metadata_cache = {
        mysql_harness::PLUGIN_ABI_VERSION,
        mysql_harness::ARCHITECTURE_DESCRIPTOR,
        "Metadata Cache, managing information fetched from the Metadata Server",
        VERSION_NUMBER(0, 0, 1),
        // requires
        required.size(),
        required.data(),
        // conflicts
        0,
        nullptr,
        init,     // init
        nullptr,  // deinit
        start,    // start
        nullptr,  // stop
        true,     // declares_readiness
        metadata_cache_supported_options.size(),
        metadata_cache_supported_options.data(),
        expose_configuration,
};
}
