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

#include "metadata_cache.h"

#include <cassert>
#include <cmath>  // fabs()
#include <memory>
#include <stdexcept>
#include <vector>

#include "my_thread.h"  // my_thread_self_setname
#include "mysql/harness/destination.h"
#include "mysql/harness/event_state_tracker.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/plugin.h"
#include "mysqld_error.h"
#include "mysqlrouter/cluster_metadata.h"
#include "mysqlrouter/mysql_client_thread_token.h"
#include "mysqlrouter/mysql_session.h"

using namespace std::chrono_literals;
using namespace std::string_literals;
using mysql_harness::EventStateTracker;
using mysql_harness::logging::LogLevel;

IMPORT_LOG_FUNCTIONS()

MetadataCache::MetadataCache(
    const unsigned router_id, const std::string &clusterset_id,
    const std::vector<mysql_harness::TcpDestination> &metadata_servers,
    std::shared_ptr<MetaData> cluster_metadata,
    const metadata_cache::MetadataCacheTTLConfig &ttl_config,
    const mysqlrouter::SSLOptions &ssl_options,
    const mysqlrouter::TargetCluster &target_cluster,
    const metadata_cache::RouterAttributes &router_attributes,
    size_t thread_stack_size, bool use_cluster_notifications,
    bool close_connection_after_refresh)
    : target_cluster_(target_cluster),
      clusterset_id_(clusterset_id),
      ttl_config_(ttl_config),
      ssl_options_(ssl_options),
      router_id_(router_id),
      meta_data_(std::move(cluster_metadata)),
      refresh_thread_(thread_stack_size),
      use_cluster_notifications_(use_cluster_notifications),
      close_connection_after_refresh_(close_connection_after_refresh),
      router_attributes_(router_attributes) {
  for (const auto &srv : metadata_servers) {
    metadata_servers_.emplace_back(srv);
  }
}

MetadataCache::~MetadataCache() {
  meta_data_->shutdown_notifications_listener();
}

void *MetadataCache::run_thread(void *context) {
  mysqlrouter::MySQLClientThreadToken api_token;
  MetadataCache *metadata_cache = static_cast<MetadataCache *>(context);

  metadata_cache->refresh_thread();

  return nullptr;
}

void MetadataCache::refresh_thread() {
  my_thread_self_setname("MDC Refresh");
  log_info("Starting metadata cache refresh thread");

  // this will be only useful if the TTL is set to some value that is more than
  // 1 second
  const std::chrono::milliseconds kTerminateOrForcedRefreshCheckInterval =
      std::chrono::seconds(1);

  auto auth_cache_ttl_left = ttl_config_.auth_cache_refresh_interval;
  bool auth_cache_force_update = true;
  while (!terminated_) {
    bool refresh_ok{false};
    const bool attributes_upd = needs_initial_attributes_update();
    const bool last_check_in_upd = needs_last_check_in_update();
    const bool needs_rw_node = attributes_upd || last_check_in_upd;
    try {
      // Component tests are using this log message as a indicator of metadata
      // refresh start
      log_debug("Started refreshing the cluster metadata");
      refresh_ok = refresh(needs_rw_node);
      // Component tests are using this log message as a indicator of metadata
      // refresh finish
      log_debug("Finished refreshing the cluster metadata");
      on_refresh_completed();
    } catch (const mysqlrouter::MetadataUpgradeInProgressException &) {
      log_info(
          "Cluster metadata upgrade in progress, aborting the metada refresh");
    } catch (const std::exception &e) {
      log_info("Failed refreshing metadata: %s", e.what());
      on_refresh_failed(true);
    }

    if (!refresh_ok || close_connection_after_refresh_ ||
        meta_data_->get_cluster_type() != mysqlrouter::ClusterType::GR_V2) {
      meta_data_->disconnect();
    }

    if (refresh_ok) {
      if (!ready_announced_) {
        ready_announced_ = true;
        const auto &name =
            metadata_cache::MetadataCacheAPI::instance()->instance_name();
        mysql_harness::on_service_ready(
            name.empty() ? "metadata_cache" : "metadata_cache:" + name);
      }
      // update router attributes in the routers table once when we start
      if (attributes_upd) {
        update_router_attributes();
      }

      if (auth_cache_force_update) {
        update_auth_cache();
        auth_cache_force_update = false;
      }

      // update the router.last_check_in
      if (last_check_in_upd) {
        update_router_last_check_in();
      }
    }

    auto ttl_left = ttl_config_.ttl;
    while (ttl_left > 0ms) {
      auto sleep_for =
          std::min(ttl_left, kTerminateOrForcedRefreshCheckInterval);

      {
        std::unique_lock<std::mutex> lock(refresh_wait_mtx_);
        // first check if we were not told to leave or refresh again while we
        // were outside of the wait_for
        if (terminated_) return;
        if (refresh_requested_) {
          auth_cache_force_update = true;
          refresh_requested_ = false;
          break;  // go to the refresh() in the outer loop
        }

        if (sleep_for >= auth_cache_ttl_left) {
          refresh_wait_.wait_for(lock, auth_cache_ttl_left);
          ttl_left -= auth_cache_ttl_left;
          auto start_timestamp = std::chrono::steady_clock::now();
          if (refresh_ok && update_auth_cache())
            auth_cache_ttl_left = ttl_config_.auth_cache_refresh_interval;
          auto end_timestamp = std::chrono::steady_clock::now();
          auto time_spent =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  end_timestamp - start_timestamp);
          ttl_left -= time_spent;
        } else {
          refresh_wait_.wait_for(lock, sleep_for);
          auth_cache_ttl_left -= sleep_for;
          ttl_left -= sleep_for;
        }
        if (terminated_) return;
        if (refresh_requested_) {
          auth_cache_force_update = true;
          refresh_requested_ = false;
          break;  // go to the refresh() in the outer loop
        }
      }

      {
        std::lock_guard<std::mutex> lock(cache_refreshing_mutex_);
        // if the metadata is not consistent refresh it at a higher rate (if the
        // ttl>1s) until it becomes consistent again
        const bool md_discrepancy =
            std::find_if(cluster_topology_.clusters_data.begin(),
                         cluster_topology_.clusters_data.end(),
                         [](const auto &c) { return c.md_discrepancy; }) !=
            cluster_topology_.clusters_data.end();

        if (md_discrepancy) {
          break;
        }
      }
    }
  }
}

