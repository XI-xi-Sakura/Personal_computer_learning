/*
  Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_ROUTING_INCLUDE_MYSQLROUTER_DESTINATION_NODES_STATE_NOTIFIER_H_
#define ROUTER_SRC_ROUTING_INCLUDE_MYSQLROUTER_DESTINATION_NODES_STATE_NOTIFIER_H_

#include "mysqlrouter/routing_export.h"

#include <list>
#include <mutex>
#include <string>

#include "mysqlrouter/destination_status_types.h"

// first argument is the new set of the allowed nodes
// second argument is a set of nodes that can be used for new connections
// third argument is an indication whether we should disconnect existing
// connections (based on disconnect_on_metadata_unavailable setting)
// fourth argument is the description of the condition that triggered the change
// (like 'metadata change' etc.) can be used for logging purposes by the caller
using AllowedNodesChangedCallback =
    std::function<void(const AllowedNodes &, const AllowedNodes &, const bool,
                       const std::string &)>;
// NOTE: this has to be container like std::list that does not invalidate
// iterators when it is modified as we return the iterator to the inserted
// callback to the caller to allow unregistering
using AllowedNodesChangeCallbacksList = std::list<AllowedNodesChangedCallback>;
using AllowedNodesChangeCallbacksListIterator =
    AllowedNodesChangeCallbacksList::iterator;
// Starting a socket acceptor returns a value indicating if the start succeeded.
using StartSocketAcceptorCallback =
    std::function<stdx::expected<void, std::string>()>;
using StopSocketAcceptorCallback = std::function<void()>;
// First callback argument informs if the instances returned from the metadata
// has changed. Second argument is a list of new instances available after
// md refresh.
using MetadataRefreshCallback =
    std::function<void(const bool, const AllowedNodes &)>;
// Callback argument is a destination we want to check, value returned is
// true if the destination is quarantined, false otherwise.
using QueryQuarantinedDestinationsCallback =
    std::function<bool(const mysql_harness::Destination &)>;

/** @class DestinationNodesStateNotifier
 *
 * Allows the obervers to register for notifications on the change in the state
 * of the destination nodes.
 */
class ROUTING_EXPORT DestinationNodesStateNotifier {
 public:
  virtual ~DestinationNodesStateNotifier() = default;

  /** @brief Registers the callback for notification on the change in the
   *         state if the destination nodes.
   *
   * @param clb callback that should be called
   * @return identifier of the inserted callback, can be used to unregister
   *         the callback
   */
  AllowedNodesChangeCallbacksListIterator
  register_allowed_nodes_change_callback(
      const AllowedNodesChangedCallback &clb);

  /** @brief Unregisters the callback registered with
   * register_allowed_nodes_change_callback().
   *
   * @param it  iterator returned by the call to
   * register_allowed_nodes_change_callback()
   */
  void unregister_allowed_nodes_change_callback(
      const AllowedNodesChangeCallbacksListIterator &it);

  /**
   * Registers the callback for notification that the routing socket acceptor
   * should accept new connections.
   *
   * @param clb callback that should be called
   */
  void register_start_router_socket_acceptor(
      const StartSocketAcceptorCallback &clb);

  /**
   * Unregisters the callback registered with
   * register_start_router_socket_acceptor().
   */
  void unregister_start_router_socket_acceptor();

  /**
   * Registers the callback for notification that the routing socket acceptor
   * should stop accepting new connections.
   *
   * @param clb callback that should be called
   */
  void register_stop_router_socket_acceptor(
      const StopSocketAcceptorCallback &clb);

  /**
   * Unregisters the callback registered with
   * register_stop_router_socket_acceptor().
   */
  void unregister_stop_router_socket_acceptor();

  /**
   * Registers a callback that is going to be used on metadata refresh
   *
   * @param callback Callback that will be called on each metadata refresh.
   */
  void register_md_refresh_callback(const MetadataRefreshCallback &callback);

  /**
   * Unregisters the callback registered with
   * register_md_refresh_callback().
   */
  void unregister_md_refresh_callback();

  /**
   * Registers a callback that could be used for checking if the provided
   * destination candidate is currently quarantined.
   *
   * @param clb Callback to query unreachable destinations.
   */
  void register_query_quarantined_destinations(
      const QueryQuarantinedDestinationsCallback &clb);

  /**
   * Unregisters the callback registered with
   * register_query_quarantined_destinations().
   */
  void unregister_query_quarantined_destinations();

  /**
   * Specifies if the destination addresses can be added/removed from the set.
   *
   * NOTE: the sequence of nodes may change.
   */
  virtual bool is_dynamic();
  virtual std::string get_dynamic_plugin_name();

 protected:
  AllowedNodesChangeCallbacksList allowed_nodes_change_callbacks_;
  MetadataRefreshCallback md_refresh_callback_;
  StartSocketAcceptorCallback start_router_socket_acceptor_callback_;
  StopSocketAcceptorCallback stop_router_socket_acceptor_callback_;
  QueryQuarantinedDestinationsCallback query_quarantined_destinations_callback_;
  mutable std::mutex allowed_nodes_change_callbacks_mtx_;
  mutable std::mutex md_refresh_callback_mtx_;
  mutable std::mutex socket_acceptor_handle_callbacks_mtx;
  mutable std::mutex query_quarantined_destinations_callback_mtx_;
};

#endif  // ROUTER_SRC_ROUTING_INCLUDE_MYSQLROUTER_DESTINATION_NODES_STATE_NOTIFIER_H_
