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

#ifndef ROUTING_CONFIG_INCLUDED
#define ROUTING_CONFIG_INCLUDED

#include <string>

#include "mysql/harness/destination.h"
#include "mysql/harness/filesystem.h"  // Path
#include "mysqlrouter/routing.h"       // RoutingStrategy
#include "mysqlrouter/ssl_mode.h"
#include "protocol/protocol.h"  // Protocol::Type

/**
 * route specific configuration.
 */
class RoutingConfig {
 public:
  Protocol::Type protocol{};                   //!< protocol (classic, x)
  std::string destinations;                    //!< destinations
  bool accept_connections{true};
  int bind_port{};                             //!< TCP port to bind to
  mysql_harness::TcpDestination bind_address;  //!< IP address to bind to
  mysql_harness::Path named_socket;  //!< unix domain socket path to bind to
  int connect_timeout{};             //!< connect-timeout in seconds
  std::optional<routing::RoutingStrategy>
      routing_strategy;                     //!< routing strategy
  int max_connections{};                    //!< max connections allowed
  unsigned long long max_connect_errors{};  //!< max connect errors
  unsigned int client_connect_timeout{};  //!< client connect timeout in seconds
  unsigned int net_buffer_length{};       //!< Size of buffer to receive packets
  unsigned int thread_stack_size{};       //!< thread stack size in kilobytes

  SslMode source_ssl_mode{};         //!< SslMode of the client side connection.
  std::string source_ssl_cert;       //!< Cert file
  std::string source_ssl_key;        //!< Key file
  std::string source_ssl_cipher;     //!< allowed TLS ciphers
  std::string source_ssl_curves;     //!< allowed TLS curves
  std::string source_ssl_dh_params;  //!< DH params
  std::string
      source_ssl_ca_file;  //!< CA file to used to verify sources' identity
  std::string source_ssl_ca_dir;  //!< directory of CA files used to verify
                                  //!< sources' identity
  std::string
      source_ssl_crl_file;  //!< CRL file used to check revoked certificates
  std::string source_ssl_crl_dir;  //!< directory of CRL files

  SslMode dest_ssl_mode{};      //!< SslMode of the server side connection.
  std::string dest_ssl_cert;    //!< Cert file
  std::string dest_ssl_key;     //!< Key file
  SslVerify dest_ssl_verify{};  //!< How to verify the server-side cert.
  std::string dest_ssl_cipher;  //!< allowed TLS ciphers
  std::string
      dest_ssl_ca_file;  //!< CA file to used to verify destinations' identity
  std::string dest_ssl_ca_dir;  //!< directory of CA files used to verify
                                //!< destinations' identity
  std::string
      dest_ssl_crl_file;  //!< CRL file used to check revoked certificates
  std::string dest_ssl_crl_dir;  //!< directory of CRL files
  std::string dest_ssl_curves;   //!< allowed TLS curves

  bool connection_sharing{};  //!< if connection sharing is allowed.
  std::chrono::milliseconds
      connection_sharing_delay{};  //!< delay before an idling connection is
                                   //!< moved to the pool and connection sharing
                                   //!< is allowed.

  bool client_ssl_session_cache_mode{true};
  size_t client_ssl_session_cache_size{};
  unsigned int client_ssl_session_cache_timeout{};

  bool server_ssl_session_cache_mode{true};
  size_t server_ssl_session_cache_size{};
  unsigned int server_ssl_session_cache_timeout{};

  std::chrono::milliseconds
      connect_retry_timeout{};  //!< timeout of retrying after a transient
                                //!< connect-failure.
  routing::AccessMode access_mode{
      routing::AccessMode::kUndefined};  //!< read_write,read_only,auto

  bool wait_for_my_writes;
  std::chrono::seconds
      wait_for_my_writes_timeout{};  //!< how long to wait for writes to be
                                     //!< applied before reads.

  /*
   * read the users routing_require attribute from
   *
   *   information_schema.user_attributes
   *
   * and enforce them.
   */
  bool router_require_enforce{true};
};

#endif  // ROUTING_CONFIG_INCLUDED