/**
 * Connect to the metadata servers and refresh the metadata information in the
 * cache.
 */
void MetadataCache::start() {
  // start refresh thread that uses classic protocol
  refresh_thread_.run(&run_thread, this);
}

/**
 * Stop the refresh thread.
 */
void MetadataCache::stop() noexcept {
  {
    std::unique_lock<std::mutex> lk(refresh_wait_mtx_, std::defer_lock);
    std::unique_lock<std::mutex> lk2(refresh_completed_mtx_, std::defer_lock);
    std::lock(lk, lk2);
    terminated_ = true;
  }
  refresh_wait_.notify_one();
  refresh_completed_.notify_one();
  refresh_thread_.join();
}

/**
 * Return a list of servers that are part of a cluster.
 */
metadata_cache::cluster_nodes_list_t MetadataCache::get_cluster_nodes() {
  std::lock_guard<std::mutex> lock(cache_refreshing_mutex_);
  return cluster_topology_.get_all_members();
}

metadata_cache::ClusterTopology MetadataCache::get_cluster_topology() {
  std::lock_guard<std::mutex> lock(cache_refreshing_mutex_);
  return cluster_topology_;
}

bool metadata_cache::ManagedInstance::operator==(
    const ManagedInstance &other) const {
  return mysql_server_uuid == other.mysql_server_uuid && mode == other.mode &&
         role == other.role && host == other.host && port == other.port &&
         xport == other.xport && hidden == other.hidden &&
         disconnect_existing_sessions_when_hidden ==
             other.disconnect_existing_sessions_when_hidden &&
         ignore == other.ignore && tags == other.tags &&
         version == other.version && label == other.label;
}

metadata_cache::ManagedInstance::ManagedInstance(
    mysqlrouter::InstanceType p_type, const std::string &p_mysql_server_uuid,
    const ServerMode p_mode, const ServerRole p_role, const std::string &p_host,
    const uint16_t p_port, const uint16_t p_xport, std::string label)
    : type(p_type),
      mysql_server_uuid(p_mysql_server_uuid),
      mode(p_mode),
      role(p_role),
      host(p_host),
      port(p_port),
      xport(p_xport),
      hidden(mysqlrouter::kNodeTagHiddenDefault),
      disconnect_existing_sessions_when_hidden(
          mysqlrouter::kNodeTagDisconnectWhenHiddenDefault),
      label(std::move(label)) {}

