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

#ifndef ROUTING_CONTEXT_INCLUDED
#define ROUTING_CONTEXT_INCLUDED

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "blocked_endpoints.h"
#include "destination_ssl_context.h"
#include "mysql/harness/filesystem.h"  // Path
#include "mysql/harness/tls_server_context.h"
#include "mysql_router_thread.h"
#include "mysqlrouter/base_protocol.h"
#include "mysqlrouter/datatypes.h"
#include "mysqlrouter/destination.h"
#include "mysqlrouter/routing.h"
#include "mysqlrouter/ssl_mode.h"
#include "routing_config.h"
#include "shared_quarantine_handler.h"

/**
 * @brief MySQLRoutingContext holds data used by MySQLRouting (1 per plugin
 * instances) and MySQLRoutingConnection instances (many instances). It is
 * created and owned by MySQLRouting while MySQLRoutingConnection objects hold
 * reference to it.
 */
class MySQLRoutingContext {
 public:
  MySQLRoutingContext(
      const RoutingConfig &routing_config, std::string name,
      TlsServerContext *client_ssl_ctx, DestinationTlsContext *dest_tls_context,
      std::shared_ptr<routing_guidelines::Routing_guidelines_engine>
          routing_guidelines)
      : routing_config_(routing_config),
        name_(std::move(name)),
        client_ssl_ctx_{client_ssl_ctx},
        destination_tls_context_{dest_tls_context},
        routing_guidelines_{std::move(routing_guidelines)},
        blocked_endpoints_{routing_config.max_connect_errors} {
    router_info_.route_name = get_id();
    connection_sharing_ = routing_config_.connection_sharing;
  }

  BlockedEndpoints &blocked_endpoints() { return blocked_endpoints_; }
  const BlockedEndpoints &blocked_endpoints() const {
    return blocked_endpoints_;
  }

  void increase_info_active_routes();
  void decrease_info_active_routes();
  void increase_info_handled_routes();

  uint16_t get_active_routes() { return info_active_routes_; }
  uint64_t get_handled_routes() { return info_handled_routes_; }
  uint64_t get_max_connect_errors() const {
    return blocked_endpoints().max_connect_errors();
  }

  BaseProtocol::Type get_protocol() const { return routing_config_.protocol; }

  const std::string &get_name() const { return name_; }

  /**
   * identifier part of the name.
   *
   * name has the form 'routing:{id}'
   */
  std::string get_id() const {
    auto pos = name_.find(':');

    if (pos == name_.npos) return {};

    return name_.substr(pos + 1);
  }

  unsigned int get_net_buffer_length() const {
    return routing_config_.net_buffer_length;
  }

  std::chrono::milliseconds get_destination_connect_timeout() const {
    return std::chrono::milliseconds{routing_config_.connect_timeout * 1000};
  }

  std::chrono::milliseconds get_client_connect_timeout() const {
    return std::chrono::milliseconds{routing_config_.client_connect_timeout *
                                     1000};
  }

  std::chrono::milliseconds connect_retry_timeout() const {
    return routing_config_.connect_retry_timeout;
  }

  const mysql_harness::TcpDestination &get_bind_address() const {
    return routing_config_.bind_address;
  }

  const mysql_harness::Path &get_bind_named_socket() const {
    return routing_config_.named_socket;
  }

  SslMode source_ssl_mode() const noexcept {
    return routing_config_.source_ssl_mode;
  }
  SslMode dest_ssl_mode() const noexcept {
    return routing_config_.dest_ssl_mode;
  }

  /**
   * get the SSL context for the client side of the route.
   */
  TlsServerContext *source_ssl_ctx() const { return client_ssl_ctx_; }

  DestinationTlsContext *destination_ssl_config() const {
    return destination_tls_context_;
  }

  /**
   * get the SSL context for the server side of the route.
   *
   * @param dest_id  unique id of the destination
   * @param hostname name of the destination host
   *
   * @returns a TlsClientContext for the destination.
   * @retval nullptr if creating tls-context failed.
   */
  TlsClientContext *dest_ssl_ctx(const std::string &dest_id,
                                 const std::string &hostname) {
    if (destination_tls_context_ == nullptr) return nullptr;

    return destination_tls_context_->get(dest_id, hostname);
  }

  SharedQuarantineHandler &shared_quarantine() {
    return shared_quarantine_handler_;
  }

  const SharedQuarantineHandler &shared_quarantine() const {
    return shared_quarantine_handler_;
  }

  bool connection_sharing() const { return connection_sharing_; }

  void connection_sharing(const std::optional<bool> &is_enabled) {
    connection_sharing_ = is_enabled.has_value()
                              ? *is_enabled
                              : routing_config_.connection_sharing;
  }

  std::chrono::milliseconds connection_sharing_delay() const {
    return routing_config_.connection_sharing_delay;
  }

  routing::AccessMode access_mode() const {
    return routing_config_.access_mode;
  }

  bool wait_for_my_writes() const { return routing_config_.wait_for_my_writes; }

  std::chrono::seconds wait_for_my_writes_timeout() const {
    return routing_config_.wait_for_my_writes_timeout;
  }

  std::string dest_ssl_key() const { return routing_config_.dest_ssl_key; }
  std::string dest_ssl_cert() const { return routing_config_.dest_ssl_cert; }

  bool router_require_enforce() const {
    return routing_config_.router_require_enforce;
  }

  std::shared_ptr<routing_guidelines::Routing_guidelines_engine>
  get_routing_guidelines() const {
    return routing_guidelines_;
  }

  void set_router_info(routing_guidelines::Router_info router_info) {
    std::lock_guard<std::mutex> l{router_info_mtx_};
    router_info_ = std::move(router_info);
    router_info_.route_name = get_id();
    router_info_.bind_address = routing_config_.bind_address.hostname();
  }

  routing_guidelines::Router_info get_router_info() const {
    std::lock_guard<std::mutex> l{router_info_mtx_};
    return router_info_;
  }

 private:
  const RoutingConfig routing_config_;

  /** @brief Descriptive name of the connection routing */
  const std::string name_;

  mutable std::mutex mutex_conn_errors_;

  /** @brief memory in kilobytes allocated for thread's stack */
  size_t thread_stack_size_ = mysql_harness::kDefaultStackSizeInKiloBytes;

  TlsServerContext *client_ssl_ctx_{};

  DestinationTlsContext *destination_tls_context_{};

  /**
   * Routing guidelines engine used for the routing.
   */
  std::shared_ptr<routing_guidelines::Routing_guidelines_engine>
      routing_guidelines_{nullptr};

  mutable std::mutex router_info_mtx_;
  routing_guidelines::Router_info router_info_;

  // Connection sharing could be configured in routing plugin config, but it
  // could be overwritten by routing guidelines
  std::atomic<bool> connection_sharing_;

  /**
   * Callbacks for communicating with quarantined destination candidates
   * instance.
   */
  SharedQuarantineHandler shared_quarantine_handler_;

  BlockedEndpoints blocked_endpoints_;

 public:
  /** @brief Number of active routes */
  std::atomic<uint16_t> info_active_routes_{0};
  /** @brief Number of handled routes, not used at the moment */
  std::atomic<uint64_t> info_handled_routes_{0};
};
#endif /* ROUTING_CONTEXT_INCLUDED */
