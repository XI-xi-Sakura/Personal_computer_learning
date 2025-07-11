/*
  Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTING_MYSQL_ROUTING_BASE_INCLUDED
#define ROUTING_MYSQL_ROUTING_BASE_INCLUDED

#include "context.h"
#include "mysql/harness/destination.h"
#include "mysqlrouter/routing_component.h"  // MySQLRoutingAPI

/** @class MySQLRoutingBase
 *
 *  @brief Facade to avoid a tight coupling between Routing component and
 * actual routing endpoint implementation.
 *
 * Allows replacing the routing
 * endpoint with an alternative implementation.
 */
class ROUTING_EXPORT MySQLRoutingBase {
 public:
  MySQLRoutingBase() = default;

  MySQLRoutingBase(const MySQLRoutingBase &) = default;
  MySQLRoutingBase(MySQLRoutingBase &&) = default;

  MySQLRoutingBase &operator=(const MySQLRoutingBase &) = default;
  MySQLRoutingBase &operator=(MySQLRoutingBase &&) = default;

  virtual ~MySQLRoutingBase() = default;

  virtual MySQLRoutingContext &get_context() = 0;
  virtual int get_max_connections() const noexcept = 0;
  virtual std::vector<mysql_harness::Destination> get_destination_candidates()
      const = 0;
  virtual DestinationManager *destination_manager() = 0;
  virtual std::vector<MySQLRoutingAPI::ConnData> get_connections() = 0;
  virtual MySQLRoutingConnectionBase *get_connection(const std::string &) = 0;
  virtual bool is_accepting_connections() const = 0;
  virtual std::optional<routing::RoutingStrategy> get_routing_strategy()
      const = 0;
  virtual stdx::expected<void, std::string> restart_accepting_connections() = 0;
  virtual stdx::expected<void, std::string> start_accepting_connections() = 0;
  virtual void stop_socket_acceptors(const bool shutting_down) = 0;
  virtual bool is_standalone() const = 0;

  virtual bool is_running() const = 0;

  virtual mysqlrouter::ServerMode purpose() const = 0;
};

#endif  // ROUTING_MYSQL_ROUTING_BASE_INCLUDED