metadata_cache::ManagedInstance::ManagedInstance(
    mysqlrouter::InstanceType p_type)
    : hidden(mysqlrouter::kNodeTagHiddenDefault),
      disconnect_existing_sessions_when_hidden(
          mysqlrouter::kNodeTagDisconnectWhenHiddenDefault) {
  type = p_type;
}

metadata_cache::ManagedInstance::ManagedInstance(
    mysqlrouter::InstanceType p_type, const mysql_harness::TcpDestination &dest)
    : ManagedInstance(p_type) {
  host = dest.hostname();
  port = dest.port();
}

metadata_cache::ManagedInstance::operator mysql_harness::TcpDestination()
    const {
  return mysql_harness::TcpDestination(host, port);
}

namespace metadata_cache {

bool operator==(const metadata_cache::ManagedCluster &cluster_a,
                const metadata_cache::ManagedCluster &cluster_b) {
  if (cluster_a.md_discrepancy != cluster_b.md_discrepancy ||
      cluster_a.id != cluster_b.id || cluster_a.name != cluster_b.name ||
      cluster_a.is_invalidated != cluster_b.is_invalidated ||
      cluster_a.is_primary != cluster_b.is_primary)
    return false;
  // we need to compare 2 vectors if their content is the same
  // but order of their elements can be different as we use
  // SQL with no "ORDER BY" to fetch them from different nodes
  if (cluster_a.members.size() != cluster_b.members.size()) return false;
  if (!std::is_permutation(cluster_a.members.begin(), cluster_a.members.end(),
                           cluster_b.members.begin())) {
    return false;
  }

  return true;
}

bool operator!=(const metadata_cache::ManagedCluster &cluster_a,
                const metadata_cache::ManagedCluster &cluster_b) {
  return !(cluster_a == cluster_b);
}

bool operator==(const metadata_cache::ClusterTopology &a,
                const metadata_cache::ClusterTopology &b) {
  if (!std::is_permutation(a.clusters_data.begin(), a.clusters_data.end(),
                           b.clusters_data.begin(), b.clusters_data.end())) {
    return false;
  }

  if (!std::is_permutation(a.metadata_servers.begin(), a.metadata_servers.end(),
                           b.metadata_servers.begin(),
                           b.metadata_servers.end())) {
    return false;
  }

  return a.target_cluster_pos == b.target_cluster_pos && a.view_id == b.view_id;
}

bool operator!=(const metadata_cache::ClusterTopology &a,
                const metadata_cache::ClusterTopology &b) {
  return !(a == b);
}

}  // namespace metadata_cache

std::string to_string(metadata_cache::ServerMode mode) {
  switch (mode) {
    case metadata_cache::ServerMode::ReadWrite:
      return "RW";
    case metadata_cache::ServerMode::ReadOnly:
      return "RO";
    case metadata_cache::ServerMode::Unavailable:
      return "n/a";
    default:
      return "?";
  }
}

std::string get_hidden_info(const metadata_cache::ManagedInstance &instance) {
  std::string result;
  // if both values are default return empty string
  if (instance.hidden || !instance.disconnect_existing_sessions_when_hidden) {
    result =
        " hidden=" + (instance.hidden ? "yes"s : "no"s) +
        " disconnect_when_hidden=" +
        (instance.disconnect_existing_sessions_when_hidden ? "yes"s : "no"s);
  }

  return result;
}

void MetadataCache::on_refresh_failed(bool terminated,
                                      bool md_servers_reachable) {
  stats_([](auto &stats) {
    stats.refresh_failed++;
    stats.last_refresh_failed = std::chrono::system_clock::now();
  });

  const bool refresh_state_changed =
      EventStateTracker::instance().state_changed(
          false, EventStateTracker::EventId::MetadataRefreshOk);

  // we failed to fetch metadata from any of the metadata servers
  if (!terminated) {
    const auto log_level =
        refresh_state_changed ? LogLevel::kError : LogLevel::kDebug;
    log_custom(log_level,
               "Failed fetching metadata from any of the %u metadata servers.",
               static_cast<unsigned>(metadata_servers_.size()));
  }

  // clearing metadata
  {
    bool clearing;
    {
      std::lock_guard<std::mutex> lock(cache_refreshing_mutex_);
      clearing = !cluster_topology_.get_all_members().empty();
      if (clearing) cluster_topology_.clear_all_members();
    }
    if (clearing) {
      on_instances_changed(md_servers_reachable, {});
      const auto log_level =
          refresh_state_changed ? LogLevel::kInfo : LogLevel::kDebug;
      log_custom(log_level,
                 "... cleared current routing table as a precaution");
    }
  }
}

