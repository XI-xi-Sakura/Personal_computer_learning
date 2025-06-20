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

#ifndef METADATA_CACHE_CLUSTER_METADATA_INCLUDED
#define METADATA_CACHE_CLUSTER_METADATA_INCLUDED

#include "mysqlrouter/metadata_cache_datatypes.h"
#include "mysqlrouter/metadata_cache_export.h"

#include "mysqlrouter/cluster_metadata.h"
#include "mysqlrouter/metadata.h"
#include "mysqlrouter/metadata_cache.h"
#include "mysqlrouter/mysql_session.h"
#include "mysqlrouter/routing_guidelines_datatypes.h"
#include "mysqlrouter/routing_guidelines_version.h"
#include "router_options.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GroupReplicationMember;

namespace mysqlrouter {
class MySQLSession;
}  // namespace mysqlrouter

namespace xcl {
class XSession;
}  // namespace xcl

using ConnectCallback =
    std::function<bool(mysqlrouter::MySQLSession &connection,
                       const metadata_cache::ManagedInstance &mi)>;

/** @class ClusterMetadata
 *
 * The `ClusterMetadata` class encapsulates a connection to the Metadata server.
 * It uses the mysqlrouter::MySQLSession to setup, manage and retrieve results.
 *
 */
class METADATA_CACHE_EXPORT ClusterMetadata : public MetaData {
 public:
  /** @brief Constructor
   *
   * @param session_config Metadata MySQL session configuration
   * @param ssl_options SSL related options to use for MySQL connections)
   */
  ClusterMetadata(
      const metadata_cache::MetadataCacheMySQLSessionConfig &session_config,
      const mysqlrouter::SSLOptions &ssl_options);

  // disable copy as it isn't needed right now. Feel free to enable
  // must be explicitly defined though.
  explicit ClusterMetadata(const ClusterMetadata &) = delete;
  ClusterMetadata &operator=(const ClusterMetadata &) = delete;

  /** @brief Destructor
   *
   * Disconnect and release the connection to the metadata node.
   */
  ~ClusterMetadata() override;

  /** @brief Connects with the Metadata server and sets up the session
   * parameters
   *
   *
   * @param metadata_server the server instance for which the connection
   *                        should be attempted.
   *
   * @return a boolean to indicate if the connection and session parameters
   * setup was successful.
   */
  bool connect_and_setup_session(const metadata_cache::metadata_server_t
                                     &metadata_server) noexcept override;

  /** @brief Disconnects from the Metadata server
   *
   */
  void disconnect() noexcept override { metadata_connection_.reset(); }

  /**
   * check if the metadata_connection_ connection is connected to
   * metadata_server and alive.
   */
  [[nodiscard]] bool is_connected_to(
      const metadata_cache::metadata_server_t &metadata_server) const;

  /**
   * make a connection to metadata_server.
   *
   * if the metadata_connection_ already is connected to metadata_server, return
   * that instead.
   */
  stdx::expected<std::shared_ptr<mysqlrouter::MySQLSession>, std::string>
  make_connection_shared(
      const metadata_cache::metadata_server_t &metadata_server);

  /** @brief Gets the object representing the session to the metadata server
   */
  std::shared_ptr<mysqlrouter::MySQLSession> get_connection() override {
    return metadata_connection_;
  }

  bool update_router_attributes(
      const metadata_cache::metadata_server_t &rw_server,
      const unsigned router_id,
      const metadata_cache::RouterAttributes &router_attributes) override;

  bool update_router_last_check_in(
      const metadata_cache::metadata_server_t &rw_server,
      const unsigned router_id) override;

  void report_guideline_name(const std::string &guideline_name,
                             const metadata_cache::metadata_server_t &rw_server,
                             const unsigned router_id) override;

  auth_credentials_t fetch_auth_credentials(
      const metadata_cache::metadata_server_t &md_server,
      const mysqlrouter::TargetCluster &target_cluster) override;

  std::optional<routing_guidelines::Router_info> fetch_router_info(
      const uint16_t router_id) override;

  stdx::expected<std::string, std::error_code>
  fetch_routing_guidelines_document(const uint16_t router_id) override;

  std::optional<metadata_cache::metadata_server_t> find_rw_server(
      const std::vector<metadata_cache::ManagedInstance> &instances);

  std::optional<metadata_cache::metadata_server_t> find_rw_server(
      const std::vector<metadata_cache::ManagedCluster> &clusters);

  std::optional<std::chrono::seconds>
  get_periodic_stats_update_frequency() noexcept override {
    return router_options_.get_stats_updates_frequency();
  }

 protected:
  /** Connects a MYSQL connection to the given instance
   */
  bool do_connect(mysqlrouter::MySQLSession &connection,
                  const metadata_cache::metadata_server_t &mi);

  // throws metadata_cache::metadata_error and
  // MetadataUpgradeInProgressException
  mysqlrouter::MetadataSchemaVersion get_and_check_metadata_schema_version(
      mysqlrouter::MySQLSession &session);

  stdx::expected<std::string, std::error_code>
  get_select_routing_guidelines_query(
      const mysqlrouter::MetadataSchemaVersion &schema_version,
      const uint16_t router_id) {
    if (schema_version >= mysqlrouter::kRoutingGuidelinesMetadataVersion) {
      return R"(SELECT guideline FROM
mysql_innodb_cluster_metadata.routing_guidelines WHERE guideline_id = (
  SELECT COALESCE(RO.router_options->>'$.guideline',
                  CS.router_options->>'$.guideline',
                  CL.router_options->>'$.guideline')
  FROM
    mysql_innodb_cluster_metadata.v2_router_options AS RO
  LEFT JOIN
    mysql_innodb_cluster_metadata.clustersets AS CS ON RO.clusterset_id = CS.clusterset_id
  LEFT JOIN
    mysql_innodb_cluster_metadata.clusters AS CL ON RO.cluster_id = CL.cluster_id
  WHERE RO.router_id = )" +
             std::to_string(router_id) + ")";
    } else {
      return stdx::unexpected(make_error_code(
          routing_guidelines::routing_guidelines_errc::not_supported_in_md));
    }
  }

  // Metadata node generic information
  mysql_ssl_mode ssl_mode_;
  mysqlrouter::SSLOptions ssl_options_;

  metadata_cache::MetadataCacheMySQLSessionConfig session_config_;

#if 0  // not used so far
  // The number of times we should try connecting to the metadata server if a
  // connection attempt fails.
  int connection_attempts_;
#endif

  // connection to metadata server (it may also be shared with GR status queries
  // for optimisation purposes)
  std::shared_ptr<mysqlrouter::MySQLSession> metadata_connection_;

  RouterOptions router_options_;
};

std::string as_string(const char *input_str);

bool set_instance_ports(metadata_cache::ManagedInstance &instance,
                        const mysqlrouter::MySQLSession::Row &row,
                        const size_t classic_port_column,
                        const size_t x_port_column);

void set_instance_attributes(metadata_cache::ManagedInstance &instance,
                             const std::string &attributes);

#endif  // METADATA_CACHE_CLUSTER_METADATA_INCLUDED
