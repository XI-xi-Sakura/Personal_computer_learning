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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef MYSQLROUTER_ROUTING_COMPONENT_INCLUDED
#define MYSQLROUTER_ROUTING_COMPONENT_INCLUDED

#include "mysqlrouter/routing_export.h"  // ROUTING_EXPORT

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "connection.h"
#include "mysql/harness/config_parser.h"
#include "routing_guidelines/routing_guidelines.h"

class MySQLRoutingBase;
class BaseProtocol;

class ROUTING_EXPORT MySQLRoutingAPI {
 public:
  MySQLRoutingAPI() = default;

  MySQLRoutingAPI(std::shared_ptr<MySQLRoutingBase> r) : r_{std::move(r)} {}

  // config
  std::string get_bind_address() const;
  uint16_t get_bind_port() const;

  std::chrono::milliseconds get_client_connect_timeout() const;
  std::chrono::milliseconds get_destination_connect_timeout() const;
  std::string get_destination_cluster_name() const;
  std::string get_destination_replicaset_name() const;

  DestinationNodesStateNotifier *get_destinations_state_notifier() const;
  int get_max_connections() const;
  uint64_t get_max_connect_errors() const;

  std::string get_name() const;

  std::string get_protocol_name() const;
  std::string get_routing_strategy() const;
  std::string get_socket() const;

  explicit operator bool() const noexcept { return r_.operator bool(); }

  std::vector<std::string> get_blocked_client_hosts() const;

  struct ConnData {
    using time_point_type = std::chrono::time_point<std::chrono::system_clock>;

    ConnData() = default;

    ConnData(std::string src, std::string dst, std::size_t bytes_up,
             std::size_t bytes_down, time_point_type started,
             time_point_type connected_to_server,
             time_point_type last_sent_to_server,
             time_point_type last_received_from_server)
        : src(std::move(src)),
          dst(std::move(dst)),
          bytes_up(bytes_up),
          bytes_down(bytes_down),
          started(started),
          connected_to_server(connected_to_server),
          last_sent_to_server(last_sent_to_server),
          last_received_from_server(last_received_from_server) {}

    std::string src;
    std::string dst;

    std::size_t bytes_up;
    std::size_t bytes_down;

    time_point_type started;
    time_point_type connected_to_server;
    time_point_type last_sent_to_server;
    time_point_type last_received_from_server;
  };

  struct SslOptions {
    mysql_ssl_mode ssl_mode;
    std::string tls_version;
    std::string ssl_cipher;
    std::string ca;
    std::string capath;
    std::string crl;
    std::string crlpath;
    std::string cert;
    std::string key;
    std::string curves;
  };

  std::vector<ConnData> get_connections() const;

  // status
  int get_active_connections() const;
  int get_total_connections() const;

  std::vector<mysql_harness::Destination> get_destination_candidates() const;
  SslOptions get_destination_ssl_options() const;

  void start_accepting_connections();
  void restart_accepting_connections();

  bool is_accepting_connections() const;

  void stop_socket_acceptors();

  bool is_running() const;

 private:
  std::shared_ptr<MySQLRoutingBase> r_;
};

class ROUTING_EXPORT MySQLRoutingComponent {
 public:
  static MySQLRoutingComponent &get_instance();

  void deinit();

  void init(const mysql_harness::Config &config);

  void register_route(const std::string &name,
                      std::shared_ptr<MySQLRoutingBase> srv);

  void erase(const std::string &name);

  MySQLRoutingAPI api(const std::string &name);

  uint64_t current_total_connections();
  uint64_t max_total_connections() const { return max_total_connections_; }

  const rapidjson::Document &routing_guidelines_document() const;

  rapidjson::Document routing_guidelines_document_schema() const;

  std::vector<std::string> route_names() const;

  void set_routing_guidelines(const std::string &routing_guidelines_document);

  bool routing_guidelines_initialized() const;

  std::shared_ptr<routing_guidelines::Routing_guidelines_engine>
  get_routing_guidelines() {
    std::lock_guard<std::mutex> lock{routing_guidelines_mtx_};
    return routing_guidelines_;
  }

  MySQLRoutingConnectionBase *get_connection(
      const std::string &client_endpoint);

 private:
  // disable copy, as we are a single-instance
  MySQLRoutingComponent(MySQLRoutingComponent const &) = delete;
  void operator=(MySQLRoutingComponent const &) = delete;

  std::mutex routes_mu_;
  std::map<std::string, std::weak_ptr<MySQLRoutingBase>> routes_;

  uint64_t max_total_connections_{0};

  MySQLRoutingComponent() = default;

  mutable std::mutex routing_guidelines_mtx_;
  std::shared_ptr<routing_guidelines::Routing_guidelines_engine>
      routing_guidelines_{nullptr};
};

#endif