void MetadataCache::on_refresh_succeeded(
    const metadata_cache::metadata_server_t &metadata_server) {
  EventStateTracker::instance().state_changed(
      true, EventStateTracker::EventId::MetadataRefreshOk);
  stats_([&metadata_server](auto &stats) {
    stats.last_refresh_succeeded = std::chrono::system_clock::now();
    stats.last_metadata_server_host = metadata_server.hostname();
    stats.last_metadata_server_port = metadata_server.port();
    stats.refresh_succeeded++;
  });
}

static void trace_instances_info(
    const metadata_cache::ClusterTopology &cluster_topology) {
  const auto &clusterset_name = cluster_topology.name;
  for (const auto &cluster : cluster_topology.clusters_data) {
    const std::string cluster_role = cluster.is_primary ? "PRIMARY" : "REPLICA";
    for (const auto &cluster_member : cluster.members) {
      if (cluster_member.mode == metadata_cache::ServerMode::Unavailable) {
        continue;
      }

      std::string tags_str;
      for (const auto &tag : cluster_member.tags) {
        tags_str += "\n\t\t" + tag.first + ": " + tag.second;
      }

      const std::string member_role =
          cluster_member.mode == metadata_cache::ServerMode::ReadWrite
              ? "PRIMARY"
              : "SECONDARY";

      const auto label_port =
          cluster_member.port == 0 ? cluster_member.xport : cluster_member.port;
      const std::string label =
          cluster_member.host + ":" + std::to_string(label_port);

      log_debug(
          "Instance reported from metadata:\n\tAddress: %s,\n\tPort: "
          "%hu\n\tX "
          "port: %hu\n\tUUID: %s\n\tMember role: %s\n\tClusterSet name: "
          "%s\n\tCluster name: %s\n\tCluster role: %s\n\tLabel: %s\n\tTags: "
          "%s",
          cluster_member.host.c_str(), cluster_member.port,
          cluster_member.xport, cluster_member.mysql_server_uuid.c_str(),
          member_role.c_str(), clusterset_name.c_str(), cluster.name.c_str(),
          cluster_role.c_str(), label.c_str(), tags_str.c_str());
    }
  }
}

void MetadataCache::on_instances_changed(const bool md_servers_reachable,
                                         uint64_t view_id) {
  // Socket acceptors state will be updated when processing new instances
  // information.
  trigger_acceptor_update_on_next_refresh_ = false;

  {
    std::lock_guard<std::mutex> lock(cluster_instances_change_callbacks_mtx_);

    for (auto *each : state_listeners_) {
      each->notify_instances_changed(md_servers_reachable, view_id);
    }
  }

  if (log_level_is_handled(mysql_harness::logging::LogLevel::kDebug)) {
    trace_instances_info(get_cluster_topology());
  }

  if (use_cluster_notifications_) {
    meta_data_->setup_notifications_listener(
        get_cluster_topology(), [this]() { on_refresh_requested(); });
  }
}

void MetadataCache::on_handle_sockets_acceptors() {
  std::lock_guard lock(acceptor_handler_callbacks_mtx_);

  trigger_acceptor_update_on_next_refresh_ = false;
  for (const auto &callback : acceptor_update_listeners_) {
    // If setting up any acceptor failed we should retry on next md refresh
    if (!callback->update_socket_acceptor_state()) {
      trigger_acceptor_update_on_next_refresh_ = true;
    }
  }
}

void MetadataCache::update_routing_guidelines(
    const std::string &routing_guidelines_doc) {
  // We want to copy the callback to avoid holding a lock when we are
  // calculating what needs to be done according to the new guidelines
  std::vector<
      metadata_cache::MetadataCacheAPI::on_routing_guidelines_change_callback_t>
      routing_guidelines_update_callbacks;
  {
    std::lock_guard routing_guidelines_lock(
        routing_guidelines_update_callback_mtx_);
    if (update_routing_guidelines_callback_) {
      for (const auto &on_update_clb :
           on_routing_guidelines_change_callbacks_) {
        routing_guidelines_update_callbacks.push_back(on_update_clb);
      }
    }
  }

  try {
    if (!routing_guidelines_update_callbacks.empty()) {
      const auto &update_details =
          update_routing_guidelines_callback_(routing_guidelines_doc);
      for (const auto &clb : routing_guidelines_update_callbacks) {
        clb(update_details);
      }
      update_reported_routing_guideline_name(update_details.guideline_name);
      log_debug("Routing guidelines document updated");
    }
  } catch (const std::exception &e) {
    log_error("Update routing guidelines failed: %s", e.what());
  }
}

void MetadataCache::on_md_refresh(const bool cluster_nodes_changed,
                                  const std::string &routing_guidelines_doc) {
  {
    std::lock_guard lock(md_refresh_callbacks_mtx_);
    for (auto &each : md_refresh_listeners_) {
      each->on_md_refresh(cluster_nodes_changed);
    }
  }

  const auto &router_info = meta_data_->fetch_router_info(router_id_);
  if (router_info) {
    std::lock_guard lock(router_info_update_callback_mtx_);
    for (const auto &update_router_info_clb : update_router_info_callbacks_) {
      // Do it for each routing plugin
      update_router_info_clb(*router_info);
    }
  }

  if (last_routing_guidelines_used_ != routing_guidelines_doc) {
    update_routing_guidelines(routing_guidelines_doc);
    last_routing_guidelines_used_ = routing_guidelines_doc;
  }
}

void MetadataCache::on_refresh_requested() {
  {
    std::unique_lock<std::mutex> lock(refresh_wait_mtx_);
    refresh_requested_ = true;
  }
  refresh_wait_.notify_one();
}

void MetadataCache::on_refresh_completed() { refresh_completed_.notify_one(); }

/**
 * check if primary has changed.
 *
 * - hidden members are ignored
 *
 * @param members container current membership info
 * @param primary_server_uuid server-uuid of the previous PRIMARY
 */
static bool primary_has_changed(
    const std::vector<metadata_cache::ManagedInstance> &members,
    const std::string &primary_server_uuid) {
  // if we have a member, that's PRIMARY and not "server_uuid" -> success
  for (auto const &member : members) {
    if (member.hidden) continue;

    if (member.mode != metadata_cache::ServerMode::ReadWrite) continue;

    if (member.mysql_server_uuid != primary_server_uuid) {
      return true;
    }
  }

  return false;
}

bool MetadataCache::wait_primary_failover(const std::string &server_uuid,
                                          const std::chrono::seconds &timeout) {
  log_debug("Waiting for failover to happen in '%s' for %lds",
            target_cluster_.c_str(), static_cast<long>(timeout.count()));

  using clock_type = std::chrono::steady_clock;
  const auto end_time = clock_type::now() + timeout;
  do {
    if (terminated_) return false;

    // if we have a member, that's PRIMARY and not "server_uuid" -> success
    if (primary_has_changed(get_cluster_nodes(), server_uuid)) {
      return true;
    }

    // wait until a refresh finished.
    std::unique_lock<std::mutex> lock(refresh_completed_mtx_);
    const auto wait_res = refresh_completed_.wait_until(lock, end_time);
    if (wait_res == std::cv_status::timeout) {
      // timed out waiting for refresh to finish.
      //
      // Either the wait-time was smaller than the metadata-cache-ttl or the
      // metadata-cache refresh took longer than expected.
      break;
    }
  } while (clock_type::now() < end_time);

  // if we have a member, that's PRIMARY and not "server_uuid" -> success
  return primary_has_changed(get_cluster_nodes(), server_uuid);
}

void MetadataCache::add_state_listener(
    metadata_cache::ClusterStateListenerInterface *listener) {
  std::lock_guard<std::mutex> lock(cluster_instances_change_callbacks_mtx_);
  state_listeners_.insert(listener);
}

void MetadataCache::remove_state_listener(
    metadata_cache::ClusterStateListenerInterface *listener) {
  std::lock_guard<std::mutex> lock(cluster_instances_change_callbacks_mtx_);
  state_listeners_.erase(listener);
}

void MetadataCache::add_acceptor_handler_listener(
    metadata_cache::AcceptorUpdateHandlerInterface *listener) {
  std::lock_guard<std::mutex> lock(acceptor_handler_callbacks_mtx_);
  acceptor_update_listeners_.insert(listener);
}

void MetadataCache::remove_acceptor_handler_listener(
    metadata_cache::AcceptorUpdateHandlerInterface *listener) {
  std::lock_guard<std::mutex> lock(acceptor_handler_callbacks_mtx_);
  acceptor_update_listeners_.erase(listener);
}

void MetadataCache::add_md_refresh_listener(
    metadata_cache::MetadataRefreshListenerInterface *listener) {
  std::lock_guard<std::mutex> lock(md_refresh_callbacks_mtx_);
  md_refresh_listeners_.insert(listener);
}

void MetadataCache::remove_md_refresh_listener(
    metadata_cache::MetadataRefreshListenerInterface *listener) {
  std::lock_guard<std::mutex> lock(md_refresh_callbacks_mtx_);
  md_refresh_listeners_.erase(listener);
}

void MetadataCache::check_auth_metadata_timers() const {
  if (ttl_config_.auth_cache_ttl > 0ms &&
      ttl_config_.auth_cache_ttl < ttl_config_.ttl) {
    throw std::invalid_argument(
        "'auth_cache_ttl' option value '" +
        std::to_string(static_cast<float>(ttl_config_.auth_cache_ttl.count()) /
                       1000) +
        "' cannot be less than the 'ttl' value which is '" +
        std::to_string(static_cast<float>(ttl_config_.ttl.count()) / 1000) +
        "'");
  }
  if (ttl_config_.auth_cache_refresh_interval < ttl_config_.ttl) {
    throw std::invalid_argument(
        "'auth_cache_refresh_interval' option value '" +
        std::to_string(static_cast<float>(
                           ttl_config_.auth_cache_refresh_interval.count()) /
                       1000) +
        "' cannot be less than the 'ttl' value which is '" +
        std::to_string(static_cast<float>(ttl_config_.ttl.count()) / 1000) +
        "'");
  }
  if (ttl_config_.auth_cache_ttl > 0ms &&
      ttl_config_.auth_cache_refresh_interval > ttl_config_.auth_cache_ttl) {
    throw std::invalid_argument(
        "'auth_cache_ttl' option value '" +
        std::to_string(static_cast<float>(ttl_config_.auth_cache_ttl.count()) /
                       1000) +
        "' cannot be less than the 'auth_cache_refresh_interval' value which "
        "is '" +
        std::to_string(static_cast<float>(
                           ttl_config_.auth_cache_refresh_interval.count()) /
                       1000) +
        "'");
  }
}

std::pair<bool, MetaData::auth_credentials_t::mapped_type>
MetadataCache::get_rest_user_auth_data(const std::string &user) {
  auto auth_cache_ttl = ttl_config_.auth_cache_ttl;

  return rest_auth_([&user, auth_cache_ttl](auto &rest_auth)
                        -> std::pair<
                            bool, MetaData::auth_credentials_t::mapped_type> {
    // negative TTL is treated as infinite
    if (auth_cache_ttl.count() >= 0 &&
        rest_auth.last_credentials_update_ + auth_cache_ttl <
            std::chrono::system_clock::now()) {
      // auth cache expired
      return {false, std::make_pair("", rapidjson::Document{})};
    }

    auto pos = rest_auth.rest_auth_data_.find(user);
    if (pos == std::end(rest_auth.rest_auth_data_))
      return {false, std::make_pair("", rapidjson::Document{})};

    auto &auth_data = pos->second;
    rapidjson::Document temp_privileges;
    temp_privileges.CopyFrom(auth_data.second, auth_data.second.GetAllocator());
    return {true, std::make_pair(auth_data.first, std::move(temp_privileges))};
  });
}

bool MetadataCache::update_auth_cache() {
  if (meta_data_ && auth_metadata_fetch_enabled_ &&
      !cluster_topology_.metadata_servers.empty()) {
    try {
      const metadata_cache::metadata_server_t md_server =
          cluster_topology_.metadata_servers[0];

      rest_auth_([this, md_server](auto &rest_auth) {
        rest_auth.rest_auth_data_ =
            meta_data_->fetch_auth_credentials(md_server, target_cluster_);
        rest_auth.last_credentials_update_ = std::chrono::system_clock::now();
      });
      return true;
    } catch (const std::exception &e) {
      log_warning("Updating the authentication credentials failed: %s",
                  e.what());
    }
  }
  return false;
}

void MetadataCache::update_router_attributes() {
  if (cluster_topology_.writable_server) {
    const auto &rw_server = cluster_topology_.writable_server.value();

    try {
      meta_data_->update_router_attributes(rw_server, router_id_,
                                           router_attributes_);
      log_debug(
          "Successfully updated the Router attributes in the metadata using "
          "instance %s",
          rw_server.str().c_str());
      initial_attributes_update_done_ = true;
    } catch (const mysqlrouter::MetadataUpgradeInProgressException &) {
    } catch (const mysqlrouter::MySQLSession::Error &e) {
      if (e.code() == ER_TABLEACCESS_DENIED_ERROR) {
        // if the update fails because of the lack of the access rights that
        // most likely means that the Router has been upgraded, we need to
        // keep retrying it until the metadata gets upgraded too and our db
        // user gets missing access rights

        // we log it only once
        const bool first_time = EventStateTracker::instance().state_changed(
            true, EventStateTracker::EventId::NoRightsToUpdateRouterAttributes);
        if (first_time) {
          log_warning(
              "Updating the router attributes in metadata failed: %s (%u)\n"
              "Make sure to follow the correct steps to upgrade your "
              "metadata.\n"
              "Run the dba.upgradeMetadata() then launch the new Router "
              "version when prompted",
              e.message().c_str(), e.code());
        }
      } else {
        log_warning("Updating the router attributes in metadata failed: %s",
                    e.what());
        initial_attributes_update_done_ = true;
      }
    } catch (const std::exception &e) {
      log_warning("Updating the router attributes in metadata failed: %s",
                  e.what());
      initial_attributes_update_done_ = true;
    }
  } else {
    log_debug(
        "Did not find writable instance to update the Router attributes in "
        "the metadata.");
  }
}

void MetadataCache::update_router_last_check_in() {
  if (cluster_topology_.writable_server) {
    const auto &rw_server = cluster_topology_.writable_server.value();
    try {
      meta_data_->update_router_last_check_in(rw_server, router_id_);
    } catch (const mysqlrouter::MetadataUpgradeInProgressException &) {
    } catch (...) {
      // failing to update the last_check_in should not be treated as an error,
      // let's try next time
    }
  }

  last_periodic_stats_update_timestamp_ = std::chrono::steady_clock::now();
  periodic_stats_update_counter_ = 1;
}

void MetadataCache::update_reported_routing_guideline_name(
    const std::string &guideline_name) {
  if (cluster_topology_.writable_server) {
    const auto &rw_server = cluster_topology_.writable_server.value();
    meta_data_->report_guideline_name(guideline_name, rw_server, router_id_);
  }
}

bool MetadataCache::needs_initial_attributes_update() {
  return !initial_attributes_update_done_;
}

bool MetadataCache::needs_last_check_in_update() {
  const auto frequency = meta_data_->get_periodic_stats_update_frequency();
  if (!frequency) {
    return (periodic_stats_update_counter_++) % 10 == 0;
  } else {
    if (*frequency == 0s) return false;  // frequency == 0 means never update

    const auto now = std::chrono::steady_clock::now();
    return now > last_periodic_stats_update_timestamp_ +
                     std::chrono::seconds(*frequency);
  }
}

void MetadataCache::add_routing_guidelines_update_callbacks(
    metadata_cache::MetadataCacheAPI::update_routing_guidelines_callback_t
        update_callback,
    metadata_cache::MetadataCacheAPI::on_routing_guidelines_change_callback_t
        on_routing_guidelines_change_callback) {
  std::lock_guard lock{routing_guidelines_update_callback_mtx_};
  if (!update_routing_guidelines_callback_)
    update_routing_guidelines_callback_ = std::move(update_callback);
  on_routing_guidelines_change_callbacks_.push_back(
      std::move(on_routing_guidelines_change_callback));
}

void MetadataCache::clear_routing_guidelines_update_callbacks() {
  std::lock_guard lock{routing_guidelines_update_callback_mtx_};
  update_routing_guidelines_callback_ = nullptr;
  on_routing_guidelines_change_callbacks_.clear();
}

void MetadataCache::add_router_info_update_callback(
    metadata_cache::MetadataCacheAPI::update_router_info_callback_t clb) {
  std::lock_guard lock{router_info_update_callback_mtx_};
  update_router_info_callbacks_.push_back(clb);
}

void MetadataCache::clear_router_info_update_callback() {
  std::lock_guard lock{router_info_update_callback_mtx_};
  update_router_info_callbacks_.clear();
}
