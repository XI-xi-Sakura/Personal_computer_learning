/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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

#define LOG_COMPONENT_TAG "sha256_password"

#include "sql/auth/sql_authentication.h"

#include <fcntl.h>
#include <mysql/components/my_service.h>
#include <sql/ssl_acceptor_context_operator.h>
#include <sql/ssl_init_callback.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string> /* std::string */
#include <utility>
#include <vector> /* std::vector */

#include "crypt_genhash_impl.h"  // generate_user_salt
#include "include/compression.h"
#include "m_string.h"
#include "map_helpers.h"
#include "mutex_lock.h"  // Mutex_lock
#include "my_byteorder.h"
#include "my_command.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "my_rnd.h"
#include "my_sys.h"
#include "my_time.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/log_shared.h"
#include "mysql/my_loglevel.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/service_my_plugin_log.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/service_mysql_password_policy.h"
#include "mysql/strings/m_ctype.h"
#include "mysql_time.h"
#include "mysqld_error.h"
#include "pfs_thread_provider.h"
#include "prealloced_array.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/auth/auth_internal.h"  // optimize_plugin_compare_by_pointer
#include "sql/auth/partial_revokes.h"
#include "sql/auth/sql_auth_cache.h"  // acl_cache
#include "sql/auth/sql_security_ctx.h"
#include "sql/conn_handler/connection_handler_manager.h"  // Connection_handler_manager
#include "sql/current_thd.h"                              // current_thd
#include "sql/debug_sync.h"                               // DEBUG_SYNC
#include "sql/derror.h"                                   // ER_THD
#include "sql/hostname_cache.h"  // Host_errors, inc_host_errors
#include "sql/log.h"             // query_logger
#include "sql/mysqld.h"          // global_system_variables
#include "sql/protocol.h"
#include "sql/protocol_classic.h"
#include "sql/psi_memory_key.h"  // key_memory_MPVIO_EXT_auth_info
#include "sql/sql_class.h"       // THD
#include "sql/sql_connect.h"     // thd_init_client_charset
#include "sql/sql_const.h"
#include "sql/sql_db.h"  // mysql_change_db
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"  // my_plugin_lock_by_name
#include "sql/sql_time.h"    // Interval
#include "sql/strfunc.h"
#include "sql/sys_vars_shared.h"  // find_static_system_variable
#include "sql/system_variables.h"
#include "sql/tztime.h"  // Time_zone
#include "sql_common.h"  // mpvio_info
#include "sql_string.h"
#include "string_with_len.h"
#include "strmake.h"
#include "template_utils.h"
#include "violite.h"

struct MEM_ROOT;

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

constexpr const std::array rsa_key_sizes{2048, 2048, 2048, 3072, 7680, 15360};

/**
   @file sql_authentication.cc

   AUTHENTICATION CODE

   including initial connect handshake, invoking appropriate plugins,
   client-server plugin negotiation, COM_CHANGE_USER, and native
   MySQL authentication plugins.
*/

/* clang-format off */
/**
  @page page_protocol_connection_phase Connection Phase

  The Connection Phase performs these tasks:
    - exchange the capabilities of client and server
    - setup SSL communication channel if requested
    - authenticate the client against the server

  It starts with the client connect()ing to the server which may send a
  ERR packet and finish the handshake or send a Initial Handshake Packet
  which the client answers with a Handshake Response Packet. At this stage
  client can request SSL connection, in which case an SSL communication
  channel is established before client sends its authentication response.

  @note In case the server sent a ERR packet as first packet it will happen
  before the client and server negotiated any capabilities.
  Therefore the ERR packet will not contain the SQL-state.

  After initial handshake, server informs client about the method to be used
  for authentication (unless it was already established during the handshake)
  and the authentication exchange continues until server either accepts
  connection by sending an @ref page_protocol_basic_ok_packet or rejects it
  with @ref page_protocol_basic_err_packet.

  @startuml
  (*) --> "Initial Handshake Packet"

  "Initial Handshake Packet" --> "Client Response"
  "Initial Handshake Packet" --> "SSL Exchange"
  "SSL Exchange" --> "Client Response"

  "Client Response" --> "Authentication method switch"
  "Client Response" --> "Authentication exchange continuation"
  "Client Response" --> [ Insufficient client capabilities] ERR

  "Authentication method switch" --> [ Client does not know requested auth method ] DISCONNECT
  "Authentication method switch" --> "Authentication exchange continuation"

  "Authentication exchange continuation" --> "Server Response"
  "Authentication exchange continuation" --> [ No more factors to authenticate] OK
  "Authentication exchange continuation" --> ERR

  "Server Response" --> "Authenticate 2nd Factor"
  "Server Response" --> "Authenticate 3rd Factor"

  "Authenticate 2nd Factor" --> "MFA Authentication Client Response"

  "Authenticate 3rd Factor" --> "MFA Authentication Client Response"

  "MFA Authentication Client Response" --> "Authentication exchange continuation"
  "MFA Authentication Client Response" --> [ Client does not know requested auth method ] DISCONNECT

  @enduml

  @section sect_protocol_connection_phase_initial_handshake Initial Handshake

  The initial handshake starts with the server sending the
  @ref page_protocol_connection_phase_packets_protocol_handshake packet.
  After this, optionally, the client can request an SSL connection to be
  established with the @ref page_protocol_connection_phase_packets_protocol_ssl_request
  packet and then the client sends the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response packet.

  @subsection sect_protocol_connection_phase_initial_handshake_plain_handshake Plain Handshake

  1. Server sending @ref page_protocol_connection_phase_packets_protocol_handshake.
  2. Client replying with @ref page_protocol_connection_phase_packets_protocol_handshake_response

  @startuml
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet
  Server -> Server: Check client capabilities and authentication method to use
  @enduml

  @subsection sect_protocol_connection_phase_initial_handshake_ssl_handshake SSL Handshake

  1. Server sending @ref page_protocol_connection_phase_packets_protocol_handshake
  2. Client replying with @ref page_protocol_connection_phase_packets_protocol_ssl_request
  3. The usual SSL exchange leading to establishing SSL connection
  4. Client sends @ref page_protocol_connection_phase_packets_protocol_handshake_response

  @startuml
  Server -> Client: Initial Handshake Packet
  Client -> Server: SSL Connection Request Packet
  == SSL Exchange ==
  Client -> Server: Handshake Response Packet
  Server -> Server: Check client capabilities and authentication method to use
  @enduml

  @subsection sect_protocol_connection_phase_initial_handshake_capabilities Capability Negotiation

  To permit an old client to connect to newer servers,
  the @ref page_protocol_connection_phase_packets_protocol_handshake contains

  * the MySQL Server version
  * the server's @ref group_cs_capabilities_flags

  The client should only announce the capabilities in the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  that it has in common with the server.

  They can agree on:
  * use of @ref CLIENT_TRANSACTIONS "status flags"
  * use of @ref CLIENT_PROTOCOL_41 "SQL states for error codes"
  * @ref sect_protocol_connection_phase_initial_handshake_auth_method "authentication methods"
  * @ref CLIENT_SSL "SSL Support"
  * @ref CLIENT_COMPRESS "Compression"

  @subsection sect_protocol_connection_phase_initial_handshake_auth_method Determining Authentication Method

  Method used for authentication is tied to the user account and stored in
  the plugin column of mysql.user table. Client informs about the user
  account it wants to log into in the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response packet.
  Only then server can look up the mysql.user table and find the authentication
  method to be used.

  However, to save round-trips, server and client start authentication exchange
  already in the initial handshake using an optimistic guess of the
  authentication method to be used.

  Server uses its default authentication method defined by @ref authentication_policy
  to produce initial authentication data payload and sends it to the client inside
  @ref page_protocol_connection_phase_packets_protocol_handshake, together with
  the name of the method used.

  Client can include in the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response packet
  its reply to the authentication data sent by the server.

  When including authentication reply in the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response,
  client is not obligated to use the same authentication method that was used
  by the server in the
  @ref page_protocol_connection_phase_packets_protocol_handshake packet.
  The name of the authentication method used by the client is stored in the
  packet. If the guessed authentication method used either by the client or
  the server in the initial handshake was not correct, server informs client
  which authentication method should be used using
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request.
  See section @ref sect_protocol_connection_phase_auth_method_mismatch for
  more details.

  Up to MySQL 4.0 the MySQL protocol only supported the
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication.
  In MySQL 4.1 the mysql_native_password method was added and in MySQL 5.5 arbitrtary
  authentication methods can be implemented by means of authentication plugins.

  In MySQL 9.0 the mysql_native_password was removed from server code.
  For compatibility reasons it is still present at client side, but it is converted
  from built-in into shared form.

  If the client or server do no support pluggable authentication
  (i.e. @ref CLIENT_PLUGIN_AUTH capability flag is not set) then
  authentication method used is inherited from client and server
  capabilities as follows:
    * The method used is
    @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
    if @ref CLIENT_PROTOCOL_41 or @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION"
    are not set.
    * The method used is mysql_native_password if both @ref CLIENT_PROTOCOL_41
    and @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION" are set, but
    @ref CLIENT_PLUGIN_AUTH is not set.

  @section sect_protocol_connection_phase_fast_path Authentication Phase Fast Path

  Assume the client wants to log in via user account U and that user account
  is defined to use authentication method `server_method`. The fast
  authentication path is used when:
  <ul>
  <li>the server used `server_method` to generate authentication data in the
    @ref page_protocol_connection_phase_packets_protocol_handshake packet.</li>
  <li>the client used a `client_authentication_method` in
    @ref page_protocol_connection_phase_packets_protocol_handshake_response
    that is compatible with the `server_method` used by the server.</li>
  </ul>

  In that case the first round of authentication has been already commenced
  during the handshake. Now, depending on the authentication method
  `server_method`, further authentication can be exchanged until the server
  either accepts or refuses the authentication.

  @subsection sect_protocol_connection_phase_fast_path_success Successful Authentication

  A successful fast authentication path looks as follows:

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client responds with
     @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. Client and server possibly exchange further packets as required by the server
     authentication method for the user account the client is trying to authenticate
     against.
  5. The server responds with an @ref page_protocol_basic_ok_packet

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet

  == Client and server possibly exchange further authentication method packets ==

  Server -> Client: OK packet

  == Client and server enter Command Phase ==
  @enduml

  The packets the server sends in step 4 are a
  @ref page_protocol_connection_phase_packets_protocol_auth_more_data packet
  prefixed with 0x01 to distinguish them from @ref page_protocol_basic_err_packet
  and @ref page_protocol_basic_ok_packet

  @note Many authentication methods, including the mysql_native_password method
  consist of a single challenge-response exchange. In that case no extra packet are
  exchanged in step 4 and the server sends an @ref page_protocol_basic_ok_packet
  directly after receiving the
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  packet (provided the authentication was successful).

  @note In MySQL 9.0 the mysql_native_password was removed from server code.
  For compatibility reasons it is still present at client side, but it is converted
  from built-in into shared form.

  @subsection sect_protocol_connection_phase_fast_path_fails Authentication Fails

  It goes exactly like @ref sect_protocol_connection_phase_fast_path_success,
  but if the server decides that it won't authenticate the user, it replies
  with an @ref page_protocol_basic_err_packet instead of
  @ref page_protocol_basic_ok_packet.

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client responds with
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. Client and server possibly exchange further packets as required by the server
  authentication method for the user account the client is trying to authenticate
  against.
  5. The server responds with an @ref page_protocol_basic_err_packet

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet

  == Client and server possibly exchange further authentication method packets ==

  Server -> Client: ERR packet

  == Client and server close connection ==
  @enduml

  Again, the @ref page_protocol_connection_phase_packets_protocol_auth_more_data
  packets sent by the server during step 4 start with 0x01 byte and thus can
  never be confused with the @ref page_protocol_basic_err_packet.

  @section sect_protocol_connection_phase_auth_method_mismatch Authentication Method Mismatch

  Assume that client wants to log in as user U and that user account uses
  authentication method M. If:

  1. Server's default method used to generate authentication payload for
  @ref page_protocol_connection_phase_packets_protocol_handshake was different
  from M or
  2. Method used by the client to generate authentication reply in
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  was not compatible with M
  then there is an authentication method mismatch and authentication exchange
  must be restarted using the correct authentication method.

  @note
  1. The mismatch can happen even if client and server used compatible
  authentication methods in the initial handshake, but the method the server
  used was different from the method required by the user account.
  2. In the 4.1-5.7 server and client the default authentication method is
  mysql_native_password
  3. In 8.0 server and client the default authentication method is
  @ref page_caching_sha2_authentication_exchanges.
  4. In 9.0 server the mysql_native_password was removed from the server code.
  For compatibility reasons it is still present at client side, but it is converted
  from built-in into shared form.
  5. The client and the server can change their default authentication method via the
  `--default-auth` option.
  . A sensibe thing to do for a client would be to see the server's default
  authentication method announced in the
  @ref page_protocol_connection_phase_packets_protocol_handshake packet and infer the
  authentication method from it instead of using the client default authentication
  method when producing
  @ref page_protocol_connection_phase_packets_protocol_handshake_response.
  But since there can be one to many server to client plugins and the clients
  generally do not know the mapping from server authentication methods to client
  authentication methods this is not implemented in the client mysql library.

  If authentication method mismatch happens, server sends to client the
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
  which contains the name of the client authentication method to be used and
  the first authentication payload generated by the new method. Client should
  switch to the requested authentication method and continue the exchange as
  dictated by that method.

  If the client does not know the requested method it should disconnect.

  @subsection sect_protocol_connection_phase_auth_method_mismatch_method_change Authentication Method Change

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client responds with
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. The server sends the
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request to tell
  the client that it needs to switch to a new authentication method.
  5. Client and server possibly exchange further packets as required by the server
  authentication method for the user account the client is trying to authenticate
  against.
  6. The server responds with an @ref page_protocol_basic_ok_packet or rejects
    with @ref page_protocol_basic_err_packet

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet
  Server -> Client: Authentication Switch Request Packet

  == Client and server possibly exchange further authentication method packets ==

  Server -> Client: ERR packet or OK packet
  @enduml

  @subsection sect_protocol_connection_phase_auth_method_mismatch_insuficcient_client Insufficient Client Capabilities

  Server will reject with @ref page_protocol_basic_err_packet if it discovers
  that client capabilities are not sufficient to complete authentication.
  This can happen in the following situations:

  <ul>
  <li>A client which does not support pluggable authentication
  (@ref CLIENT_PLUGIN_AUTH flag not set) connects to an account which uses
  authentication method different from mysql_native_password
  </li>
  <li>
  A client which does not support secure authentication (
  @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION" flag not set) attempts
  to connect.
  </li>
  <li>Server's default authentication method used to generate authentication
  data in @ref page_protocol_connection_phase_packets_protocol_handshake is
  incompatible with mysql_native_password and client does not support pluggable
  authentication (@ref CLIENT_PLUGIN_AUTH flag is not set).
  </li>
  </ul>

  In either of these cases authentication phase will look as follows:

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client response with
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. The server recognizes that the client does not have enough capabilities
  to handle the required authentication method, sends
  @ref page_protocol_basic_err_packet and closes the connection.

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet
  Server -> Client: Error Packet

  == server disconnects ==
  @enduml

  @subsection sect_protocol_connection_phase_auth_method_mismatch_unknown_auth_method New Authentication Method Not Known by Client

  Even if client supports external authentication (@ref CLIENT_PLUGIN_AUTH flag
  is set) the new authentication method indicated in
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request might
  not be known to it. In that case the client simply disconnects.

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client response with
  @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. The server sends the
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request to tell
  the client that it needs to switch to a new authentication method.
  5. client discovers that it does not know the authentication method requested by
  the server - it disconnects.

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet
  Server -> Client: Authentication Switch Packet

  == client disconnects ==
  @enduml


  @subsection sect_protocol_connection_phase_auth_method_mismatch_non_client_plugin_auth Non-CLIENT_PLUGIN_AUTH Clients

  @note 9.0 server no longer supports non-CLIENT_PLUGIN_AUTH Clients.
  Clients with no CLIENT_PLUGIN_AUTH capability are rejected.

  @note This can only happen on pre-8.0 servers. 8.0 has the
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  removed.

  @note In MySQL 9.0 the mysql_native_password was removed from server code.
  For compatibility reasons it is still present at client side, but it is converted
  from built-in into shared form.

  The only situation where server will request authentication method change from
  a client which does not set @ref CLIENT_PLUGIN_AUTH flag is when the following
  conditions hold:

  1. The client uses
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  for the @ref page_protocol_connection_phase_packets_protocol_handshake_response
  packet.
  2. The client supports secure authentication
  (@ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION" is set)
  3. Server's default authentication method is mysql_native_password

  In this case server sends
  @ref page_protocol_connection_phase_packets_protocol_old_auth_switch_request.
  This packet does not contain a new authenticartion method name because it's
  implicitly assumed to be mysql_native_password and it does not contain authentication data.
  Client replies with @ref sect_protocol_connection_phase_packets_protocol_handshake_response320.
  To generate a password hash the client should re-use the random bytes sent by
  the server in the
  @ref page_protocol_connection_phase_packets_protocol_handshake.

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet
  Server -> Client: Old Switch Request Packet
  Client -> Server: Old Handshake Response
  Server -> Client: ERR packet or OK packet
  @enduml


  @section sect_protocol_connection_phase_com_change_user_auth Authentication After COM_CHANGE_USER Command

  During @ref page_protocol_command_phase a client can send a ::COM_CHANGE_USER
  command which will trigger authenticating into a new account via a full
  authentication handshake.

  Similarly to the @ref page_protocol_connection_phase the server may reply
  with a @ref page_protocol_basic_err_packet or
  @ref page_protocol_basic_ok_packet for the usual fast-path or with
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
  containing the authentication method to be used for the new account
  and the first authentication data payload to be consumed by the client.
  Further handshake continues as usual, as defined by the authentication
  method of the new account. Eventually the server will accept the new
  account with @ref page_protocol_basic_ok_packet or it will reject the change
  with an @ref page_protocol_basic_err_packet and disconnect.

  1. The client sends ::COM_CHANGE_USER packet
  2. The server responds with the
    @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
    which initiates authentication handshake using the correct authentication
    method
  3. Client and server exchange further packets as required by the
    authentication method for the new account
  4. The server responds with @ref page_protocol_basic_ok_packet and returns
    to command phase or @ref page_protocol_basic_err_packet and closes
    the connection.

  @startuml
  Client -> Server: COM_CHANGE_USER
  Server -> Client: Auth Switch Request Packet
  == packets exchanged depending on the authentication method ==
  Server -> Client: ERR packet or OK packet
  @enduml

  @subsection sect_protocol_connection_phase_com_change_user_auth_non_plugin COM_CHANGE_USER and Non-CLIENT_PLUGIN_AUTH Clients

  @note 9.0 server no longer supports non-CLIENT_PLUGIN_AUTH Clients.
  Clients with no CLIENT_PLUGIN_AUTH capability are rejected.

  @note In MySQL 9.0 the mysql_native_password was removed from server code.
  For compatibility reasons it is still present at client side, but it is converted
  from built-in into shared form.

  Clients which do not support pluggable authentication can send
  ::COM_CHANGE_USER command for accounts which use mysql_native_password or
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication.
  In this case it is assumed that server has already sent the authentication
  challenge - the same which was sent when the client connected for the first
  time - and client's reply to that challenge, i.e. the hash of the new
  password, should be sent in the `auth_response` field of ::COM_CHANGE_USER.

  1. The client sends ::COM_CHANGE_USER packet with authentication response
  (hash of the password) for mysql_native_password (post 4.1 clients) or
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  (pre 4.1 clients) method.
  2. The server responds with an @ref page_protocol_basic_ok_packet and returns
  to @ref page_protocol_command_phase or with an
  @ref page_protocol_basic_err_packet and closes the connection.

  @startuml
  Client -> Server : COM_CHANGE_USER with a password hash
  Server -> Client : ERR packet or OK packet.
  @enduml

  As during normal connection, it is also possible that a post 4.1 client which
  does not support pluggable authentication connects to an account which uses
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  In that case server will send
  @ref page_protocol_connection_phase_packets_protocol_old_auth_switch_request
  and expect the client to reply with
  @ref sect_protocol_connection_phase_packets_protocol_handshake_response320

  1. The client sends ::COM_CHANGE_USER packet with response for mysql_native_password
  2. The server replies with
  @ref page_protocol_connection_phase_packets_protocol_old_auth_switch_request (0xFE byte)
  3. The client sends response again, this time in the form required by
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  4. The server responds with an @ref page_protocol_basic_ok_packet and returns
  to @ref page_protocol_command_phase or an @ref page_protocol_basic_err_packet
  and disconnects

  @startuml
  Client -> Server : COM_CHANGE_USER with a password hash
  Server -> Client : Old Switch Request Packet
  Client -> Server : Old Password Auth Response
  Server -> Client : ERR packet or OK packet
  @enduml

  @sa group_cs_capabilities_flags
  @sa unknown_accounts
  @subpage page_protocol_connection_phase_packets
  @subpage page_protocol_connection_phase_authentication_methods
  @subpage page_protocol_multi_factor_authentication_methods
*/


/**
  @page page_protocol_basic_expired_passwords Expired Password

  Since MySQL 5.6.7, a MySQL account can be expired.
  If a account is expired, the session is in a restricted mode which
  only permits SET PASSWORD = .. and similar SET commands.
  Other statements will fail with an error like this:
  ~~~~~~~~
  mysql> SELECT 1;
  ERROR 1820 (HY000): You must SET PASSWORD before executing this statement
  ~~~~~~~~

  Not all clients can properly deal with that error.
  So on the protocol side exists a safeguard
  ::CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS
  @ref group_cs_capabilities_flags "capability flag" exists to prevent
  clients from entering this "sandbox" mode.
  Only clients that can handle this sandbox mode should report
  ::CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS on.
  Usually this means all interactive clients and all applications that got
  adjusted to handle the relevant SQL error.

  If a client is not setting that capability and it tries to login with an
  account that has an expired password, the server will return an
  @ref page_protocol_basic_err_packet for the
  @ref page_protocol_connection_phase or the ::COM_CHANGE_USER request.

  The idea is to block any activity until the password is reset.

  @sa ::MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS, mysql_options,
  ACL_USER::password_expired, ACL_USER::password_lifetime,
  acl_authenticate
*/


/**
   @page page_protocol_connection_phase_authentication_methods Authentication Methods

   To authenticate a user against the server the client server protocol employs one of
   several authentication methods.

   As of MySQL 5.5 the authentication method to be used to authenticate
   connections to a particular MySQL account is indicated in the mysql.user table.
   For earlier servers it's always mysql native authentication or
   old password authentication depending on
   the @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION" flag.

   Client and server negotiate what types of authentication they support as part of the
   @ref page_protocol_connection_phase and
   @ref sect_protocol_connection_phase_initial_handshake_auth_method.

   Each authentication method consists of
     * a client plugin name
     * a server plugin name
     * a specific exchange

   The exchanged input and output data may either be sent as part of the
   @ref page_protocol_connection_phase_packets_protocol_handshake and the
   @ref page_protocol_connection_phase_packets_protocol_handshake_response
   or as a part of the
   @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
   and following packets. The structure is usually the same.

   @section page_protocol_connection_phase_authentication_methods_limitations Limitations

   While the overall exchange of data is free-form there are some limitations
   in the initial handshake of the amount of data that can be exchanged without
   causing an extra round trip:

   <ul>
   <li>
   The `auth_plugin_data` field in
   @ref page_protocol_connection_phase_packets_protocol_handshake packet can
   only carry 255 bytes max (see @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION").
   </li><li>
   The `auth_reponse_data` field in
   @ref page_protocol_connection_phase_packets_protocol_handshake_response
   packet can only carry 255 bytes max too if
   @ref CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA is not set.
   </li><li>
   The client-side plugin may not receive its initial data in the initial handshake
   </li>
   </ul>

   @section page_protocol_connection_phase_authentication_methods_old_password_authentication Old Password Authentication

   Authentication::Old:

   <ul>
   <li>
   The server name is *mysql_old_password*
   </li>
   <li>
   The client name is *mysql_old_password*
   </li>
   <li>
   Client side requires an 8-byte random challenge from server
   </li>
   <li>
   Client side sends a 8 byte response packet based on a proprietary algorithm.
   </li>
   </ul>

   @note If the server announces mysql_native_password in the
   @ref page_protocol_connection_phase_packets_protocol_handshake packet
   the client may use the first 8 bytes of its 20-byte auth_plugin_data as input.

   @startuml
   Client->Server: 8 byte random data
   Server->client: 8 byte scrambled password
   @enduml

   @warning The hashing algorithm used for this auth method is *broken* as
   shown in CVE-2000-0981.

   @subpage page_caching_sha2_authentication_exchanges
   @subpage page_protocol_connection_phase_authentication_methods_clear_text_password
   @subpage page_protocol_connection_phase_authentication_methods_authentication_windows
   @subpage page_webauthn_authentication_exchanges
*/

/**
  @page page_protocol_connection_phase_packets_protocol_handshake Protocol::Handshake

  Initial Handshake %Packet

  When the client connects to the server the server sends a handshake
  packet to the client. Depending on the server version and configuration
  options different variants of the initial packet are sent.

  To permit the server to add support for newer protocols, the first byte
  defines the protocol version.

  Since 3.21.0 the @ref page_protocol_connection_phase_packets_protocol_handshake_v10
  is sent.

  * @subpage page_protocol_connection_phase_packets_protocol_handshake_v9
  * @subpage page_protocol_connection_phase_packets_protocol_handshake_v10
*/

/**
  @page page_protocol_connection_phase_packets Connection Phase Packets

  @subpage page_protocol_connection_phase_packets_protocol_handshake
  @subpage page_protocol_connection_phase_packets_protocol_ssl_request
  @subpage page_protocol_connection_phase_packets_protocol_handshake_response
  @subpage page_protocol_connection_phase_packets_protocol_auth_switch_request
  @subpage page_protocol_connection_phase_packets_protocol_old_auth_switch_request
  @subpage page_protocol_connection_phase_packets_protocol_auth_switch_response
  @subpage page_protocol_connection_phase_packets_protocol_auth_more_data
  @subpage page_protocol_connection_phase_packets_protocol_auth_next_factor_request
*/

/**
  @page page_webauthn_authentication_exchanges authentication_webauthn information
page_webauthn_authentication_exchanges
  @section sect_webauthn_definition Definition
  <ul>
  <li>
  The server side plugin name is *authentication_webauthn*
  </li>
  <li>
  The client side plugin name is *authentication_webauthn_client*
  </li>
  <li>
  Account - user account (user-host combination)
  </li>
  <li>
  authentication_string - Transformation of Credential ID stored in mysql.user table
  </li>
  <li>
  relying party ID - Unique name assigned to server by authentication_webauthn plugin
  </li>
  <li>
  FIDO authenticator - A hardware token device
  </li>
  <li>
  Salt - 32 byte long random data
  </li>
  <li>
  Registration mode - Refers to state of connection where only ALTER USER is allowed
  to do registration steps.
  </li>
  </ul>

  @section sect_webauthn_info How authentication_webauthn works?

  Plugin authentication_webauthn works in two phases.
  <ul>
   <li>
    Registration of hardware token device
   </li>
   <li>
    Authentication process
   </li>
  </ul>

  Registration process:
  This is a 2 step process for a given user account.
  <ul>
   <li>
    Initiate registration step.
   </li>
   <li>
    Finish registration step.
   </li>
  </ul>

  Initiate registration:
  --register-factor mysql client option initiates registration step.

  <ol>
   <li>
    Client executes ALTER USER user() nth FACTOR INITIATE REGISTRATION;
   </li>
   <li>
   Server sends a challenge comprising of 1 byte capability bit, 32 bytes random salt, relying party ID
   Format of challenge is:
    <ul>
      <li>1 byte capability</li>
      <li>length encoded 32 bytes random salt</li>
      <li>length encoded relying party ID</li>
      <li>length encoded user id (`user name`\@`host name`)
    </ul>

   Server also sends name of the client plugin - In this case authentication_webauthn_client.
   </li>
   <li>
   Client receives challenge and client plugin name.
   It then passes challenge to authentication_webauthn_client plugin
   with option "registration_challenge" using mysql_plugin_options()
   </li>
   <li>
    FIDO authenticator may prompt to enter device pin.
    By default pin can be provided via standard input.
    Alternatively, register a callback with option "authentication_webauthn_client_callback_get_password"
    using mysql_plugin_options() to provide pin.
    FIDO authenticator prompts to perform gesture action.
    This message can be accessed via callback. Register a callback with option
    "authentication_webauthn_client_messages_callback" using mysql_plugin_options()
   </li>
   <li>
    Once gesture action (touching the token) is performed,
    FIDO authenticator generates a public/private key pair, a credential attestation statement (
    X.509 certificate, signature) and authenticator data.
   </li>
   <li>
   Client extracts registration response(aka challenge response) from authentication_webauthn_client
   plugin with option "registration_response" using mysql_plugin_get_option()
   Response is encoded in base64. Format of challenge response is:
    <ul>
       <li>1 bytes capability</li>
       <li>length encoded authenticator data</li>
       <li>length encoded signature</li>
       <li>length encoded x509 certificate</li>
       <li>length encoded Client data JSON</li>
       <li>length encoded full credential attestation statement CBOR, if capability has the SEND_FULL_ATTESTATION_BLOB on</li>
       <li>length encoded algoritm used for the authentication data, if capability has the SEND_FULL_ATTESTATION_BLOB on</li>
    </ul>
   </li>
  </ol>

  Finish registration:
  <ol>
   <li>
    Client executes ALTER USER user() nth FACTOR FINISH REGISTRATION SET CHALLENGE_RESPONSE AS '?';
    parameter is binded to challenge response received during initiate registration step.
   </li>
   <li>
    authentication_webauthn plugin verifies the challenge response and responds with an
    @ref page_protocol_basic_ok_packet or rejects with @ref page_protocol_basic_err_packet
   </li>
  </ol>
       @startuml
         title Registration

         participant server as "MySQL server"
         participant client as "Client"
         participant authenticator as "FIDO authenticator"

         == Initiate registration ==

         client -> server : connect
         server -> client : OK packet. Connection is in registration mode where only ALTER USER command is allowed
         client -> server : ALTER USER USER() nth FACTOR INITIATE REGISTRATION
         server -> client : random challenge (capability, 32 byte random salt, relying party ID, user id)
         client -> authenticator : random challenge
         authenticator -> client : challenge response (capability, authenticator data, signature, x509 certificate, client data json, attestation, algorithm id)

         == Finish registration ==

         client -> server : ALTER USER USER() nth FACTOR FINISH REGISTRATION SET CHALLENGE_RESPONSE = 'challenge response'
         server -> client : Ok packet upon successful verification of credential ID
       @enduml

  Authentication process:
  Once initial authentication methods defined for user account are successful,
  server initiates webauthn authentication process. This includes following steps:
   <ol>
    <li>
     Server sends a 32 byte random salt, relying party ID to client.
     Format is:
     <ul>
       <li>1 byte capability</li>
       <li>length encoded 32 byte random salt</li>
       <li>length encoded relying party ID</li>
     </ul>
    </li>
    <li>
     Client receives them and checks if FIDO device has CTAP2(aka fido2) capability.
    </li>
    <li>
     If FIDO device is not capable of CTAP2, client requests server to send credential ID.
     Format is:
     | 0x01 |
    </li>
    <li>
     Server sends credential ID (or empty string if unavailable) to client.
     Format is:
     | length encoded credential ID |
    </li>
    <li>
     If device has CTAP2 capability and if user has configured preserve-privacy option,
     client prompts user to enter pin.
     client then retrieves all credentials for given relying party ID from FIDO authenticator.
    </li>
    <li>
     Client prompts user to choose from the list of credentials.
    </li>
    <li>
     Client sends random salt, relying party ID and optionally credential ID OR
     resident key identifier to FIDO authenticator.
    </li>
    <li>
     FIDO authenticator prompts to perform gesture action.
    </li>
    <li>
     For CTAP2 capable device, FIDO authenticator extracts one (in case of preserve-privacy
     option) or all private key based on relying party ID and signs the challenge.
    </li>
    <li>
     For non-CTAP2 devices, FIDO authenticator extracts private key based on
     relying party ID and credential ID received from server and signs the challenge.
    </li>
    <li>
     Client sends signed challenge to server.
     Format:
     <ul>
       <li>0x02</li>
       <li>length encoded number of assertions</li>
       <li>length encoded authenticator data</li>
       <li>length encoded signature, ...</li>
       <li>length encoded authenticator data</li>
       <li>length encoded signature</li>
       <li>client data json</li>
     </ul>
    </li>
    <li>
     Server side webauthn authentication plugin verifies the signature with the
     public key and responds with an @ref page_protocol_basic_ok_packet or with
     @ref page_protocol_basic_err_packet
    </li>
   </ol>
       @startuml
         title Authentication

         participant server as "MySQL server"
         participant client as "Client"
         participant authenticator as "FIDO authenticator"

         == Authentication ==

         client -> server : connect
         server -> client : OK packet
         server -> client : send client side webauthn authentication plugin name in OK packet
         server -> client : sends 32 byte random salt, relying party ID
         client -> authenticator : check CTAP2 capability
         client -> server: send request for credential ID if device is not CTAP2 capable
         server -> client: send credential ID if available or emplty string
         client -> authenticator: Retrieve credentials for given relying party ID if preserve-privacy preference is specified
         client -> authenticator : sends 32 byte random salt, relying party ID, credential ID OR resident key identifier
         authenticator -> client : signed challenge
         client -> server : signed challenge
         server -> client : verify signed challenge and send OK or ERR packet
       @enduml

  @section sect_webauthn_packet_info Packet Information

  @subsection subsect_webauthn_packet_registration Packets related to registration

  When client sends ALTER USER &lt;username&gt; &lt;N&gt; FACTOR INITIATE REGISTRATION and
  if user is using authentication_webauthn for given factor, server response will
  contain registration challege received from server plugin. Following is the format
  of such a challenge.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>capability flags</td>
    <td>Can be a combination of RESIDENT_KEYS(0x01) and SEND_FULL_ATTESTATION_BLOB(0x02)</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string[32]"</td>
    <td>random data</td>
    <td>32 bytes random string </td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>Relying Party ID</td>
    <td>Variable length Relying Party ID set by authentication_webauthn_rp_id</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>Username</td>
    <td>Variable length username information</td></tr>
  </table>


  In response to registration challenge, client plugin calculates response and sends
  it to server as a part of ALTER USER &lt;username&gt; &lt;N&gt; FACTOR FINISH REGISTRATION

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>capability flags</td>
    <td>Can be a combination of RESIDENT_KEYS(0x01) and SEND_FULL_ATTESTATION_BLOB(0x02)</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string[32]"</td>
    <td>authenticator data</td>
    <td>length encoded challenge response received as a part of FIDO registration. Not used if attestation blob is sent</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>X509 Certificate</td>
    <td>length encoded X509 certificate received as a part of FIDO registration. Not used if attestation blob is sent</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>ClientDataJSON</td>
    <td>length encoded client data JSON used for calculating response</td></tr>
  <tr><td colspan="3">if capabilities @& SEND_FULL_ATTESTATION_BLOB {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>Attestation blob</td>
    <td>length encoded CBOR formatted attestation statenment blob serialization</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>authentication data format</td>
    <td>length encoded format name string. Can be packed, fido-u2f, tpm or none</td></tr>
  <tr><td colspan="3">} // SEND_FULL_ATTESTATION_BLOB</td></tr>
  </table>

  @subsection subsect_webauthn_packet_authentication Packets related to authentication

  As a part of @ref page_protocol_connection_phase_packets_protocol_auth_next_factor_request,
  server plugin sends following information to client.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0x01 </td>
    <td>capability</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string[32]"</td>
    <td>random data</td>
    <td>32 bytes random string </td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>Relying Party ID</td>
    <td>Variable length Relying Party ID set by authentication_webauthn_rp_id</td></tr>
  </table>


  If client plugin detects that FIDO device is not capable of CTAP2, it requests
  server plugin for the same using following.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0x01 (1) </td>
    <td>Credential ID request packet</td></tr>
  </table>


  When server plugin receive request for credential ID, it sends it in following format.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>credential data</td>
    <td>Variable length credential ID</td></tr>
  </table>


  Client plugin sends final authentication reply in following format

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0x02 (1) </td>
    <td>Assertion information</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_int_le "int&lt;lenenc&gt;"</td>
    <td>number_of_assertions</td>
    <td>length encoded number of assertions</td></tr>
  <tr><td colspan="3">if number_of_assertions > 0, for each {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>authenticator data</td>
    <td>Variable length authdata obtained as a part of FIDO assertion</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>authenticator data</td>
    <td>Variable length signed challenge obtained as a part of FIDO assertion</td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string[<var>]"</td>
    <td>Clientdata JSON</td>
    <td>Variable length JSON client data used for assertion</td></tr>
  </table>
*/
/* clang-format on */

const uint MAX_UNKNOWN_ACCOUNTS = 1000;
/**
  Hash to map unknown accounts to an authentication plugin.

  If unknown accounts always map to default authentication plugin,
  server's reply to switch authentication plugin would indicate that
  user in question is indeed a valid user.

  To counter this, one of the built-in authentication plugins is chosen
  at random. Thus, a request to switch authentication plugin is not and
  indicator of a valid user account.

  For same unknown account, if different plugin is chosen every time,
  that again is an indicator. To resolve this, a hashmap is  used to
  store information about unknown account => authentication plugin.
  This way, if same unknown account appears again, same authentication
  plugin is chosen again.

  However, size of such a hash has to be kept under control. Hence,
  once MAX_UNKNOWN_ACCOUNTS lim
*/
Map_with_rw_lock<Auth_id, uint> *unknown_accounts = nullptr;

inline const char *client_plugin_name(plugin_ref ref) {
  return ((st_mysql_auth *)(plugin_decl(ref)->info))->client_auth_plugin;
}

LEX_CSTRING validate_password_plugin_name = {
    STRING_WITH_LEN("validate_password")};

const LEX_CSTRING Cached_authentication_plugins::cached_plugins_names[(
    uint)PLUGIN_LAST] = {{STRING_WITH_LEN("caching_sha2_password")},
                         {STRING_WITH_LEN("sha256_password")}};

LEX_CSTRING default_auth_plugin_name{STRING_WITH_LEN("caching_sha2_password")};

/**
  Use known pointers for cached plugins to improve comparison time

  @param  [in] plugin Name of the plugin
*/
void Cached_authentication_plugins::optimize_plugin_compare_by_pointer(
    LEX_CSTRING *plugin) {
  DBUG_TRACE;
  for (uint i = 0; i < (uint)PLUGIN_LAST; ++i) {
    if (my_strcasecmp(system_charset_info, cached_plugins_names[i].str,
                      plugin->str) == 0) {
      plugin->str = cached_plugins_names[i].str;
      plugin->length = cached_plugins_names[i].length;
      return;
    }
  }
}

/**
  Cached_authentication_plugins constructor

  Cache plugin_ref for each plugin in cached_plugins_names list
*/
Cached_authentication_plugins::Cached_authentication_plugins() {
  DBUG_TRACE;
  m_valid = true;
  for (uint i = 0; i < (uint)PLUGIN_LAST; ++i) {
    if (cached_plugins_names[i].str[0]) {
      cached_plugins[i] = my_plugin_lock_by_name(
          nullptr, cached_plugins_names[i], MYSQL_AUTHENTICATION_PLUGIN);
      if (!cached_plugins[i]) m_valid = false;
    } else
      cached_plugins[i] = nullptr;
  }
}

/**
  Cached_authentication_plugins destructor

  Releases all plugin_refs
*/
Cached_authentication_plugins::~Cached_authentication_plugins() {
  DBUG_TRACE;
  for (uint i = 0; i < (uint)PLUGIN_LAST; ++i) {
    if (cached_plugins[i]) plugin_unlock(nullptr, cached_plugins[i]);
  }
}

/**
  Get plugin_ref if plugin is cached

  @param [in] plugin Name of the plugin

  @returns cached plugin_ref if found, 0 otherwise.
*/
plugin_ref Cached_authentication_plugins::get_cached_plugin_ref(
    const LEX_CSTRING *plugin) {
  plugin_ref cached_plugin = nullptr;
  LEX_CSTRING plugin_cstring;
  DBUG_TRACE;
  if (!plugin || !plugin->str || !this->is_valid()) return cached_plugin;

  plugin_cstring.str = plugin->str;
  plugin_cstring.length = plugin->length;
  this->optimize_plugin_compare_by_pointer(&plugin_cstring);

  for (uint i = 0; i < (uint)PLUGIN_LAST; ++i) {
    if (plugin_cstring.str == cached_plugins_names[i].str) {
      cached_plugin = cached_plugins[i];
      return cached_plugin;
    }
  }
  return cached_plugin;
}

/*
  Fetch the SSL security level
*/
int security_level(void) {
  int current_sec_level = 2;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  /*
    create a temporary SSL_CTX, we're going to use it to fetch
    the current OpenSSL security level. So that we can generate
    keys accordingly.
  */
  SSL_CTX *temp_ssl_ctx = SSL_CTX_new(TLS_server_method());

  /* get the current security level */
  current_sec_level = SSL_CTX_get_security_level(temp_ssl_ctx);

  assert(current_sec_level <= 5);

  /* current range for security level is [1,5] */
  if (current_sec_level > 5)
    current_sec_level = 5;
  else if (current_sec_level <= 1)
    current_sec_level = 2;

  /* get rid of temp_ssl_ctx, we're done with it */
  SSL_CTX_free(temp_ssl_ctx);
#endif
  DBUG_EXECUTE_IF("crypto_policy_3", current_sec_level = 3;);
  return current_sec_level;
}

Cached_authentication_plugins *g_cached_authentication_plugins = nullptr;

bool disconnect_on_expired_password = true;

extern bool initialized;

/** Size of the header fields of an authentication packet. */
#define AUTH_PACKET_HEADER_SIZE_PROTO_41 32
#define AUTH_PACKET_HEADER_SIZE_PROTO_40 5

#define MAX_CIPHER_LENGTH 1024
#define SHA256_PASSWORD_MAX_PASSWORD_LENGTH MAX_PLAINTEXT_LENGTH

#define DEFAULT_SSL_CLIENT_CERT "client-cert.pem"
#define DEFAULT_SSL_CLIENT_KEY "client-key.pem"

#define MAX_CN_NAME_LENGTH 64

bool opt_auto_generate_certs = true;

bool auth_rsa_auto_generate_rsa_keys = true;

static bool do_auto_rsa_keys_generation();

char *auth_rsa_private_key_path;
char *auth_rsa_public_key_path;
Rsa_authentication_keys *g_sha256_rsa_keys = nullptr;

bool Thd_charset_adapter::init_client_charset(uint cs_number) {
  if (thd_init_client_charset(thd, cs_number)) return true;
  thd->update_charset();
  return thd->is_error();
}

const CHARSET_INFO *Thd_charset_adapter::charset() { return thd->charset(); }

/**
  @brief Set key file path

  @param [in] key            Points to either m_private_key_path or
                             m_public_key_path.
  @param [out] key_file_path Stores value of actual key file path.

*/
void Rsa_authentication_keys::get_key_file_path(char *key,
                                                String *key_file_path) {
  /*
     If a fully qualified path is entered use that, else assume the keys are
     stored in the data directory.
   */
  if (strchr(key, FN_LIBCHAR) != nullptr
#ifdef _WIN32
      || strchr(key, FN_LIBCHAR2) != nullptr
#endif
  )
    key_file_path->set_quick(key, strlen(key), system_charset_info);
  else {
    key_file_path->append(mysql_real_data_home, strlen(mysql_real_data_home));
    if ((*key_file_path)[key_file_path->length()] != FN_LIBCHAR)
      key_file_path->append(FN_LIBCHAR);
    key_file_path->append(key);
  }
}

/**
  @brief Read a key file and store its value in RSA structure

  @param [out] key_ptr         Address of pointer to RSA. This is set to
                               point to a non null value if key is correctly
                               read.
  @param [in] is_priv_key      Whether we are reading private key or public
                               key.
  @param [out] key_text_buffer To store key file content of public key.

  @return Error status
    @retval false              Success : Either both keys are read or none
                               are.
    @retval true               Failure : An appropriate error is raised.
*/
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
bool Rsa_authentication_keys::read_key_file(EVP_PKEY **key_ptr,
                                            bool is_priv_key,
                                            char **key_text_buffer) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
bool Rsa_authentication_keys::read_key_file(RSA **key_ptr, bool is_priv_key,
                                            char **key_text_buffer) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  String key_file_path;
  char *key;
  const char *key_type;
  FILE *key_file = nullptr;

  key = is_priv_key ? *m_private_key_path : *m_public_key_path;
  key_type = is_priv_key ? "private" : "public";
  *key_ptr = nullptr;

  get_key_file_path(key, &key_file_path);

  /*
     Check for existence of private key/public key file.
  */
  if ((key_file = fopen(key_file_path.c_ptr(), "rb")) == nullptr) {
    LogErr(WARNING_LEVEL, ER_AUTH_RSA_CANT_FIND, key_type,
           key_file_path.c_ptr());
  } else {
    *key_ptr = is_priv_key
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
                   ? PEM_read_PrivateKey(key_file, nullptr, nullptr, nullptr)
                   : PEM_read_PUBKEY(key_file, nullptr, nullptr, nullptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
                   ? PEM_read_RSAPrivateKey(key_file, nullptr, nullptr, nullptr)
                   : PEM_read_RSA_PUBKEY(key_file, nullptr, nullptr, nullptr);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

    if (!(*key_ptr)) {
      char error_buf[MYSQL_ERRMSG_SIZE];
      ERR_error_string_n(ERR_get_error(), error_buf, MYSQL_ERRMSG_SIZE);
      LogErr(ERROR_LEVEL, ER_AUTH_RSA_CANT_PARSE, key_type,
             key_file_path.c_ptr(), error_buf);

      /*
        Call ERR_clear_error() just in case there are more than 1 entry in the
        OpenSSL thread's error queue.
      */
      ERR_clear_error();
      fclose(key_file);
      return true;
    }

    /* For public key, read key file content into a char buffer. */
    bool read_error = false;
    if (!is_priv_key) {
      int filesize;
      fseek(key_file, 0, SEEK_END);
      filesize = ftell(key_file);
      fseek(key_file, 0, SEEK_SET);
      *key_text_buffer = new char[filesize + 1];
      const int items_read = fread(*key_text_buffer, filesize, 1, key_file);
      read_error = items_read != 1;
      if (read_error) {
        char errbuf[MYSQL_ERRMSG_SIZE];
        LogErr(ERROR_LEVEL, ER_AUTH_RSA_CANT_READ,
               my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
      }
      (*key_text_buffer)[filesize] = '\0';
    }
    fclose(key_file);
    return read_error;
  }
  return false;
}

void Rsa_authentication_keys::free_memory() {
  if (m_private_key)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY_free(m_private_key);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    RSA_free(m_private_key);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  if (m_public_key) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY_free(m_public_key);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    RSA_free(m_public_key);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    m_cipher_len = 0;
  }

  if (m_pem_public_key) delete[] m_pem_public_key;
}

void *Rsa_authentication_keys::allocate_pem_buffer(size_t buffer_len) {
  m_pem_public_key = new char[buffer_len];
  return m_pem_public_key;
}

int Rsa_authentication_keys::get_cipher_length() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  return (m_cipher_len = EVP_PKEY_get_size(m_public_key));
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  return (m_cipher_len = RSA_size(m_public_key));
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
}

/**
  @brief Read RSA private key and public key from file and store them
         in m_private_key and m_public_key. Also, read public key in
         text format and store it in m_pem_public_key.

  @return Error status
    @retval false        Success : Either both keys are read or none are.
    @retval true         Failure : An appropriate error is raised.
*/
bool Rsa_authentication_keys::read_rsa_keys() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_PKEY *rsa_private_key_ptr = nullptr;
  EVP_PKEY *rsa_public_key_ptr = nullptr;
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *rsa_private_key_ptr = nullptr;
  RSA *rsa_public_key_ptr = nullptr;
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  char *pub_key_buff = nullptr;

  if ((strlen(*m_private_key_path) == 0) && (strlen(*m_public_key_path) == 0)) {
    LogErr(INFORMATION_LEVEL, ER_AUTH_RSA_FILES_NOT_FOUND);
    return false;
  }

  /*
    Read private key in RSA format.
  */
  if (read_key_file(&rsa_private_key_ptr, true, nullptr)) return true;

  /*
    Read public key in RSA format.
  */
  if (read_key_file(&rsa_public_key_ptr, false, &pub_key_buff)) {
    if (rsa_private_key_ptr)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
      EVP_PKEY_free(rsa_private_key_ptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
      RSA_free(rsa_private_key_ptr);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    return true;
  }

  /*
     If both key files are read successfully then assign values to following
     members of the class
     1. m_pem_public_key
     2. m_private_key
     3. m_public_key

     Else clean up.
   */
  if (rsa_private_key_ptr && rsa_public_key_ptr) {
    const size_t buff_len = strlen(pub_key_buff);
    char *pem_file_buffer = (char *)allocate_pem_buffer(buff_len + 1);
    strncpy(pem_file_buffer, pub_key_buff, buff_len);
    pem_file_buffer[buff_len] = '\0';

    m_private_key = rsa_private_key_ptr;
    m_public_key = rsa_public_key_ptr;

    delete[] pub_key_buff;
  } else {
    if (rsa_private_key_ptr)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
      EVP_PKEY_free(rsa_private_key_ptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
      RSA_free(rsa_private_key_ptr);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    if (rsa_public_key_ptr) {
      delete[] pub_key_buff;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
      EVP_PKEY_free(rsa_public_key_ptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
      RSA_free(rsa_public_key_ptr);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    }
  }
  return false;
}

void optimize_plugin_compare_by_pointer(LEX_CSTRING *plugin_name) {
  Cached_authentication_plugins::optimize_plugin_compare_by_pointer(
      plugin_name);
}

bool auth_plugin_is_built_in(const char *plugin_name) {
  LEX_CSTRING plugin = {STRING_WITH_LEN(plugin_name)};
  return g_cached_authentication_plugins->auth_plugin_is_built_in(&plugin);
}

/**
  Only the plugins that are known to use the mysql.user table
  to store their passwords support password expiration atm.
  TODO: create a service and extend the plugin API to support
  password expiration for external plugins.
  @retval      false  expiration not supported
  @retval      true   expiration supported
*/
bool auth_plugin_supports_expiration(const char *plugin_name) {
  if (!plugin_name || !*plugin_name) return false;

  return auth_plugin_is_built_in(plugin_name);
}

/**
  a helper function to report cannot proxy error in all the proper places
*/
static void cannot_proxy_error(THD *thd, const MPVIO_EXT &mpvio,
                               int server_error, int client_error) {
  my_error(client_error, MYF(0), mpvio.auth_info.user_name,
           mpvio.auth_info.host_or_ip, mpvio.auth_info.authenticated_as);
  query_logger.general_log_print(
      thd, COM_CONNECT, ER_DEFAULT_NONCONST(client_error),
      mpvio.auth_info.user_name, mpvio.auth_info.host_or_ip);
  LogErr(INFORMATION_LEVEL, server_error, mpvio.auth_info.user_name,
         mpvio.auth_info.host_or_ip, mpvio.auth_info.authenticated_as);
}

/**
  a helper function to report an access denied error in all the proper places
*/
static void login_failed_error(THD *thd, MPVIO_EXT *mpvio, int passwd_used) {
  if (thd->is_error()) {
    LogEvent()
        .prio(INFORMATION_LEVEL)
        .errcode(ER_ABORTING_USER_CONNECTION)
        .subsys(LOG_SUBSYSTEM_TAG)
        .verbatim(thd->get_stmt_da()->message_text());
  }

  else if (passwd_used == 2) {
    my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
             mpvio->auth_info.user_name, mpvio->auth_info.host_or_ip);
    query_logger.general_log_print(
        thd, COM_CONNECT, ER_DEFAULT(ER_ACCESS_DENIED_NO_PASSWORD_ERROR),
        mpvio->auth_info.user_name, mpvio->auth_info.host_or_ip);
    /*
      Log access denied messages to the error log when log_error_verbosity = 3
      so that the overhead of the general query log is not required to track
      failed connections.
    */
    LogErr(INFORMATION_LEVEL, ER_ACCESS_DENIED_ERROR_WITHOUT_PASSWORD,
           mpvio->auth_info.user_name, mpvio->auth_info.host_or_ip);
  } else {
    my_error(ER_ACCESS_DENIED_ERROR, MYF(0), mpvio->auth_info.user_name,
             mpvio->auth_info.host_or_ip,
             passwd_used ? ER_THD(thd, ER_YES) : ER_THD(thd, ER_NO));
    query_logger.general_log_print(
        thd, COM_CONNECT, ER_DEFAULT(ER_ACCESS_DENIED_ERROR),
        mpvio->auth_info.user_name, mpvio->auth_info.host_or_ip,
        passwd_used ? ER_DEFAULT(ER_YES) : ER_DEFAULT(ER_NO));
    /*
      Log access denied messages to the error log when log_error_verbosity = 3
      so that the overhead of the general query log is not required to track
      failed connections.
    */
    LogErr(INFORMATION_LEVEL, ER_ACCESS_DENIED_ERROR_WITH_PASSWORD,
           mpvio->auth_info.user_name, mpvio->auth_info.host_or_ip,
           passwd_used ? ER_DEFAULT(ER_YES) : ER_DEFAULT(ER_NO));
  }
}

/* clang-format off */
/**
  @page page_protocol_connection_phase_packets_protocol_handshake_v9 Protocol::HandshakeV9:

  Initial handshake packet for protocol version 9.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>protocol version</td>
    <td>Always 9</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>server version</td>
      <td>human readable status information</td></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>thread id</td>
    <td>a.k.a. connection id</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
    <td>scramble</td>
    <td>Authentication plugin data for @ref page_protocol_connection_phase_authentication_methods_old_password_authentication</td></tr>
  </table>

  @returns @ref sect_protocol_connection_phase_packets_protocol_handshake_response320
*/



/**
  @page page_protocol_connection_phase_packets_protocol_handshake_v10 Protocol::HandshakeV10

  Initial handshake packet for protocol version 10.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>protocol version</td>
    <td>Always 10</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>server version</td>
      <td>human readable status information</td></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>thread id</td>
    <td>a.k.a. connection id</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_fix "string[8]"</td>
    <td>auth-plugin-data-part-1</td>
    <td>first 8 bytes of the plugin provided data (scramble)</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>filler</td>
    <td>0x00 byte, terminating the first part of a scramble</td></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
    <td>capability_flags_1</td>
    <td>The lower 2 bytes of the \ref group_cs_capabilities_flags</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>character_set</td>
    <td>default server \ref a_protocol_character_set, only the lower 8-bits</td></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
    <td>status_flags</td>
    <td>\ref SERVER_STATUS_flags_enum</td></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
    <td>capability_flags_2</td>
    <td>The upper 2 bytes of the \ref group_cs_capabilities_flags</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PLUGIN_AUTH {</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>auth_plugin_data_len</td>
    <td>length of the combined auth_plugin_data (scramble), if auth_plugin_data_len is &gt; 0</td></tr>
  <tr><td colspan="3">} else {</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>00</td>
    <td>constant 0x00</td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_fix "string[10]"</td>
    <td>reserved</td>
    <td>reserved. All 0s.</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "$length"</td>
    <td>auth-plugin-data-part-2</td>
    <td>Rest of the plugin provided data (scramble), $len=MAX(13, length of auth-plugin-data - 8)</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PLUGIN_AUTH {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "NULL"</td>
    <td>auth_plugin_name</td>
    <td>name of the auth_method that the auth_plugin_data belongs to</td></tr>
  <tr><td colspan="3">}</td></tr>
  </table>

  If the client supports SSL (\ref group_cs_capabilities_flags @& ::CLIENT_SSL
  is on and the \ref mysql_ssl_mode of the client is not ::SSL_MODE_DISABLED)
  a short package called
  @ref page_protocol_connection_phase_packets_protocol_ssl_request is sent,
  causing the server to establish an SSL layer and wait for the next package
  from the client.

  Client then returns
  @ref page_protocol_connection_phase_packets_protocol_handshake_response

  At any time, at any error, the client will just disconnect.

  @sa send_server_handshake_packet mysql_real_connect
*/
/* clang-format on */

/**
  Sends a server @ref
  page_protocol_connection_phase_packets_protocol_handshake_v10

  @retval 0 ok
  @retval 1 error
*/
static bool send_server_handshake_packet(MPVIO_EXT *mpvio, const char *data,
                                         uint data_len) {
  assert(mpvio->status == MPVIO_EXT::FAILURE);
  assert(data_len <= 255);
  Protocol_classic *protocol = mpvio->protocol;

  char *buff = (char *)my_alloca(1 + SERVER_VERSION_LENGTH + data_len + 64);
  char scramble_buf[SCRAMBLE_LENGTH];
  char *end = buff;

  DBUG_TRACE;
  *end++ = protocol_version;

  protocol->set_client_capabilities(CLIENT_BASIC_FLAGS);

  if (opt_using_transactions)
    protocol->add_client_capability(CLIENT_TRANSACTIONS);

  protocol->add_client_capability(CAN_CLIENT_COMPRESS);

  bool have_ssl = false;
  if (current_thd->is_admin_connection() && g_admin_ssl_configured == true) {
    Lock_and_access_ssl_acceptor_context context(mysql_admin);
    have_ssl = context.have_ssl();
  } else {
    Lock_and_access_ssl_acceptor_context context(mysql_main);
    have_ssl = context.have_ssl();
  }

  if (have_ssl) {
    protocol->add_client_capability(CLIENT_SSL);
    protocol->add_client_capability(CLIENT_SSL_VERIFY_SERVER_CERT);
  }

  if (opt_protocol_compression_algorithms &&
      opt_protocol_compression_algorithms[0] != 0) {
    /* turn off the capability flag as the global variable might have changed */
    protocol->remove_client_capability(CLIENT_COMPRESS);
    protocol->remove_client_capability(CLIENT_ZSTD_COMPRESSION_ALGORITHM);
    std::vector<std::string> list;
    parse_compression_algorithms_list(opt_protocol_compression_algorithms,
                                      list);
    auto it = list.begin();
    NET_SERVER *ext = static_cast<NET_SERVER *>(protocol->get_net()->extension);
    struct compression_attributes *compression = &(ext->compression);
    compression->compression_optional = false;
    while (it != list.end()) {
      std::string value = *it;
      switch (get_compression_algorithm(value)) {
        case enum_compression_algorithm::MYSQL_ZSTD:
          protocol->add_client_capability(CLIENT_ZSTD_COMPRESSION_ALGORITHM);
          break;
        case enum_compression_algorithm::MYSQL_ZLIB:
          protocol->add_client_capability(CLIENT_COMPRESS);
          break;
        case enum_compression_algorithm::MYSQL_UNCOMPRESSED:
          compression->compression_optional = true;
          break;
        case enum_compression_algorithm::MYSQL_INVALID:
          assert(false);
          break;
      }
      it++;
    }
  }

  if (data_len) {
    mpvio->cached_server_packet.pkt =
        (char *)memdup_root(mpvio->mem_root, data, data_len);
    mpvio->cached_server_packet.pkt_len = data_len;
  }

  if (data_len < SCRAMBLE_LENGTH) {
    if (data_len) {
      /*
        the first packet *must* have at least 20 bytes of a scramble.
        if a plugin provided less, we pad it to 20 with zeros
      */
      memcpy(scramble_buf, data, data_len);
      memset(scramble_buf + data_len, 0, SCRAMBLE_LENGTH - data_len);
      data = scramble_buf;
    }
    data_len = SCRAMBLE_LENGTH;
  }

  end = my_stpnmov(end, server_version, SERVER_VERSION_LENGTH) + 1;

  assert(sizeof(my_thread_id) == 4);
  int4store((uchar *)end, mpvio->thread_id);
  end += 4;

  /*
    Old clients does not understand long scrambles, but can ignore packet
    tail: that's why first part of the scramble is placed here, and second
    part at the end of packet.
  */
  end = (char *)memcpy(end, data, AUTH_PLUGIN_DATA_PART_1_LENGTH);
  end += AUTH_PLUGIN_DATA_PART_1_LENGTH;
  *end++ = 0;

  int2store(end, static_cast<uint16>(protocol->get_client_capabilities()));
  /* write server characteristics: up to 16 bytes allowed */
  end[2] = (char)default_charset_info->number;
  int2store(end + 3, mpvio->server_status[0]);
  int2store(end + 5, protocol->get_client_capabilities() >> 16);
  end[7] = data_len;
  DBUG_EXECUTE_IF("poison_srv_handshake_scramble_len", end[7] = -100;);
  DBUG_EXECUTE_IF("increase_srv_handshake_scramble_len", end[7] = 50;);
  memset(end + 8, 0, 10);
  end += 18;
  /* write scramble tail */
  end = (char *)memcpy(end, data + AUTH_PLUGIN_DATA_PART_1_LENGTH,
                       data_len - AUTH_PLUGIN_DATA_PART_1_LENGTH);
  end += data_len - AUTH_PLUGIN_DATA_PART_1_LENGTH;
  end = strmake(end, client_plugin_name(mpvio->plugin),
                strlen(client_plugin_name(mpvio->plugin)));

  const int res = protocol->write((uchar *)buff, (size_t)(end - buff + 1)) ||
                  protocol->flush();
  return res;
}

/* clang-format off */
/**
  @page page_protocol_connection_phase_packets_protocol_auth_switch_request Protocol::AuthSwitchRequest:

  Authentication method Switch Request Packet

  If both server and the client support @ref CLIENT_PLUGIN_AUTH capability,
  server can send this packet to ask client to use another authentication method.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0xFE (254)</td>
    <td>status tag</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string[NUL]"</td>
    <td>plugin name</td>
    <td>name of the client authentication plugin to switch to</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_eof "string[EOF]"</td>
    <td>plugin provided data</td>
    <td>Initial authentication data for that client plugin</td></tr>
  </table>

  @return @ref page_protocol_connection_phase_packets_protocol_auth_switch_response
  or closing the connection.

  @sa send_plugin_request_packet(), client_mpvio_read_packet()
*/


/**
  @page page_protocol_connection_phase_packets_protocol_old_auth_switch_request Protocol::OldAuthSwitchRequest:

  @warning *Deprecated*. Newer servers should never send this since they don't support
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  and they support @ref CLIENT_PLUGIN_AUTH.
  Newer clients should not support it since they should not support
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication.
  and they should support @ref CLIENT_PLUGIN_AUTH.

  Old Authentication Method Switch Request Packet consisting of
  a single 0xfe byte. It is sent by server to request client to switch to
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  if @ref CLIENT_PLUGIN_AUTH capability flag is not supported (by either the
  client or the server).

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0xFE (254)</td>
    <td>status tag</td></tr>
  </table>

  @return @ref page_protocol_connection_phase_packets_protocol_auth_switch_response
  with an
  @ref page_protocol_connection_phase_authentication_methods_old_password_authentication
  hash or closing the connection.

  @sa client_mpvio_read_packet()
 */

/**
  @page page_protocol_connection_phase_packets_protocol_auth_switch_response Protocol::AuthSwitchResponse:

  Authentication Method Switch Response Packet which contains response data
  generated by the authenticatication method requested in
  @ref page_protocol_connection_phase_packets_protocol_old_auth_switch_request
  packet. This data is opaque to the protocol.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_eof "string&lt;EOF&gt;"</td>
    <td>data</td>
    <td>authentication response data</td></tr>
  </table>

  @return @ref page_protocol_connection_phase_packets_protocol_auth_more_data,
    @ref page_protocol_basic_err_packet or @ref page_protocol_basic_ok_packet

  Example:

  If the client sends a @ref page_caching_sha2_authentication_exchanges and
  the server has a mysql_native_password plugin for that user it will ask the
  client to switch to mysql_native_password and the client will reply from the
  mysql_native_password plugin:

  <table>
  <tr><td>
  ~~~~~~~~~~~~~~~~~~~~~
  14 00 00 03 f4 17 96 1f    79 f3 ac 10 0b da a6 b3
  ~~~~~~~~~~~~~~~~~~~~~
  </td><td>
  ~~~~~~~~~~~~~~~~~~~~~
  ........y.......
  ~~~~~~~~~~~~~~~~~~~~~
  </td></tr>
  <tr><td>
  ~~~~~~~~~~~~~~~~~~~~~
  b5 c2 0e ab 59 85 ff b8
  ~~~~~~~~~~~~~~~~~~~~~
  </td><td>
  ~~~~~~~~~~~~~~~~~~~~~
  ....Y...
  ~~~~~~~~~~~~~~~~~~~~~
  </td></tr>
  </table>

  @sa client_mpvio_write_packet, server_mpvio_read_packet
*/

/**
  @page page_protocol_connection_phase_packets_protocol_auth_more_data Protocol::AuthMoreData:


  We need to make sure that when sending plugin supplied data to the client they
  are not considered a special out-of-band command, like e.g.
  @ref page_protocol_basic_err_packet,
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
  or @ref page_protocol_basic_ok_packet.
  To avoid this the server will send all plugin data packets "wrapped"
  in a command \1.
  Note that the client will continue sending its replies unrwapped:
  @ref page_protocol_connection_phase_packets_protocol_auth_switch_response


  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
  <td>0x01</td>
  <td>status tag</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_eof "string&lt;EOF&gt;"</td>
    <td>authentication method data</td>
    <td>Extra authentication data beyond the initial challenge</td></tr>
  </table>

  @sa wrap_plguin_data_into_proper_command, server_mpvio_write_packet,
  client_mpvio_read_packet
*/

/**
  @page page_protocol_connection_phase_packets_protocol_auth_next_factor_request Protocol::AuthNextFactor:

  Next Authentication method Packet in @ref page_protocol_multi_factor_authentication_methods

  If both server and the client support @ref MULTI_FACTOR_AUTHENTICATION capability,
  server can send this packet to ask client to initiate next authentication method
  in @ref page_protocol_multi_factor_authentication_methods process.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>0x02 </td>
    <td>packet type</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string[NUL]"</td>
    <td>plugin name</td>
    <td>name of the client authentication plugin </td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_eof "string[EOF]"</td>
    <td>plugin provided data</td>
    <td>Initial authentication data for that client plugin</td></tr>
  </table>

  @return @ref page_protocol_connection_phase_packets_protocol_auth_next_factor_request
  or closing the connection.

  @sa send_auth_next_factor_packet(), client_mpvio_read_packet()
  @ref send_auth_next_factor_packet(), client_mpvio_read_packet()
*/

/* clang-format on */

/**
  Sends a @ref
  page_protocol_connection_phase_packets_protocol_auth_switch_request

  Used by the server to request that a client should restart authentication
  using a different authentication plugin.

  See @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
  for more details.

  @retval false ok
  @retval true error
*/
static bool send_plugin_request_packet(MPVIO_EXT *mpvio, const uchar *data,
                                       uint data_len) {
  assert(mpvio->packets_written == 1);
  assert(mpvio->packets_read == 1);
  static uchar switch_plugin_request_buf[] = {254};

  DBUG_TRACE;

  /*
    In case of --skip-grant-tables, mpvio->status might already have set to
    SUCCESS, don't reset it to FAILURE now.
  */
  if (initialized)
    mpvio->status = MPVIO_EXT::FAILURE;  // the status is no longer RESTART

  /* Send the client side authentication plugin name */
  std::string client_auth_plugin(
      ((st_mysql_auth *)(plugin_decl(mpvio->plugin)->info))
          ->client_auth_plugin);

  assert(client_auth_plugin.c_str());

  DBUG_EXECUTE_IF("invalidate_client_auth_plugin", {
    client_auth_plugin.clear();
    client_auth_plugin = std::string("..") + std::string(FN_DIRSEP) +
                         std::string("..") + std::string(FN_DIRSEP) +
                         std::string("fake_plugin_name");
  });
  /*
    If we're dealing with an older client we can't just send a change plugin
    packet to re-initiate the authentication handshake, because the client
    won't understand it. The good thing is that we don't need to : the old
    client expects us to just check the user credentials here, which we can do
    by just reading the cached data that are placed there by
    parse_com_change_user_packet() In this case we just do nothing and behave as
    if normal authentication should continue.
  */
  if (!(mpvio->protocol->has_client_capability(CLIENT_PLUGIN_AUTH))) {
    /* client is too old */
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    return true;
  }

  DBUG_PRINT("info", ("requesting client to use the %s plugin",
                      client_auth_plugin.c_str()));
  return net_write_command(
      mpvio->protocol->get_net(), switch_plugin_request_buf[0],
      pointer_cast<const uchar *>(client_auth_plugin.c_str()),
      client_auth_plugin.size() + 1, pointer_cast<const uchar *>(data),
      data_len);
}

/**
  Sends a @ref
  page_protocol_connection_phase_packets_protocol_auth_next_factor_request

  Used by the server to request that a client should initiate authentication
  for next authentication methods in the plugin chain of user definition.

  See @ref
  page_protocol_connection_phase_packets_protocol_auth_next_factor_request for
  more details.

  @param [in] mpvio      The communications channel
  @param [in] data       Client plugin data
  @param [in] data_len   Length of client plugin data

  @retval false ok
  @retval true error
*/
static bool send_auth_next_factor_packet(MPVIO_EXT *mpvio, const uchar *data,
                                         uint data_len) {
  static uchar auth_next_factor_request_buf[] = {2};
  DBUG_TRACE;

  /* Send the client side authentication plugin name */
  std::string client_auth_plugin(
      ((st_mysql_auth *)(plugin_decl(mpvio->plugin)->info))
          ->client_auth_plugin);
  assert(client_auth_plugin.c_str());
  if (!(mpvio->protocol->has_client_capability(MULTI_FACTOR_AUTHENTICATION)))
    return false;

  DBUG_PRINT("info",
             ("requesting client to initiate %s plugin's authentication",
              client_auth_plugin.c_str()));

  return net_write_command(
      mpvio->protocol->get_net(), auth_next_factor_request_buf[0],
      pointer_cast<const uchar *>(client_auth_plugin.c_str()),
      client_auth_plugin.size() + 1, pointer_cast<const uchar *>(data),
      data_len);
}

/* Return true if there is no users that can match the given host */

bool acl_check_host(THD *thd, const char *host, const char *ip) {
  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false)) return true;

  if (allow_all_hosts) return false;

  if ((host && acl_check_hosts->count(host) != 0) ||
      (ip && acl_check_hosts->count(ip) != 0))
    return false;  // Found host

  for (ACL_HOST_AND_IP *acl = acl_wild_hosts->begin();
       acl != acl_wild_hosts->end(); ++acl) {
    if (acl->compare_hostname(host, ip)) return false;  // Host ok
  }

  if (ip != nullptr) {
    /* Increment HOST_CACHE.COUNT_HOST_ACL_ERRORS. */
    Host_errors errors;
    errors.m_host_acl = 1;
    inc_host_errors(ip, &errors);
  }
  return true;  // Host is not allowed
}

/**
  When authentication is attempted using an unknown username a dummy user
  account with no authentication capabilities is assigned to the connection.
  When server is started with -skip-grant-tables, a dummy user account
  with authentication capabilities is assigned to the connection.
  Dummy user authenticates with the empty authentication string.
  This is done to decrease the cost of enumerating user accounts based on
  authentication protocol.

  @param [in] username       A dummy user to be created.
  @param [in] hostname       Host of the dummy user.
  @param [in] mem            Memory in which the dummy ACL user will be created.
  @param [in] rand           Seed value to generate random data
  @param [in] is_initialized State of ACL caches

  @retval A dummy ACL USER
*/
ACL_USER *decoy_user(const LEX_CSTRING &username, const LEX_CSTRING &hostname,
                     MEM_ROOT *mem, struct rand_struct *rand,
                     bool is_initialized) {
  ACL_USER *user = (ACL_USER *)mem->Alloc(sizeof(ACL_USER));
  user->can_authenticate = !initialized;
  user->user = strdup_root(mem, username.str);
  user->user[username.length] = '\0';
  user->host.update_hostname(strdup_root(mem, hostname.str));
  user->ssl_cipher = empty_c_string;
  user->x509_issuer = empty_c_string;
  user->x509_subject = empty_c_string;
  user->password_last_changed.time_type = MYSQL_TIMESTAMP_ERROR;
  user->password_lifetime = 0;
  user->use_default_password_lifetime = true;
  user->account_locked = false;
  user->use_default_password_reuse_interval = true;
  user->password_reuse_interval = 0;
  user->use_default_password_history = true;
  user->password_history_length = 0;
  user->password_require_current = Lex_acl_attrib_udyn::DEFAULT;
  user->password_locked_state.set_parameters(0, 0);
  user->m_mfa = nullptr;

  if (is_initialized) {
    Auth_id key(user);

    uint value;
    if (unknown_accounts->find(key, value)) {
      user->plugin = Cached_authentication_plugins::cached_plugins_names[value];
    } else {
      const int DECIMAL_SHIFT = 1000;
      const int random_number = static_cast<int>(my_rnd(rand) * DECIMAL_SHIFT);
      uint plugin_num = (uint)(random_number % ((uint)PLUGIN_LAST));
      user->plugin =
          Cached_authentication_plugins::cached_plugins_names[plugin_num];
      unknown_accounts->clear_if_greater(MAX_UNKNOWN_ACCOUNTS);

      /*
        If we fail to insert, someone already did it.
        So try to retrieve it. If we fail (e.g. map was cleared),
        just use the default and move on.
      */
      if (!unknown_accounts->insert(key, plugin_num)) {
        if (!unknown_accounts->find(key, plugin_num))
          user->plugin = default_auth_plugin_name;
        else
          user->plugin =
              Cached_authentication_plugins::cached_plugins_names[plugin_num];
      }
    }
  } else
    user->plugin = default_auth_plugin_name;

  for (int i = 0; i < NUM_CREDENTIALS; ++i) {
    memset(user->credentials[i].m_salt, 0, SCRAMBLE_LENGTH + 1);
    user->credentials[i].m_salt_len = 0;
    user->credentials[i].m_auth_string = EMPTY_CSTR;
  }
  return user;
}

/**
   Finds acl entry in user database for authentication purposes.

   Finds a user and copies it into mpvio. Reports an authentication
   failure if a user is not found.

   @note find_acl_user is not the same, because it doesn't take into
   account the case when user is not empty, but acl_user->user is empty

   @retval 0    found
   @retval 1    not found
*/
static bool find_mpvio_user(THD *thd, MPVIO_EXT *mpvio) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("entry: %s", mpvio->auth_info.user_name));
  assert(mpvio->acl_user == nullptr);

  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false)) return true;

  Acl_user_ptr_list *list = nullptr;
  if (likely(acl_users)) {
    list = cached_acl_users_for_name(mpvio->auth_info.user_name);
  }
  if (list) {
    for (auto it = list->begin(); it != list->end(); ++it) {
      ACL_USER *acl_user_tmp = (*it);

      if ((!acl_user_tmp->user ||
           !strcmp(mpvio->auth_info.user_name, acl_user_tmp->user)) &&
          acl_user_tmp->host.compare_hostname(mpvio->host, mpvio->ip)) {
        mpvio->acl_user = acl_user_tmp->copy(mpvio->mem_root);
        *(mpvio->restrictions) =
            acl_restrictions->find_restrictions(mpvio->acl_user);

        /*
          When setting mpvio->acl_user_plugin we can save memory allocation if
          this is a built in plugin.
        */
        if (auth_plugin_is_built_in(acl_user_tmp->plugin.str))
          mpvio->acl_user_plugin = mpvio->acl_user->plugin;
        else
          lex_string_strmake(mpvio->mem_root, &mpvio->acl_user_plugin,
                             acl_user_tmp->plugin.str,
                             acl_user_tmp->plugin.length);
        break;
      }
    }
  }
  acl_cache_lock.unlock();

  if (!mpvio->acl_user) {
    /*
      Pretend the user exists; let the plugin decide how to handle
      bad credentials.
    */
    const LEX_CSTRING usr = {mpvio->auth_info.user_name,
                             mpvio->auth_info.user_name_length};
    const LEX_CSTRING hst = {
        mpvio->host ? mpvio->host : mpvio->ip,
        mpvio->host ? strlen(mpvio->host) : strlen(mpvio->ip)};
    mpvio->acl_user =
        decoy_user(usr, hst, mpvio->mem_root, mpvio->rand, initialized);
    mpvio->acl_user_plugin = mpvio->acl_user->plugin;
  }

  if (!(mpvio->protocol->has_client_capability(CLIENT_PLUGIN_AUTH))) {
    /* client is too old */
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    query_logger.general_log_print(thd, COM_CONNECT, "%s",
                                   ER_DEFAULT(ER_NOT_SUPPORTED_AUTH_MODE));
    return true;
  }

  mpvio->auth_info.multi_factor_auth_info[0].auth_string =
      mpvio->acl_user->credentials[PRIMARY_CRED].m_auth_string.str;
  mpvio->auth_info.multi_factor_auth_info[0].auth_string_length =
      (unsigned long)mpvio->acl_user->credentials[PRIMARY_CRED]
          .m_auth_string.length;
  if (mpvio->acl_user->credentials[SECOND_CRED].m_auth_string.length) {
    mpvio->auth_info.additional_auth_string =
        mpvio->acl_user->credentials[SECOND_CRED].m_auth_string.str;
    mpvio->auth_info.additional_auth_string_length =
        (unsigned long)mpvio->acl_user->credentials[SECOND_CRED]
            .m_auth_string.length;
  } else {
    mpvio->auth_info.additional_auth_string = nullptr;
    mpvio->auth_info.additional_auth_string_length = 0;
  }
  strmake(mpvio->auth_info.authenticated_as,
          mpvio->acl_user->user ? mpvio->acl_user->user : "", USERNAME_LENGTH);

  /* auth_string references to 1st factor auth plugin credential details */
  mpvio->auth_info.auth_string =
      mpvio->auth_info.multi_factor_auth_info[0].auth_string;
  mpvio->auth_info.auth_string_length =
      mpvio->auth_info.multi_factor_auth_info[0].auth_string_length;

  DBUG_PRINT("info",
             ("exit: user=%s, auth_string=%s, authenticated as=%s"
              ", plugin=%s, authentication factor=%d",
              mpvio->auth_info.user_name, mpvio->auth_info.auth_string,
              mpvio->auth_info.authenticated_as, mpvio->acl_user->plugin.str,
              mpvio->auth_info.current_auth_factor));

  /* Copy 2nd and 3rd factor auth string and registration flag into mpvio */
  if (mpvio->acl_user->m_mfa) {
    Multi_factor_auth_list *auth_factor =
        mpvio->acl_user->m_mfa->get_multi_factor_auth_list();
    uint f = 1;
    for (auto m_it : auth_factor->get_mfa_list()) {
      Multi_factor_auth_info *af = m_it->get_multi_factor_auth_info();
      mpvio->auth_info.multi_factor_auth_info[f].auth_string =
          af->get_auth_str();
      mpvio->auth_info.multi_factor_auth_info[f].auth_string_length =
          af->get_auth_str_len();
      mpvio->auth_info.multi_factor_auth_info[f].is_registration_required =
          af->get_requires_registration();
      DBUG_PRINT("info",
                 ("exit: user=%s, auth_string=%s, plugin=%s, authentication "
                  "factor=%d",
                  mpvio->auth_info.user_name,
                  mpvio->auth_info.multi_factor_auth_info[f].auth_string,
                  af->get_plugin_str(), f));
      f++;
    }
  }

  return false;
}

static bool read_client_connect_attrs(THD *thd, char **ptr,
                                      size_t *max_bytes_available,
                                      MPVIO_EXT *mpvio [[maybe_unused]]) {
  size_t length, length_length;
  char *ptr_save;

  /* not enough bytes to hold the length */
  if (*max_bytes_available < 1) return true;

  /* read the length */
  ptr_save = *ptr;
  length = static_cast<size_t>(net_field_length_ll((uchar **)ptr));
  length_length = *ptr - ptr_save;
  if (*max_bytes_available < length_length) return true;

  *max_bytes_available -= length_length;

  /* length says there're more data than can fit into the packet */
  if (length > *max_bytes_available) return true;

  /* impose an artificial length limit of 64k */
  if (length > 65535) return true;

#ifdef HAVE_PSI_THREAD_INTERFACE
  MYSQL_SERVER_AUTH_INFO *auth_info = &mpvio->auth_info;
  int bytes_lost;
  if ((bytes_lost = PSI_THREAD_CALL(set_thread_connect_attrs)(
           *ptr, length, mpvio->charset_adapter->charset())))
    LogErr(WARNING_LEVEL, ER_CONN_ATTR_TRUNCATED, (unsigned long)length,
           (int)bytes_lost, (unsigned long long)mpvio->thread_id,
           (auth_info->user_name == NULL) ? "" : auth_info->user_name,
           auth_info->host_or_ip, auth_info->authenticated_as,
           mpvio->can_authenticate() ? "yes" : "no");
#endif /* HAVE_PSI_THREAD_INTERFACE */

  // assign the connection attributes in the current thread
  thd->m_connection_attributes = std::vector<char>(length);
  std::copy(*ptr, *ptr + length, thd->m_connection_attributes.begin());

  *max_bytes_available -= length;
  *ptr = *ptr + length;

  return false;
}

static bool acl_check_ssl(THD *thd, const ACL_USER *acl_user) {
  Vio *vio = thd->get_protocol_classic()->get_vio();
  SSL *ssl = (SSL *)vio->ssl_arg;
  X509 *cert;

  /*
    At this point we know that user is allowed to connect
    from given host by given username/password pair. Now
    we check if SSL is required, if user is using SSL and
    if X509 certificate attributes are OK
  */
  switch (acl_user->ssl_type) {
    case SSL_TYPE_NOT_SPECIFIED:  // Impossible
    case SSL_TYPE_NONE:           // SSL is not required
      return false;
    case SSL_TYPE_ANY:  // Any kind of SSL is ok
      return vio_type(vio) != VIO_TYPE_SSL;
    case SSL_TYPE_X509: /* Client should have any valid certificate. */
      /*
        Connections with non-valid certificates are dropped already
        in sslaccept() anyway, so we do not check validity here.

        We need to check for absence of SSL because without SSL
        we should reject connection.
      */
      if (vio_type(vio) == VIO_TYPE_SSL &&
          SSL_get_verify_result(ssl) == X509_V_OK &&
          (cert = SSL_get_peer_certificate(ssl))) {
        X509_free(cert);
        return false;
      }
      return true;
    case SSL_TYPE_SPECIFIED: /* Client should have specified attrib */
      /* If a cipher name is specified, we compare it to actual cipher in use.
       */
      if (vio_type(vio) != VIO_TYPE_SSL ||
          SSL_get_verify_result(ssl) != X509_V_OK)
        return true;
      if (acl_user->ssl_cipher) {
        DBUG_PRINT("info", ("comparing ciphers: '%s' and '%s'",
                            acl_user->ssl_cipher, SSL_get_cipher(ssl)));
        if (strcmp(acl_user->ssl_cipher, SSL_get_cipher(ssl))) {
          LogErr(INFORMATION_LEVEL, ER_X509_CIPHERS_MISMATCH,
                 acl_user->ssl_cipher, SSL_get_cipher(ssl));
          return true;
        }
      }
      /* Prepare certificate (if exists) */
      if (!(cert = SSL_get_peer_certificate(ssl))) return true;
      /* If X509 issuer is specified, we check it... */
      if (acl_user->x509_issuer) {
        char *ptr = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
        DBUG_PRINT("info", ("comparing issuers: '%s' and '%s'",
                            acl_user->x509_issuer, ptr));
        if (strcmp(acl_user->x509_issuer, ptr)) {
          LogErr(INFORMATION_LEVEL, ER_X509_ISSUER_MISMATCH,
                 acl_user->x509_issuer, ptr);
          OPENSSL_free(ptr);
          X509_free(cert);
          return true;
        }
        OPENSSL_free(ptr);
      }
      /* X509 subject is specified, we check it .. */
      if (acl_user->x509_subject) {
        char *ptr = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        DBUG_PRINT("info", ("comparing subjects: '%s' and '%s'",
                            acl_user->x509_subject, ptr));
        if (strcmp(acl_user->x509_subject, ptr)) {
          LogErr(INFORMATION_LEVEL, ER_X509_SUBJECT_MISMATCH,
                 acl_user->x509_subject, ptr);
          OPENSSL_free(ptr);
          X509_free(cert);
          return true;
        }
        OPENSSL_free(ptr);
      }
      X509_free(cert);
      return false;
  }
  return true;
}

/**

  Check if server has valid public key/private key
  pair for RSA communication.

  @retval false RSA support is available
  @retval true RSA support is not available
*/
bool sha256_rsa_auth_status() {
  return (!g_sha256_rsa_keys->get_private_key() ||
          !g_sha256_rsa_keys->get_public_key());
}

/* clang-format off */
/**
  @page page_protocol_com_change_user COM_CHANGE_USER

  @brief Changes the user of the current connection.

  Also and resets the following connection state:
  - user variables
  - temporary tables
  - prepared statements
  - ... and others

  It is going through the same states as the
  @ref sect_protocol_connection_phase_initial_handshake

  @return @ref page_protocol_connection_phase_packets_protocol_auth_switch_request
    or @ref page_protocol_basic_err_packet

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
      <td>command</td>
      <td>0x11: COM_CHANGE_USER</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>user</td>
      <td>user name</td></tr>
  <tr><td colspan="3">if capabilities @& @ref CLIENT_RESERVED2 "CLIENT_SECURE_CONNECTION" {</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
      <td>auth_plugin_data_len</td>
      <td>length of auth_response</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "$length"</td>
      <td>auth_plugin_data</td>
      <td>authentication data</td></tr>
  <tr><td colspan="3">} else {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>auth_plugin_data</td>
      <td>authentication data (9 bytes)</td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>database</td>
      <td>schema name</td></tr>
  <tr><td colspan="3">if more data available {</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PROTOCOL_41 {</td></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
      <td>character_set</td>
      <td>new connection character set. See @ref page_protocol_basic_character_set</td></tr>
  <tr><td colspan="3">} -- ::CLIENT_PROTOCOL_41</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PLUGIN_AUTH {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>auth_plugin_name</td>
      <td>client authentication plugin name used to generate auth_plugin_data</td></tr>
  <tr><td colspan="3">} -- ::CLIENT_PLUGIN_AUTH</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_CONNECT_ATTRS {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_int_le "int&lt;lenenc&gt;"</td>
      <td>connection_attributes_length</td>
      <td>length in bytes of the following block of key-value pairs</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "$length"</td>
      <td>key</td>
      <td>Key name</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "$length"</td>
      <td>value</td>
      <td>value of key</td></tr>
  <tr><td colspan="3">more key/value pairs until connection_attributes_length is depleted</td></tr>
  <tr><td colspan="3">} -- ::CLIENT_CONNECT_ATTRS</td></tr>
  <tr><td colspan="3">} -- more data available</td></tr>
  </table>

  Please also read @ref sect_protocol_connection_phase_com_change_user_auth

  @sa mysql_change_user, send_change_user_packet, parse_com_change_user_packet,
  acl_authenticate, dispatch_command
*/
/* clang-format on */

/**
  @brief Parses a @ref page_protocol_com_change_user

  @param thd            current thread
  @param mpvio          the communications channel
  @param packet_length  length of the packet in mpvio's buffer

  @retval true error
  @retval false success
*/
static bool parse_com_change_user_packet(THD *thd, MPVIO_EXT *mpvio,
                                         size_t packet_length) {
  Protocol_classic *protocol = mpvio->protocol;
  char *user = (char *)protocol->get_net()->read_pos;
  char *end = user + packet_length;
  /* Safe because there is always a trailing \0 at the end of the packet */
  char *passwd = strend(user) + 1;
  size_t user_len = passwd - user - 1;
  char *db = passwd;
  char db_buff[NAME_LEN + 1];           // buffer to store db in utf8
  char user_buff[USERNAME_LENGTH + 1];  // buffer to store user in utf8
  uint dummy_errors;

  DBUG_TRACE;
  if (passwd >= end) {
    my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
    return true;
  }

  /*
    Clients send the size (1 byte) + string (not null-terminated).

    Cast *passwd to an unsigned char, so that it doesn't extend the sign for
    *passwd > 127 and become 2**32-127+ after casting to uint.
  */
  const size_t passwd_len = (uchar)(*passwd++);

  db += passwd_len + 1;
  /*
    Database name is always NUL-terminated, so in case of empty database
    the packet must contain at least the trailing '\0'.
  */
  if (db >= end) {
    my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
    return true;
  }

  size_t db_len = strlen(db);

  char *ptr = db + db_len + 1;

  if (ptr + 1 < end) {
    if (mpvio->charset_adapter->init_client_charset(uint2korr(ptr)))
      return true;
    // skip over the charset's 2 bytes
    ptr += 2;
  }

  /* Convert database and user names to utf8 */
  db_len = copy_and_convert(db_buff, sizeof(db_buff) - 1, system_charset_info,
                            db, db_len, mpvio->charset_adapter->charset(),
                            &dummy_errors);
  db_buff[db_len] = 0;

  user_len = copy_and_convert(user_buff, sizeof(user_buff) - 1,
                              system_charset_info, user, user_len,
                              mpvio->charset_adapter->charset(), &dummy_errors);
  user_buff[user_len] = 0;

  /* we should not free mpvio->user here: it's saved by dispatch_command() */
  if (!(mpvio->auth_info.user_name = my_strndup(
            key_memory_MPVIO_EXT_auth_info, user_buff, user_len, MYF(MY_WME))))
    return true;
  mpvio->auth_info.user_name_length = user_len;

  if (lex_string_strmake(mpvio->mem_root, &mpvio->db, db_buff, db_len))
    return true; /* The error is set by make_lex_string(). */

  if (!initialized) {
    // if mysqld's been started with --skip-grant-tables option
    strmake(mpvio->auth_info.authenticated_as, mpvio->auth_info.user_name,
            USERNAME_LENGTH);

    mpvio->status = MPVIO_EXT::SUCCESS;
    return false;
  }

  if (find_mpvio_user(thd, mpvio)) {
    return true;
  }

  if (!protocol->has_client_capability(CLIENT_PLUGIN_AUTH)) {
    /* client is too old */
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    query_logger.general_log_print(thd, COM_CONNECT, "%s",
                                   ER_DEFAULT(ER_NOT_SUPPORTED_AUTH_MODE));
    return true;
  }

  const char *client_plugin;
  client_plugin = ptr;
  /*
    ptr needs to be updated to point to correct position so that
    connection attributes are read properly.
  */
  ptr = ptr + strlen(client_plugin) + 1;

  if (client_plugin >= end) {
    my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
    return true;
  }

  if (ptr > end) {
    my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
    return true;
  }
  size_t bytes_remaining_in_packet = end - ptr;

  if (protocol->has_client_capability(CLIENT_CONNECT_ATTRS) &&
      read_client_connect_attrs(thd, &ptr, &bytes_remaining_in_packet, mpvio))
    return true;

  DBUG_PRINT("info", ("client_plugin=%s, restart", client_plugin));
  /*
    Remember the data part of the packet, to present it to plugin in
    read_packet()
  */
  mpvio->cached_client_reply.pkt = passwd;
  mpvio->cached_client_reply.pkt_len = passwd_len;
  mpvio->cached_client_reply.plugin = client_plugin;
  mpvio->status = MPVIO_EXT::RESTART;

  return false;
}

/** Get a string according to the protocol of the underlying buffer. */
typedef char *(*get_proto_string_func_t)(char **, size_t *, size_t *);

/**
  Get a string formatted according to the 4.1 version of the MySQL protocol.

  @param [in, out] buffer    Pointer to the user-supplied buffer to be scanned.
  @param [in, out] max_bytes_available  Limit the bytes to scan.
  @param [out] string_length The number of characters scanned not including
                            the null character.

  @note Strings are always null character terminated in this version of the
          protocol.

  @note The string_length does not include the terminating null character.
          However, after the call, the buffer is increased by string_length+1
          bytes, beyond the null character if there still available bytes to
          scan.

  @return pointer to beginning of the string scanned.
    @retval NULL The buffer content is malformed
*/

static char *get_41_protocol_string(char **buffer, size_t *max_bytes_available,
                                    size_t *string_length) {
  char *str = (char *)memchr(*buffer, '\0', *max_bytes_available);

  if (str == nullptr) return nullptr;

  *string_length = (size_t)(str - *buffer);
  *max_bytes_available -= *string_length + 1;
  str = *buffer;
  *buffer += *string_length + 1;

  return str;
}

/**
  Get a string formatted according to the 4.0 version of the MySQL protocol.

  @param [in, out] buffer    Pointer to the user-supplied buffer to be scanned.
  @param [in, out] max_bytes_available  Limit the bytes to scan.
  @param [out] string_length The number of characters scanned not including
                            the null character.

  @note If there are not enough bytes left after the current position of
          the buffer to satisfy the current string, the string is considered
          to be empty and a pointer to empty_c_string is returned.

  @note A string at the end of the packet is not null terminated.

  @return Pointer to beginning of the string scanned, or a pointer to a empty
          string.
*/
static char *get_40_protocol_string(char **buffer, size_t *max_bytes_available,
                                    size_t *string_length) {
  char *str;
  size_t len;

  /* No bytes to scan left, treat string as empty. */
  if ((*max_bytes_available) == 0) {
    *string_length = 0;
    return empty_c_string;
  }

  str = (char *)memchr(*buffer, '\0', *max_bytes_available);

  /*
    If the string was not null terminated by the client,
    the remainder of the packet is the string. Otherwise,
    advance the buffer past the end of the null terminated
    string.
  */
  if (str == nullptr)
    len = *string_length = *max_bytes_available;
  else
    len = (*string_length = (size_t)(str - *buffer)) + 1;

  str = *buffer;
  *buffer += len;
  *max_bytes_available -= len;

  return str;
}

/**
  Get a length encoded string from a user-supplied buffer.

  @param [in, out] buffer The buffer to scan; updates position after scan.
  @param [in, out] max_bytes_available Limit the number of bytes to scan
  @param [out] string_length Number of characters scanned

  @note In case the length is zero, then the total size of the string is
    considered to be 1 byte; the size byte.

  @return pointer to first byte after the header in buffer.
    @retval NULL The buffer content is malformed
*/

static char *get_56_lenc_string(char **buffer, size_t *max_bytes_available,
                                size_t *string_length) {
  static char empty_string[1] = {'\0'};
  char *begin = *buffer;
  uchar *pos = (uchar *)begin;

  if (*max_bytes_available == 0) return nullptr;

  /*
    If the length encoded string has the length 0
    the total size of the string is only one byte long (the size byte)
  */
  if (*begin == 0) {
    *string_length = 0;
    --*max_bytes_available;
    ++*buffer;
    /*
      Return a pointer to the \0 character so the return value will be
      an empty string.
    */
    return empty_string;
  }

  /* Make sure we have enough bytes available for net_field_length_ll */

  DBUG_EXECUTE_IF("buffer_too_short_3", *pos = 252; *max_bytes_available = 2;);
  DBUG_EXECUTE_IF("buffer_too_short_4", *pos = 253; *max_bytes_available = 3;);
  DBUG_EXECUTE_IF("buffer_too_short_9", *pos = 254; *max_bytes_available = 8;);

  const size_t required_length = (size_t)net_field_length_size(pos);

  if (*max_bytes_available < required_length) return nullptr;

  *string_length = (size_t)net_field_length_ll((uchar **)buffer);

  DBUG_EXECUTE_IF("sha256_password_scramble_too_long",
                  *string_length = SIZE_T_MAX;);

  const size_t len_len = (size_t)(*buffer - begin);

  assert((*max_bytes_available >= len_len) && (len_len == required_length));

  if (*string_length > *max_bytes_available - len_len) return nullptr;

  *max_bytes_available -= *string_length;
  *max_bytes_available -= len_len;
  *buffer += *string_length;
  return (char *)(begin + len_len);
}

/**
  Get a length encoded string from a user-supplied buffer.

  @param [in, out] buffer The buffer to scan; updates position after scan.
  @param [in, out] max_bytes_available Limit the number of bytes to scan
  @param [out] string_length Number of characters scanned

  @note In case the length is zero, then the total size of the string is
    considered to be 1 byte; the size byte.

  @note the maximum size of the string is 255 because the header is always
    1 byte.
  @return pointer to first byte after the header in buffer.
    @retval NULL The buffer content is malformed
*/

static char *get_41_lenc_string(char **buffer, size_t *max_bytes_available,
                                size_t *string_length) {
  if (*max_bytes_available == 0) return nullptr;

  /* Do double cast to prevent overflow from signed / unsigned conversion */
  const size_t str_len = (size_t)(unsigned char)**buffer;

  /*
    If the length encoded string has the length 0
    the total size of the string is only one byte long (the size byte)
  */
  if (str_len == 0) {
    ++*buffer;
    *string_length = 0;
    /*
      Return a pointer to the 0 character so the return value will be
      an empty string.
    */
    return *buffer - 1;
  }

  if (str_len >= *max_bytes_available) return nullptr;

  char *str = *buffer + 1;
  *string_length = str_len;
  *max_bytes_available -= *string_length + 1;
  *buffer += *string_length + 1;
  return str;
}

/* the packet format is described in send_client_reply_packet() */
static size_t parse_client_handshake_packet(THD *thd, MPVIO_EXT *mpvio,
                                            uchar **buff, size_t pkt_len) {
  Protocol_classic *protocol = mpvio->protocol;
  char *end;
  bool packet_has_required_size = false;
  /* save server capabilities before setting client capabilities */
  const bool is_server_supports_zlib =
      protocol->has_client_capability(CLIENT_COMPRESS);
  const bool is_server_supports_zstd =
      protocol->has_client_capability(CLIENT_ZSTD_COMPRESSION_ALGORITHM);
  assert(mpvio->status == MPVIO_EXT::FAILURE);

  uint charset_code = 0;

  NET *net = protocol->get_net();
  end = (char *)net->read_pos;
  /*
    In order to safely scan a head for '\0' string terminators
    we must keep track of how many bytes remain in the allocated
    buffer or we might read past the end of the buffer.
  */
  size_t bytes_remaining_in_packet = pkt_len;

  /*
    Peek ahead on the client capability packet and determine which version of
    the protocol should be used.
  */
  if (bytes_remaining_in_packet < 2) return packet_error;

  protocol->set_client_capabilities(uint2korr(end));

  bool is_client_proto_41 = protocol->has_client_capability(CLIENT_PROTOCOL_41);

  /*
    JConnector only sends server capabilities before starting SSL
    negotiation.  The below code is patch for this.
  */
  if (bytes_remaining_in_packet == 4 &&
      protocol->has_client_capability(CLIENT_SSL)) {
    protocol->set_client_capabilities(uint4korr(end));
    mpvio->max_client_packet_length = 0xfffff;
    charset_code = global_system_variables.character_set_client->number;
    goto skip_to_ssl;
  }

  if (is_client_proto_41)
    packet_has_required_size =
        bytes_remaining_in_packet >= AUTH_PACKET_HEADER_SIZE_PROTO_41;
  else
    packet_has_required_size =
        bytes_remaining_in_packet >= AUTH_PACKET_HEADER_SIZE_PROTO_40;

  if (!packet_has_required_size) return packet_error;

  if (is_client_proto_41) {
    protocol->set_client_capabilities(uint4korr(end));
    mpvio->max_client_packet_length = uint4korr(end + 4);
    charset_code = (uint)(uchar) * (end + 8);
    /*
      Skip 23 remaining filler bytes which have no particular meaning.
    */
    end += AUTH_PACKET_HEADER_SIZE_PROTO_41;
    bytes_remaining_in_packet -= AUTH_PACKET_HEADER_SIZE_PROTO_41;
  } else {
    protocol->set_client_capabilities(uint2korr(end));
    mpvio->max_client_packet_length = uint3korr(end + 2);
    end += AUTH_PACKET_HEADER_SIZE_PROTO_40;
    bytes_remaining_in_packet -= AUTH_PACKET_HEADER_SIZE_PROTO_40;
    /**
      Old clients didn't have their own charset. Instead the assumption
      was that they used what ever the server used.
    */
    charset_code = global_system_variables.character_set_client->number;
  }

skip_to_ssl:
  DBUG_PRINT("info",
             ("client capabilities: %lu", protocol->get_client_capabilities()));

  is_client_proto_41 = protocol->has_client_capability(CLIENT_PROTOCOL_41);

  /*
    If client requested SSL then we must stop parsing, try to switch to SSL,
    and wait for the client to send a new handshake packet.
    The client isn't expected to send any more bytes until SSL is initialized.
  */
  if (protocol->has_client_capability(CLIENT_SSL)) {
    unsigned long errptr;
    uint ssl_charset_code = 0;

    /*
      We need to make sure that reference count for
      SSL context is kept till the end of function
    */
    const bool admin_ctx = thd->is_admin_connection() && g_admin_ssl_configured;
    Lock_and_access_ssl_acceptor_context context(admin_ctx ? mysql_admin
                                                           : mysql_main);

    /* Do the SSL layering. */
    if (!context.have_ssl()) return packet_error;
    DBUG_PRINT("info", ("IO layer change in progress..."));
    if (sslaccept(context, protocol->get_vio(), net->read_timeout, &errptr)) {
      DBUG_PRINT("error", ("Failed to accept new SSL connection"));
      return packet_error;
    }

    DBUG_PRINT("info", ("Reading user information over SSL layer"));
    const int rc = protocol->read_packet();
    pkt_len = protocol->get_packet_length();
    if (rc) {
      DBUG_PRINT("error", ("Failed to read user information (pkt_len= %lu)",
                           static_cast<ulong>(pkt_len)));
      return packet_error;
    }
    /* mark vio as encrypted */
    mpvio->vio_is_encrypted = 1;

    /*
      A new packet was read and the statistics reflecting the remaining bytes
      in the packet must be updated.
    */
    bytes_remaining_in_packet = pkt_len;

    /*
      After the SSL handshake is performed the client resends the handshake
      packet but because of legacy reasons we chose not to parse the packet
      fields a second time and instead only assert the length of the packet.
    */
    if (is_client_proto_41) {
      packet_has_required_size =
          bytes_remaining_in_packet >= AUTH_PACKET_HEADER_SIZE_PROTO_41;
      ssl_charset_code = (uint)(uchar) * ((char *)net->read_pos + 8);
      DBUG_PRINT("info", ("client_character_set: %u", ssl_charset_code));
      end = (char *)net->read_pos + AUTH_PACKET_HEADER_SIZE_PROTO_41;
      bytes_remaining_in_packet -= AUTH_PACKET_HEADER_SIZE_PROTO_41;
    } else {
      packet_has_required_size =
          bytes_remaining_in_packet >= AUTH_PACKET_HEADER_SIZE_PROTO_40;
      end = (char *)net->read_pos + AUTH_PACKET_HEADER_SIZE_PROTO_40;
      bytes_remaining_in_packet -= AUTH_PACKET_HEADER_SIZE_PROTO_40;
      /**
        Old clients didn't have their own charset. Instead the assumption
        was that they used what ever the server used.
      */
      ssl_charset_code = global_system_variables.character_set_client->number;
    }

    if (charset_code != ssl_charset_code || !packet_has_required_size)
      return packet_error;
  }

  DBUG_PRINT("info", ("client_character_set: %u", charset_code));
  if (mpvio->charset_adapter->init_client_charset(charset_code))
    return packet_error;

  if ((protocol->has_client_capability(CLIENT_TRANSACTIONS)) &&
      opt_using_transactions)
    net->return_status = mpvio->server_status;

  /*
    The 4.0 and 4.1 versions of the protocol differ on how strings
    are terminated. In the 4.0 version, if a string is at the end
    of the packet, the string is not null terminated. Do not assume
    that the returned string is always null terminated.
  */
  const get_proto_string_func_t get_string =
      is_client_proto_41 ? get_41_protocol_string : get_40_protocol_string;

  /*
    When the ability to change default plugin require that the initial password
   field can be of arbitrary size. However, the 41 client-server protocol limits
   the length of the auth-data-field sent from client to server to 255 bytes
   (CLIENT_SECURE_CONNECTION). The solution is to change the type of the field
   to a true length encoded string and indicate the protocol change with a new
   client capability flag: CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA.
  */
  get_proto_string_func_t get_length_encoded_string;

  if (protocol->has_client_capability(CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA))
    get_length_encoded_string = get_56_lenc_string;
  else
    get_length_encoded_string = get_41_lenc_string;

  /*
    In order to safely scan a head for '\0' string terminators
    we must keep track of how many bytes remain in the allocated
    buffer or we might read past the end of the buffer.
  */
  bytes_remaining_in_packet = pkt_len - (end - (char *)net->read_pos);

  size_t user_len;
  char *user = get_string(&end, &bytes_remaining_in_packet, &user_len);
  if (user == nullptr) return packet_error;

  /*
    Old clients send a null-terminated string as password; new clients send
    the size (1 byte) + string (not null-terminated). Hence in case of empty
    password both send '\0'.
  */
  size_t passwd_len = 0;
  char *passwd = nullptr;

  passwd =
      get_length_encoded_string(&end, &bytes_remaining_in_packet, &passwd_len);
  if (passwd == nullptr) return packet_error;

  size_t db_len = 0;
  char *db = nullptr;

  if (protocol->has_client_capability(CLIENT_CONNECT_WITH_DB)) {
    db = get_string(&end, &bytes_remaining_in_packet, &db_len);
    if (db == nullptr) return packet_error;
  }

  size_t client_plugin_len = 0;
  const char *client_plugin =
      get_string(&end, &bytes_remaining_in_packet, &client_plugin_len);
  if (client_plugin == nullptr) client_plugin = &empty_c_string[0];

  char db_buff[NAME_LEN + 1];           // buffer to store db in utf8
  char user_buff[USERNAME_LENGTH + 1];  // buffer to store user in utf8
  uint dummy_errors;

  /*
    Copy and convert the user and database names to the character set used
    by the server. Since 4.1 all database names are stored in UTF-8. Also,
    ensure that the names are properly null-terminated as this is relied
    upon later.
  */
  if (db) {
    db_len = copy_and_convert(db_buff, sizeof(db_buff) - 1, system_charset_info,
                              db, db_len, mpvio->charset_adapter->charset(),
                              &dummy_errors);
    db_buff[db_len] = '\0';
    db = db_buff;
  }

  user_len = copy_and_convert(user_buff, sizeof(user_buff) - 1,
                              system_charset_info, user, user_len,
                              mpvio->charset_adapter->charset(), &dummy_errors);
  user_buff[user_len] = '\0';
  user = user_buff;

  /* If username starts and ends in "'", chop them off */
  if (user_len > 1 && user[0] == '\'' && user[user_len - 1] == '\'') {
    user[user_len - 1] = 0;
    user++;
    user_len -= 2;
  }

  if (lex_string_strmake(mpvio->mem_root, &mpvio->db, db, db_len))
    return packet_error; /* The error is set by make_lex_string(). */
  if (mpvio->auth_info.user_name) my_free(mpvio->auth_info.user_name);
  if (!(mpvio->auth_info.user_name = my_strndup(key_memory_MPVIO_EXT_auth_info,
                                                user, user_len, MYF(MY_WME))))
    return packet_error; /* The error is set by my_strdup(). */
  mpvio->auth_info.user_name_length = user_len;

  if (find_mpvio_user(thd, mpvio)) return packet_error;

  if (!initialized) {
    // if mysqld's been started with --skip-grant-tables option
    mpvio->status = MPVIO_EXT::SUCCESS;
  }

  if (protocol->has_client_capability(CLIENT_CONNECT_ATTRS) &&
      read_client_connect_attrs(thd, &end, &bytes_remaining_in_packet, mpvio))
    return packet_error;

  auto *ext = static_cast<NET_SERVER *>(net->extension);
  struct compression_attributes *compression = &(ext->compression);
  const bool is_client_supports_zlib =
      protocol->has_client_capability(CLIENT_COMPRESS);
  const bool is_client_supports_zstd =
      protocol->has_client_capability(CLIENT_ZSTD_COMPRESSION_ALGORITHM);

  if (is_client_supports_zlib && is_server_supports_zlib) {
    strcpy(compression->compress_algorithm, COMPRESSION_ALGORITHM_ZLIB);
    /*
      for zlib compression method client does not send any compression level,
      set default compression level
    */
    compression->compress_level = 6;
  } else if (is_client_supports_zstd && is_server_supports_zstd) {
    strcpy(compression->compress_algorithm, COMPRESSION_ALGORITHM_ZSTD);
    compression->compress_level = (uint) * (end);
    end += 1;
    bytes_remaining_in_packet -= 1;
    if (!is_zstd_compression_level_valid(compression->compress_level)) {
      my_error(ER_WRONG_COMPRESSION_LEVEL_CLIENT, MYF(0),
               compression->compress_algorithm);
      mpvio->status = MPVIO_EXT::FAILURE;
      return CR_COMPRESSION_WRONGLY_CONFIGURED;
    }
  } else if (!compression->compression_optional) {
    /*
      if server is configured to only allow connections with compression
      enabled then check if client has enabled compression else report error
    */
    my_error(
        ER_WRONG_COMPRESSION_ALGORITHM_CLIENT, MYF(0),
        (compression->compress_algorithm[0] ? compression->compress_algorithm
                                            : "uncompressed"));

    mpvio->status = MPVIO_EXT::FAILURE;
    return CR_COMPRESSION_WRONGLY_CONFIGURED;
  }

  if (!(protocol->has_client_capability(CLIENT_PLUGIN_AUTH))) {
    /* client is too old */
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    query_logger.general_log_print(thd, COM_CONNECT, "%s",
                                   ER_DEFAULT(ER_NOT_SUPPORTED_AUTH_MODE));
    return packet_error;
  }

  /*
    If the acl_user needs a different plugin to authenticate then
    the server default plugin we need to restart the authentication
    in the server.
    But perhaps the client has already used the correct plugin -
    in that case the authentication on the client may not need to be
    restarted and a server auth plugin will read the data that the client
    has just send. Cache them to return in the next server_mpvio_read_packet().
  */
  if (my_strcasecmp(system_charset_info, mpvio->acl_user_plugin.str,
                    plugin_name(mpvio->plugin)->str) != 0) {
    /* Server default plugin didn't match user plugin */
    mpvio->cached_client_reply.pkt = passwd;
    mpvio->cached_client_reply.pkt_len = passwd_len;
    mpvio->cached_client_reply.plugin = client_plugin;
    mpvio->status = MPVIO_EXT::RESTART;
    return packet_error;
  }

  /*
    ok, we don't need to restart the authentication on the server.
    but if the client used the wrong plugin, we need to restart
    the authentication on the client. Do it here, the server plugin
    doesn't need to know.
  */
  plugin_ref user_plugin =
      g_cached_authentication_plugins->get_cached_plugin_ref(
          &mpvio->acl_user_plugin);
  if (user_plugin == nullptr) return packet_error;
  auto user_client_plugin_name = client_plugin_name(user_plugin);
  if (my_strcasecmp(system_charset_info, client_plugin,
                    user_client_plugin_name)) {
    /*
      Client plugins don't match - send request to client to use a
      different plugin and restart authentication process.
    */
    mpvio->cached_client_reply.plugin = client_plugin;
    /*
      Inject error here for testing purpose.
      See auth_sec.server_send_client_plugin
    */
    DBUG_EXECUTE_IF("assert_authentication_roundtrips",
                    { return packet_error; });

    if (send_plugin_request_packet(mpvio,
                                   (uchar *)mpvio->cached_server_packet.pkt,
                                   mpvio->cached_server_packet.pkt_len))
      return packet_error;

    mpvio->protocol->read_packet();
    passwd_len = protocol->get_packet_length();
    passwd = (char *)net->read_pos;
  }

  *buff = (uchar *)passwd;
  return passwd_len;
}

/**
  Wrap the extra auth data sent so that they can pass in the protocol.

  Check @ref page_protocol_connection_phase_packets_protocol_auth_more_data
  for the format description.

  @retval 0 ok
  @retval 1 error

  @param net         the network abstraction to use
  @param packet      data to transmit
  @param packet_len  length of packet

  @sa net_write_command, client_mpvio_write_packet

*/

static inline int wrap_plguin_data_into_proper_command(NET *net,
                                                       const uchar *packet,
                                                       int packet_len) {
  return net_write_command(net, 1, pointer_cast<const uchar *>(""), 0, packet,
                           packet_len);
}

/*
  Note: The following functions are declared inside extern "C" because
  they are used to initialize C structure MPVIO (see
  server_mpvio_initialize()).
*/

extern "C" {

/**
  vio->write_packet() callback method for server authentication plugins

  This function is called by a server authentication plugin, when it wants
  to send data to the client.

  It transparently wraps the data into a handshake packet,
  and handles plugin negotiation with the client. If necessary,
  it escapes the plugin data, if it starts with a mysql protocol packet byte.
*/
static int server_mpvio_write_packet(MYSQL_PLUGIN_VIO *param,
                                     const uchar *packet, int packet_len) {
  MPVIO_EXT *mpvio = (MPVIO_EXT *)param;
  int res;
  Protocol_classic *protocol = mpvio->protocol;

  DBUG_TRACE;

  // sometimes this function is invoked early enough that m_client_capabilities
  // has value of 0, so even for new client & server the function
  // has_client_capability() returns 0 !
  // for example that happens when user authenticates:
  // a. caching_sha2_password_authenticate() + vio->write_packet() call stack
  // b. caching_sha2_password_authenticate() + vio->read_packet() +
  //      parse_client_handshake_packet() call stack

  /*
    Reset cached_client_reply if not an old client doing mysql_change_user,
    as this is where the password from COM_CHANGE_USER is stored.
  */
  if (!((!(protocol->has_client_capability(CLIENT_PLUGIN_AUTH))) &&
        mpvio->status == MPVIO_EXT::RESTART &&
        mpvio->cached_client_reply.plugin ==
            ((st_mysql_auth *)(plugin_decl(mpvio->plugin)->info))
                ->client_auth_plugin))
    mpvio->cached_client_reply.pkt = nullptr;

  /* for the 1st packet we wrap plugin data into the handshake packet */
  if (mpvio->packets_written == 0)
    res = send_server_handshake_packet(
        mpvio, pointer_cast<const char *>(packet), packet_len);
  else if (mpvio->status == MPVIO_EXT::RESTART) {
    /*
      Inject error here for testing purpose.
      See auth_sec.server_send_client_plugin
    */
    DBUG_EXECUTE_IF("assert_authentication_roundtrips", {
      return -1;  // Crash here.
    });
    res = send_plugin_request_packet(mpvio, packet, packet_len);
  } else if (mpvio->status == MPVIO_EXT::START_MFA) {
    res = send_auth_next_factor_packet(mpvio, packet, packet_len);
    /*
      reset the status to avoid sending AuthNextFactor again for the
      same factor authentication.
    */
    mpvio->status = MPVIO_EXT::FAILURE;
  } else
    res = wrap_plguin_data_into_proper_command(protocol->get_net(), packet,
                                               packet_len);
  mpvio->packets_written++;
  return res;
}

/**
  vio->read_packet() callback method for server authentication plugins

  This function is called by a server authentication plugin, when it wants
  to read data from the client.

  It transparently extracts the client plugin data, if embedded into
  a client authentication handshake packet, and handles plugin negotiation
  with the client, if necessary.

  RETURN
    -1          Protocol failure
    >= 0        Success and also the packet length
*/
static int server_mpvio_read_packet(MYSQL_PLUGIN_VIO *param, uchar **buf) {
  MPVIO_EXT *mpvio = (MPVIO_EXT *)param;
  Protocol_classic *protocol = mpvio->protocol;
  size_t pkt_len;

  DBUG_TRACE;
  if (mpvio->packets_written == 0) {
    /*
      plugin wants to read the data without sending anything first.
      send an empty packet to force a server handshake packet to be sent
    */
    if (mpvio->write_packet(mpvio, nullptr, 0))
      pkt_len = packet_error;
    else {
      protocol->read_packet();
      pkt_len = protocol->get_packet_length();
    }
  } else if (mpvio->cached_client_reply.pkt) {
    assert(mpvio->status == MPVIO_EXT::RESTART);
    assert(mpvio->packets_read > 0);
    /*
      If the data cached from the last server_mpvio_read_packet
      and a client has used the correct plugin, then we can return the
      cached data straight away and avoid one round trip.
    */

    auto client_auth_plugin_name = client_plugin_name(mpvio->plugin);
    if (client_auth_plugin_name == nullptr ||
        my_strcasecmp(system_charset_info, mpvio->cached_client_reply.plugin,
                      client_auth_plugin_name) == 0) {
      mpvio->status = MPVIO_EXT::FAILURE;
      *buf = const_cast<uchar *>(
          pointer_cast<const uchar *>(mpvio->cached_client_reply.pkt));
      mpvio->cached_client_reply.pkt = nullptr;
      mpvio->packets_read++;
      return (int)mpvio->cached_client_reply.pkt_len;
    }

    /* older clients don't support change of client plugin request */
    if (!(protocol->has_client_capability(CLIENT_PLUGIN_AUTH))) {
      mpvio->status = MPVIO_EXT::FAILURE;
      pkt_len = packet_error;
      goto err;
    }
    /*
      But if the client has used the wrong plugin, the cached data are
      useless. Furthermore, we have to send a "change plugin" request
      to the client.
    */
    if (mpvio->write_packet(mpvio, nullptr, 0))
      pkt_len = packet_error;
    else {
      protocol->read_packet();
      pkt_len = protocol->get_packet_length();
    }
  } else if (mpvio->status == MPVIO_EXT::START_MFA) {
    /* Send AuthNextFactor packet to client and change mpvio status */
    if (mpvio->write_packet(mpvio, nullptr, 0)) {
      pkt_len = packet_error;
    } else {
      protocol->read_packet();
      pkt_len = protocol->get_packet_length();
    }
  } else {
    protocol->read_packet();
    pkt_len = protocol->get_packet_length();
  }

  DBUG_EXECUTE_IF("simulate_packet_error", pkt_len = packet_error;);
  if (pkt_len == packet_error) [[unlikely]]
    goto err;

  mpvio->packets_read++;

  /*
    the 1st packet has the plugin data wrapped into the client authentication
    handshake packet
  */
  if (mpvio->packets_read == 1) {
    pkt_len = parse_client_handshake_packet(current_thd, mpvio, buf, pkt_len);
    if (pkt_len == packet_error) [[unlikely]]
      goto err;
  } else
    *buf = protocol->get_net()->read_pos;

  return (int)pkt_len;

err:
  if (mpvio->status == MPVIO_EXT::FAILURE) {
    my_error(ER_HANDSHAKE_ERROR, MYF(0));
  }
  return -1;
}

/**
  fills MYSQL_PLUGIN_VIO_INFO structure with the information about the
  connection
*/
static void server_mpvio_info(MYSQL_PLUGIN_VIO *vio,
                              MYSQL_PLUGIN_VIO_INFO *info) {
  MPVIO_EXT *mpvio = (MPVIO_EXT *)vio;
  mpvio_info(mpvio->protocol->get_net()->vio, info);
}

}  // extern "C"

static int do_auth_once(THD *thd, const LEX_CSTRING &auth_plugin_name,
                        MPVIO_EXT *mpvio) {
  DBUG_TRACE;
  int res = CR_OK, old_status = MPVIO_EXT::FAILURE;
  bool unlock_plugin = false;
  plugin_ref plugin =
      g_cached_authentication_plugins->get_cached_plugin_ref(&auth_plugin_name);

  if (!plugin) {
    if ((plugin = my_plugin_lock_by_name(thd, auth_plugin_name,
                                         MYSQL_AUTHENTICATION_PLUGIN)))
      unlock_plugin = true;
  }

  mpvio->plugin = plugin;
  old_status = mpvio->status;

  if (plugin) {
    st_mysql_auth *auth = (st_mysql_auth *)plugin_decl(plugin)->info;
    res = auth->authenticate_user(mpvio, &mpvio->auth_info);

    if (unlock_plugin) plugin_unlock(thd, plugin);
  } else {
    /* Server cannot load the required plugin. */
    Host_errors errors;
    errors.m_no_auth_plugin = 1;
    inc_host_errors(mpvio->ip, &errors);
    my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), auth_plugin_name.str);
    res = CR_ERROR;
  }

  /*
    If the status was MPVIO_EXT::RESTART before the authenticate_user() call
    it can never be MPVIO_EXT::RESTART after the call, because any call
    to write_packet() or read_packet() will reset the status.

    But (!) if a plugin never called a read_packet() or write_packet(), the
    status will stay unchanged. We'll fix it, by resetting the status here.
  */
  if (old_status == MPVIO_EXT::RESTART && mpvio->status == MPVIO_EXT::RESTART)
    mpvio->status = MPVIO_EXT::FAILURE;  // reset to the default

  return res;
}

/* clang-format off */
/**
  @page page_protocol_multi_factor_authentication_methods Multi Factor Authentication

  Assume the client wants to log in via user account U and that user account is defined
  with multiple authentication methods namely X,Y,Z. Assume default authentication method
  on both server and client is X.

  @section sect_protocol_multi_factor_authentication_phase Authentication

  A successful authentication path looks as follows:

  1. The client connects to the server
  2. The server sends @ref page_protocol_connection_phase_packets_protocol_handshake
  3. The client responds with
     @ref page_protocol_connection_phase_packets_protocol_handshake_response
  4. X authentication method packets are exchanged
  5. The server responds with an @ref page_protocol_connection_phase_packets_protocol_auth_next_factor_request
     containing client side plugin name and plugin data of plugin Y.
  6. Client reads plugin name and plugin data from AuthNextFactor packet and loads corresponding client side plugin.
  7. Y authentication method packets are exchanged
  8. The server responds with an @ref page_protocol_connection_phase_packets_protocol_auth_next_factor_request
     containing client side plugin name and plugin data of plugin Z.
  9. Client reads plugin name and plugin data from AuthNextFactor packet and loads corresponding client side plugin.
  10.Z authentication method packets are exchanged
  11.The server responds with an @ref page_protocol_basic_ok_packet

  @note At any point, if the Nth Authentication Method fails, the server can return ERR and disconnect.
    And the client can just disconnect.

  @startuml
  Client -> Server: Connect
  Server -> Client: Initial Handshake Packet
  Client -> Server: Handshake Response Packet

  == X authentication method packets are exchanged ==

  Server -> Client: AuthNextFactor packet containing plugin name/data of plugin Y

  == Y authentication method packets are exchanged ==

  Server -> Client: AuthNextFactor packet containing plugin name/data of plugin Z

  == Z authentication method packets are exchanged ==

  Server -> Client: OK packet

  == Client and server enter Command Phase ==
  @enduml
*/
/* clang-format on */

/**
  Perform 2nd and 3rd factor authentication.

  Once 1FA method succeeds, server checks if connecting user requires more
  authentication methods to do the authentication.

  Refer to @ref page_protocol_multi_factor_authentication_methods
  for server-client communication in various cases

  @param thd            thread handle
  @param mpvio          the communications channel

  @retval 0  success
  @retval 1  error
*/
static int do_multi_factor_auth(THD *thd, MPVIO_EXT *mpvio) {
  DBUG_TRACE;
  int res = CR_OK;
  /* user is not configured with Multi factor authentication */
  if (!mpvio->acl_user->m_mfa) return res;
  /*
    If an old client connects to server with user account created with Multi
    factor authentication methods, then return error.
  */
  if (!mpvio->protocol->has_client_capability(MULTI_FACTOR_AUTHENTICATION))
    return CR_AUTH_USER_CREDENTIALS;

  Multi_factor_auth_list *auth_factor =
      mpvio->acl_user->m_mfa->get_multi_factor_auth_list();
  for (auto m_it : auth_factor->get_mfa_list()) {
    Multi_factor_auth_info *af = m_it->get_multi_factor_auth_info();
    if (af->get_factor() == nthfactor::SECOND_FACTOR)
      mpvio->auth_info.current_auth_factor = 1;
    else if (af->get_factor() == nthfactor::THIRD_FACTOR)
      mpvio->auth_info.current_auth_factor = 2;
    /* reset cached_client_reply for 2nd and 3rd factors */
    mpvio->cached_client_reply.pkt = nullptr;
    plugin_ref plugin = my_plugin_lock_by_name(thd, af->plugin_name(),
                                               MYSQL_AUTHENTICATION_PLUGIN);
    if (plugin) {
      mpvio->plugin = plugin;
      /*
        Update auth_string, to refer to corresponding factors auth plugin
        credentials
      */
      mpvio->auth_info.auth_string =
          mpvio->auth_info
              .multi_factor_auth_info[mpvio->auth_info.current_auth_factor]
              .auth_string;
      mpvio->auth_info.auth_string_length =
          mpvio->auth_info
              .multi_factor_auth_info[mpvio->auth_info.current_auth_factor]
              .auth_string_length;
      mpvio->status = MPVIO_EXT::START_MFA;
      st_mysql_auth *auth = (st_mysql_auth *)plugin_decl(plugin)->info;
      res = auth->authenticate_user(mpvio, &mpvio->auth_info);
      if (res == CR_OK_AUTH_IN_SANDBOX_MODE) {
        /*
          Server allows user account to connect in case registration is
          required, and set server in sandbox mode.
        */
        if (af->get_requires_registration())
          thd->security_context()->set_registration_sandbox_mode(true);
        assert(af->get_requires_registration());
        plugin_unlock(thd, plugin);
        return CR_OK;
      }

      plugin_unlock(thd, plugin);
      if (res != CR_OK) {
        mpvio->status = MPVIO_EXT::FAILURE;
        break;
      } else {
        mpvio->status = MPVIO_EXT::SUCCESS;
      }
    } else {
      /* Server cannot load the required plugin. */
      Host_errors errors;
      errors.m_no_auth_plugin = 1;
      inc_host_errors(mpvio->ip, &errors);
      my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), af->get_plugin_str());
      res = CR_ERROR;
      break;
    }
  }
  return res;
}

static void server_mpvio_initialize(THD *thd, MPVIO_EXT *mpvio,
                                    Thd_charset_adapter *charset_adapter) {
  const LEX_CSTRING sctx_host_or_ip = thd->security_context()->host_or_ip();

  memset(mpvio, 0, sizeof(MPVIO_EXT));
  mpvio->read_packet = server_mpvio_read_packet;
  mpvio->write_packet = server_mpvio_write_packet;
  mpvio->info = server_mpvio_info;
  mpvio->auth_info.user_name = nullptr;
  mpvio->auth_info.user_name_length = 0;
  mpvio->auth_info.host_or_ip = sctx_host_or_ip.str;
  mpvio->auth_info.host_or_ip_length = sctx_host_or_ip.length;
  mpvio->auth_info.password_used = PASSWORD_USED_NO;
  mpvio->auth_info.current_auth_factor = 0;

  Vio *vio = thd->get_protocol_classic()->get_vio();
  if (vio->ssl_arg)
    mpvio->vio_is_encrypted = 1;
  else
    mpvio->vio_is_encrypted = 0;
  mpvio->status = MPVIO_EXT::FAILURE;
  mpvio->mem_root = thd->mem_root;
  mpvio->scramble = thd->scramble;
  mpvio->rand = &thd->rand;
  mpvio->thread_id = thd->thread_id();
  mpvio->server_status = &thd->server_status;
  mpvio->protocol = thd->get_protocol_classic();
  mpvio->ip = thd->security_context()->ip().str;
  mpvio->host = thd->security_context()->host().str;
  mpvio->charset_adapter = charset_adapter;
  mpvio->restrictions = new (mpvio->mem_root) Restrictions();

  mpvio->auth_info.multi_factor_auth_info =
      new (mpvio->mem_root) auth_factor_desc[MAX_AUTH_FACTORS];
  memset(mpvio->auth_info.multi_factor_auth_info, 0, sizeof(auth_factor_desc));
}

static void server_mpvio_update_thd(THD *thd, MPVIO_EXT *mpvio) {
  thd->max_client_packet_length = mpvio->max_client_packet_length;
  if (mpvio->protocol->has_client_capability(CLIENT_INTERACTIVE))
    thd->variables.net_wait_timeout = thd->variables.net_interactive_timeout;
  thd->security_context()->assign_user(
      mpvio->auth_info.user_name,
      (mpvio->auth_info.user_name ? strlen(mpvio->auth_info.user_name) : 0));
  if (mpvio->acl_user) {
    thd->security_context()->lock_account(mpvio->acl_user->account_locked);
  }
  if (mpvio->auth_info.user_name) my_free(mpvio->auth_info.user_name);
  const LEX_CSTRING sctx_user = thd->security_context()->user();
  mpvio->auth_info.user_name = const_cast<char *>(sctx_user.str);
  mpvio->auth_info.user_name_length = sctx_user.length;
  if (thd->get_protocol()->has_client_capability(CLIENT_IGNORE_SPACE))
    thd->variables.sql_mode |= MODE_IGNORE_SPACE;
}

/**
  Calculate the timestamp difference for password expiry

  @param thd			 thread handle
  @param acl_user		 ACL_USER handle

  @retval 0  password is valid
  @retval 1  password has expired
*/
static bool check_password_lifetime(THD *thd, const ACL_USER *acl_user) {
  bool password_time_expired = false;

  if (likely(acl_user != nullptr) && !acl_user->password_expired &&
      acl_user->password_last_changed.time_type != MYSQL_TIMESTAMP_ERROR &&
      auth_plugin_is_built_in(acl_user->plugin.str) &&
      (acl_user->use_default_password_lifetime ||
       acl_user->password_lifetime)) {
    MYSQL_TIME cur_time, password_change_by;
    Interval interval;

    thd->set_time();
    thd->variables.time_zone->gmt_sec_to_TIME(
        &cur_time, static_cast<my_time_t>(thd->query_start_in_secs()));
    password_change_by = acl_user->password_last_changed;
    memset(&interval, 0, sizeof(interval));

    if (!acl_user->use_default_password_lifetime)
      interval.day = acl_user->password_lifetime;
    else {
      MUTEX_LOCK(lock, &LOCK_default_password_lifetime);
      interval.day = default_password_lifetime;
    }
    if (interval.day) {
      if (!date_add_interval_with_warn(thd, &password_change_by, INTERVAL_DAY,
                                       interval))
        password_time_expired =
            my_time_compare(password_change_by, cur_time) >= 0 ? false : true;
      else {
        assert(false);
        /* Make the compiler happy. */
      }
    }
  }
  DBUG_EXECUTE_IF("force_password_interval_expire", {
    if (!acl_user->use_default_password_lifetime && acl_user->password_lifetime)
      password_time_expired = true;
  });
  DBUG_EXECUTE_IF("force_password_interval_expire_for_time_type", {
    if (acl_user->password_last_changed.time_type != MYSQL_TIMESTAMP_ERROR)
      password_time_expired = true;
  });
  return password_time_expired;
}

/**
Logging connection for the general query log, extracted from
acl_authenticate() as it's reused at different times based on
whether proxy users are checked.

@param user                    authentication user name
@param host                    authentication user host or IP address
@param auth_as                 privilege user name
@param db                      default database
@param thd                     thread handle
@param command                 type of command(connect or change user)
*/
void acl_log_connect(const char *user, const char *host, const char *auth_as,
                     const char *db, THD *thd,
                     enum enum_server_command command) {
  const char *vio_name_str = nullptr;
  int len = 0;
  get_vio_type_name(thd->get_vio_type(), &vio_name_str, &len);

  if (strcmp(auth_as, user) && (PROXY_FLAG != *auth_as)) {
    query_logger.general_log_print(thd, command, "%s@%s as %s on %s using %s",
                                   user, host, auth_as, db ? db : "",
                                   vio_name_str);
  } else {
    query_logger.general_log_print(thd, command, "%s@%s on %s using %s", user,
                                   host, db ? db : "", vio_name_str);
  }
}

/*
  Assign priv_user and priv_host fields of the Security_context.

  @param sctx Security context, which priv_user and priv_host fields are
              updated.
  @param user Authenticated user data.
*/
inline void assign_priv_user_host(Security_context *sctx, ACL_USER *user) {
  sctx->assign_priv_user(user->user, user->user ? strlen(user->user) : 0);
  sctx->assign_priv_host(user->host.get_host(), user->host.get_host_len());
}

/**
  Check that for command COM_CONNECT, either restriction on max number of
  concurrent connections  not violated or in case the connection is admin
  connection the user has required privilege.

  @param thd  Thread context

  @return Error status
    @retval false  success
    @retval true   error

  @note if connection is admin connection and a user doesn't have
  the privilege SERVICE_CONNECTION_ADMIN, the error
  ER_SPECIFIC_ACCESS_DENIED_ERROR is set in Diagnostics_area.

  @note if a user doesn't have any of the privileges SUPER_ACL,
  CONNECTION_ADMIN, SERVICE_CONNECTION_ADMIN and a number of concurrent
  connections exceeds the limit max_connections the error ER_CON_COUNT_ERROR
  is set in Diagnostics_area.
*/
static inline bool check_restrictions_for_com_connect_command(THD *thd) {
  if (thd->is_admin_connection() &&
      !thd->m_main_security_ctx
           .has_global_grant(STRING_WITH_LEN("SERVICE_CONNECTION_ADMIN"))
           .first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SERVICE_CONNECTION_ADMIN");
    return true;
  }

  if (!(thd->m_main_security_ctx.check_access(SUPER_ACL) ||
        thd->m_main_security_ctx
            .has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN"))
            .first ||
        thd->m_main_security_ctx
            .has_global_grant(STRING_WITH_LEN("SERVICE_CONNECTION_ADMIN"))
            .first)) {
    if (!Connection_handler_manager::get_instance()
             ->valid_connection_count()) {  // too many connections
      my_error(ER_CON_COUNT_ERROR, MYF(0));
      return true;
    }
  }

  return false;
}

static void check_and_update_password_lock_state(MPVIO_EXT &mpvio, THD *thd,
                                                 int &res) {
  if (mpvio.acl_user && initialized &&
      mpvio.acl_user->password_locked_state.is_active()) {
    /* update user lock status and check if the account is locked */
    Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);

    acl_cache_lock.lock();
    const ACL_USER *acl_user = mpvio.acl_user;

    ACL_USER *acl_user_ptr = find_acl_user(
        acl_user->host.get_host(), acl_user->user ? acl_user->user : "", true);
    long days_remaining = 0;
    assert(acl_user_ptr != nullptr);
    if (acl_user_ptr && acl_user_ptr->password_locked_state.update(
                            thd, res == CR_OK, &days_remaining)) {
      const uint failed_logins =
          acl_user_ptr->password_locked_state.get_failed_login_attempts();
      const int blocked_for_days =
          acl_user_ptr->password_locked_state.get_password_lock_time_days();
      acl_cache_lock.unlock();
      char str_blocked_for_days[30], str_days_remaining[30];
      if (blocked_for_days > 0)
        snprintf(str_blocked_for_days, sizeof(str_blocked_for_days), "%d",
                 blocked_for_days);
      else
        strncpy(str_blocked_for_days, "unlimited",
                sizeof(str_blocked_for_days));
      if (days_remaining > 0)
        snprintf(str_days_remaining, sizeof(str_days_remaining), "%ld",
                 days_remaining);
      else
        strncpy(str_days_remaining, "unlimited", sizeof(str_days_remaining));

      my_error(ER_USER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK,
               MYF(0), mpvio.acl_user->user ? mpvio.acl_user->user : "",
               mpvio.auth_info.host_or_ip ? mpvio.auth_info.host_or_ip : "",
               str_blocked_for_days, str_days_remaining, failed_logins);
      LogErr(INFORMATION_LEVEL,
             ER_ACCESS_DENIED_FOR_USER_ACCOUNT_BLOCKED_BY_PASSWORD_LOCK,
             mpvio.acl_user->user ? mpvio.acl_user->user : "",
             mpvio.auth_info.host_or_ip ? mpvio.auth_info.host_or_ip : "",
             str_blocked_for_days, str_days_remaining, failed_logins);
      res = CR_ERROR;
    } else
      acl_cache_lock.unlock();
  }
}

/**
  Generate ER_SERVER_OFFLINE_MODE with several possible variants of the error
  text, depending on whether OFFLINE_MODE system variable has a "reason"
  attribute attached and if SET_TIME and SET_USER parameters for a variable
  change are present.
*/
void send_server_offline_mode_error() {
  ulonglong timestamp_usec = 0;

  // safe sysvar data access
  const System_variable_tracker var_tracker =
      System_variable_tracker::make_tracker({}, "offline_mode");
  auto f = [&](const System_variable_tracker &, sys_var *var) -> int {
    timestamp_usec = var->get_timestamp();
    return 0;
  };
  int ret = var_tracker
                .access_system_variable<int>(current_thd, f,
                                             Suppress_not_found_error::NO)
                .value_or(-1);
  if (ret == -1) return;

  // format timestamp to string, format identical to SET_TIME from
  // performance_schema.variables_info
  my_timeval tm{};
  my_micro_time_to_timeval(timestamp_usec, &tm);
  char set_time[100] = "";
  MYSQL_TIME mt{};
  current_thd->variables.time_zone->gmt_sec_to_TIME(&mt, tm);
  current_thd->time_zone_used = true;
  my_datetime_to_str(mt, set_time, 6);

  // If an ATTR_VALUE is found, the following error is raised:
  // "The server is currently in offline mode since $SET_TIME, reason:
  // $ATTR_VALUE"
  std::string value;
  get_global_variable_attribute(nullptr, "offline_mode", "reason", value);
  if (!value.empty()) {
    my_error(ER_SERVER_OFFLINE_MODE_REASON, MYF(0), set_time, value.c_str());
    return;
  }

  // else, fallback to legacy message without any context info:
  // "The server is currently in offline mode."
  my_error(ER_SERVER_OFFLINE_MODE, MYF(0));
}

/**
  Perform the handshake, authorize the client and update thd sctx variables.

  @param thd                     thread handle
  @param command                 the command to be executed, it can be either a
                                 COM_CHANGE_USER or COM_CONNECT (if
                                 it's a new connection)

  @retval 0  success, thd is updated.
  @retval 1  error
*/
int acl_authenticate(THD *thd, enum_server_command command) {
  int res = CR_OK;
  int ret = 1;
  MPVIO_EXT mpvio;
  LEX_CSTRING auth_plugin_name = default_auth_plugin_name;
  Thd_charset_adapter charset_adapter(thd);

  DBUG_TRACE;
  static_assert(MYSQL_USERNAME_LENGTH == USERNAME_LENGTH, "");
  assert(command == COM_CONNECT || command == COM_CHANGE_USER);

  DBUG_EXECUTE_IF("acl_authenticate_begin", {
    const char act[] =
        "now SIGNAL conn2_in_acl_auth WAIT_FOR conn1_reached_kill";
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  server_mpvio_initialize(thd, &mpvio, &charset_adapter);
  /*
    Clear thd->db as it points to something, that will be freed when
    connection is closed. We don't want to accidentally free a wrong
    pointer if connect failed.
  */
  thd->reset_db(NULL_CSTR);

  /* acl_authenticate() takes the data from net->read_pos */
  thd->get_protocol_classic()->get_net()->read_pos =
      thd->get_protocol_classic()->get_raw_packet();
  DBUG_PRINT("info", ("com_change_user_pkt_len=%lu",
                      mpvio.protocol->get_packet_length()));

  if (command == COM_CHANGE_USER) {
    mpvio.packets_written++;  // pretend that a server handshake packet was sent
    mpvio.packets_read++;     // take COM_CHANGE_USER packet into account

    /* Clear variables that are allocated */
    thd->set_user_connect(nullptr);

    if (parse_com_change_user_packet(thd, &mpvio,
                                     mpvio.protocol->get_packet_length())) {
      login_failed_error(thd, &mpvio, mpvio.auth_info.password_used);
      server_mpvio_update_thd(thd, &mpvio);
      goto end;
    }

    assert(mpvio.status == MPVIO_EXT::RESTART ||
           mpvio.status == MPVIO_EXT::SUCCESS);
  } else {
    /* mark the thd as having no scramble yet */
    mpvio.scramble[SCRAMBLE_LENGTH] = 1;

    /*
     perform the first authentication attempt, with the default plugin.
     This sends the server handshake packet, reads the client reply
     with a user name, and performs the authentication if everyone has used
     the correct plugin.
    */

    res = do_auth_once(thd, auth_plugin_name, &mpvio);
  }

  /*
   retry the authentication, if - after receiving the user name -
   we found that we need to switch to a non-default plugin
  */
  if (mpvio.status == MPVIO_EXT::RESTART) {
    assert(mpvio.acl_user);
    assert(command == COM_CHANGE_USER ||
           my_strcasecmp(system_charset_info, auth_plugin_name.str,
                         mpvio.acl_user->plugin.str));
    auth_plugin_name = mpvio.acl_user->plugin;
    res = do_auth_once(thd, auth_plugin_name, &mpvio);
  }

  if (res == CR_OK) {
    res = do_multi_factor_auth(thd, &mpvio);
  }

  server_mpvio_update_thd(thd, &mpvio);

  check_and_update_password_lock_state(mpvio, thd, res);
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_connection_type)(thd->get_vio_type());
#endif /* HAVE_PSI_THREAD_INTERFACE */

  {
    Security_context *sctx = thd->security_context();
    const ACL_USER *acl_user = mpvio.acl_user;
    const bool proxy_check =
        check_proxy_users && !*mpvio.auth_info.authenticated_as;

    DBUG_PRINT("info", ("proxy_check=%s", proxy_check ? "true" : "false"));

    thd->password =
        mpvio.auth_info.password_used;  // remember for error messages

    // reset authenticated_as because flag value received, but server
    // proxy mapping is disabled:
    if ((!check_proxy_users) && acl_user &&
        !*mpvio.auth_info.authenticated_as) {
      DBUG_PRINT("info",
                 ("setting authenticated_as to %s as check_proxy_user is OFF.",
                  mpvio.auth_info.user_name));
      strcpy(mpvio.auth_info.authenticated_as,
             acl_user->user ? acl_user->user : "");
    }
    /*
      Log the command here so that the user can check the log
      for the tried logins and also to detect break-in attempts.

      if sctx->user is unset it's protocol failure, bad packet.
    */
    if (mpvio.auth_info.user_name && !proxy_check) {
      acl_log_connect(mpvio.auth_info.user_name, mpvio.auth_info.host_or_ip,
                      mpvio.auth_info.authenticated_as, mpvio.db.str, thd,
                      command);
    }
    if (res == CR_OK && (!mpvio.can_authenticate() || thd->is_error())) {
      res = CR_ERROR;
    }

    /*
      Assign account user/host data to the current THD. This information is
      used when the authentication fails after this point and we call audit
      api notification event. Client user/host connects to the existing
      account is easily distinguished from other connects.
    */
    if (mpvio.can_authenticate())
      assign_priv_user_host(sctx, const_cast<ACL_USER *>(acl_user));

    if (res > CR_OK && mpvio.status != MPVIO_EXT::SUCCESS) {
      Host_errors errors;
      assert(mpvio.status == MPVIO_EXT::FAILURE);
      switch (res) {
        case CR_AUTH_PLUGIN_ERROR:
          errors.m_auth_plugin = 1;
          break;
        case CR_AUTH_HANDSHAKE:
          errors.m_handshake = 1;
          break;
        case CR_AUTH_USER_CREDENTIALS:
          errors.m_authentication = 1;
          break;
        case CR_ERROR:
        default:
          /* Unknown of unspecified auth plugin error. */
          errors.m_auth_plugin = 1;
          break;
      }
      inc_host_errors(mpvio.ip, &errors);
      if (mpvio.auth_info.user_name && proxy_check) {
        acl_log_connect(mpvio.auth_info.user_name, mpvio.auth_info.host_or_ip,
                        mpvio.auth_info.authenticated_as, mpvio.db.str, thd,
                        command);
      }
      login_failed_error(thd, &mpvio, mpvio.auth_info.password_used);
      goto end;
    }

    sctx->assign_proxy_user("", 0);

    if (initialized)  // if not --skip-grant-tables
    {
      bool is_proxy_user = false;
      bool password_time_expired = false;
      const char *auth_user = acl_user->user ? acl_user->user : "";
      ACL_PROXY_USER *proxy_user;
      /* check if the user is allowed to proxy as another user */
      Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);
      if (!acl_cache_lock.lock()) return 1;

      proxy_user =
          acl_find_proxy_user(auth_user, sctx->host().str, sctx->ip().str,
                              mpvio.auth_info.authenticated_as, &is_proxy_user);
      acl_cache_lock.unlock();
      if (mpvio.auth_info.user_name && proxy_check) {
        acl_log_connect(mpvio.auth_info.user_name, mpvio.auth_info.host_or_ip,
                        mpvio.auth_info.authenticated_as, mpvio.db.str, thd,
                        command);
      }

      if (thd->is_error()) return 1;

      if (is_proxy_user) {
        ACL_USER *acl_proxy_user;
        char proxy_user_buf[USERNAME_LENGTH + HOSTNAME_LENGTH + 6];

        /* we need to find the proxy user, but there was none */
        if (!proxy_user) {
          Host_errors errors;
          errors.m_proxy_user = 1;
          inc_host_errors(mpvio.ip, &errors);
          cannot_proxy_error(thd, mpvio,
                             ER_ACCESS_DENIED_NO_PROXY_GRANT_WITH_NAME,
                             ER_ACCESS_DENIED_NO_PROXY_GRANT);
          goto end;
        }

        snprintf(proxy_user_buf, sizeof(proxy_user_buf) - 1, "'%s'@'%s'",
                 auth_user,
                 acl_user->host.get_host() ? acl_user->host.get_host() : "");
        sctx->assign_proxy_user(proxy_user_buf, strlen(proxy_user_buf));

        /* we're proxying : find the proxy user definition */
        if (!acl_cache_lock.lock()) return 1;
        acl_proxy_user = find_acl_user(proxy_user->get_proxied_host()
                                           ? proxy_user->get_proxied_host()
                                           : "",
                                       mpvio.auth_info.authenticated_as, true);
        if (!acl_proxy_user) {
          Host_errors errors;
          errors.m_proxy_user_acl = 1;
          inc_host_errors(mpvio.ip, &errors);
          cannot_proxy_error(thd, mpvio, ER_ACCESS_DENIED_NO_PROXY_WITH_NAME,
                             ER_ACCESS_DENIED_NO_PROXY);
          goto end;
        }
        acl_user = acl_proxy_user->copy(thd->mem_root);
        *(mpvio.restrictions) = acl_restrictions->find_restrictions(acl_user);

        DBUG_PRINT("info", ("User %s is a PROXY and will assume a PROXIED"
                            " identity %s",
                            auth_user, acl_user->user));
        acl_cache_lock.unlock();
      }
      assert(mpvio.restrictions);
      sctx->set_master_access(acl_user->access, *(mpvio.restrictions));
      assign_priv_user_host(sctx, const_cast<ACL_USER *>(acl_user));
      /* Assign default role */
      {
        List_of_auth_id_refs default_roles;
        if (!acl_cache_lock.lock()) return 1;
        Auth_id_ref authid = create_authid_from(acl_user);
        if (opt_always_activate_granted_roles) {
          activate_all_granted_and_mandatory_roles(acl_user, sctx);
        } else {
          /* The server policy is to only activate default roles */
          get_default_roles(authid, default_roles);
          List_of_auth_id_refs::iterator it = default_roles.begin();
          for (; it != default_roles.end(); ++it) {
            if (sctx->activate_role(it->first, it->second, true)) {
              std::string roleidstr = create_authid_str_from(*it);
              std::string authidstr = create_authid_str_from(acl_user);
              LogErr(WARNING_LEVEL, ER_AUTH_CANT_ACTIVATE_ROLE,
                     roleidstr.c_str(), authidstr.c_str());
            }
          }
        }

        acl_cache_lock.unlock();
      }
      sctx->checkout_access_maps();

      if (!thd->is_error() &&
          !(sctx->check_access(SUPER_ACL) ||
            sctx->has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN"))
                .first)) {
        if (mysqld_offline_mode()) {
          send_server_offline_mode_error();
          goto end;
        }
      }

      /*
        OK. Let's check the SSL. Historically it was checked after the
        password, as an additional layer, not instead of the password (in
        which case it would've been a plugin too).
      */
      if (acl_check_ssl(thd, acl_user)) {
        Host_errors errors;
        errors.m_ssl = 1;
        inc_host_errors(mpvio.ip, &errors);
        login_failed_error(thd, &mpvio, thd->password);
        goto end;
      }

      /*
        Check whether the account has been locked.
      */
      if (unlikely(mpvio.acl_user->account_locked)) {
        locked_account_connection_count++;

        my_error(ER_ACCOUNT_HAS_BEEN_LOCKED, MYF(0), mpvio.acl_user->user,
                 mpvio.auth_info.host_or_ip);
        LogErr(INFORMATION_LEVEL, ER_ACCESS_DENIED_FOR_USER_ACCOUNT_LOCKED,
               mpvio.acl_user->user, mpvio.auth_info.host_or_ip);
        goto end;
      }

      DBUG_EXECUTE_IF("before_secure_transport_check", {
        const char act[] = "now SIGNAL kill_now WAIT_FOR killed";
        assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
      });

      /*
        The assumption here is that thd->active_vio and thd->net.vio are both
        the same at this point. We should not use thd->active_vio at any cost,
        as a KILL command can shutdown the active_vio i.e., making it a nullptr
        which would cause issues. Instead we check the net.vio type.
      */
      if (opt_require_secure_transport && thd->get_net()->vio != nullptr &&
          !is_secure_transport(thd->get_net()->vio->type)) {
        my_error(ER_SECURE_TRANSPORT_REQUIRED, MYF(0));
        goto end;
      }

      /* checking password_time_expire for connecting user */
      password_time_expired = check_password_lifetime(thd, mpvio.acl_user);

      if (unlikely(
              mpvio.acl_user &&
              (mpvio.acl_user->password_expired || password_time_expired) &&
              !(mpvio.protocol->has_client_capability(
                  CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS)) &&
              disconnect_on_expired_password)) {
        /*
          Clients that don't signal password expiration support
          get a connect error.
        */
        Host_errors errors;

        my_error(ER_MUST_CHANGE_PASSWORD_LOGIN, MYF(0));
        query_logger.general_log_print(
            thd, COM_CONNECT, "%s", ER_DEFAULT(ER_MUST_CHANGE_PASSWORD_LOGIN));
        LogErr(INFORMATION_LEVEL, ER_ACCOUNT_WITH_EXPIRED_PASSWORD,
               mpvio.acl_user->user, mpvio.auth_info.host_or_ip);

        errors.m_authentication = 1;
        inc_host_errors(mpvio.ip, &errors);
        goto end;
      }

      /* Don't allow the user to connect if he has done too many queries */
      if ((acl_user->user_resource.questions ||
           acl_user->user_resource.updates ||
           acl_user->user_resource.conn_per_hour ||
           acl_user->user_resource.user_conn ||
           global_system_variables.max_user_connections) &&
          get_or_create_user_conn(thd, sctx->priv_user().str,
                                  sctx->priv_host().str,
                                  &acl_user->user_resource))
        goto end;  // The error is set by get_or_create_user_conn()

      /*
        We are copying the connected user's password expired flag to the
        security context. This allows proxy user to execute queries even if
        proxied user password expires.
      */
      sctx->set_password_expired(mpvio.acl_user->password_expired ||
                                 password_time_expired);
    } else {
      sctx->skip_grants();
      /*
        In case of --skip-grant-tables, we already would have set the MPVIO
        as SUCCESS, it means we are not interested in any of the error set
        in the diagnostic area, clear them.
      */
      thd->get_stmt_da()->reset_diagnostics_area();
    }

    const USER_CONN *uc;
    if ((uc = thd->get_user_connect()) &&
        (uc->user_resources.conn_per_hour || uc->user_resources.user_conn ||
         global_system_variables.max_user_connections) &&
        check_for_max_user_connections(thd, uc)) {
      goto end;  // The error is set in check_for_max_user_connections()
    }

    DBUG_PRINT("info", ("Capabilities: %lu  packet_length: %ld  Host: '%s'  "
                        "Login user: '%s' Priv_user: '%s'  Using password: %s "
                        "Access: %" PRIu32 "  db: '%s'",
                        thd->get_protocol()->get_client_capabilities(),
                        thd->max_client_packet_length, sctx->host_or_ip().str,
                        sctx->user().str, sctx->priv_user().str,
                        thd->password ? "yes" : "no", sctx->master_access(),
                        mpvio.db.str));

    if (command == COM_CONNECT &&
        check_restrictions_for_com_connect_command(thd)) {
      release_user_connection(thd);
      goto end;
    }

    /*
      This is the default access rights for the current database.  It's
      set to 0 here because we don't have an active database yet (and we
      may not have an active database to set.
    */
    sctx->cache_current_db_access(0);

    /* Change a database if necessary */
    if (mpvio.db.length) {
      if (mysql_change_db(thd, to_lex_cstring(mpvio.db), false)) {
        /* mysql_change_db() has pushed the error message. */
        release_user_connection(thd);
        Host_errors errors;
        errors.m_default_database = 1;
        inc_host_errors(mpvio.ip, &errors);
        login_failed_error(thd, &mpvio, mpvio.auth_info.password_used);
        goto end;
      }
    }

    if (mpvio.auth_info.external_user[0])
      sctx->assign_external_user(mpvio.auth_info.external_user,
                                 strlen(mpvio.auth_info.external_user));

    if (res == CR_OK_HANDSHAKE_COMPLETE)
      thd->get_stmt_da()->disable_status();
    else
      my_ok(thd);
#ifdef HAVE_PSI_THREAD_INTERFACE
    LEX_CSTRING main_sctx_user = thd->m_main_security_ctx.user();
    LEX_CSTRING main_sctx_host_or_ip = thd->m_main_security_ctx.host_or_ip();
    PSI_THREAD_CALL(set_thread_account)
    (main_sctx_user.str, main_sctx_user.length, main_sctx_host_or_ip.str,
     main_sctx_host_or_ip.length);
#endif /* HAVE_PSI_THREAD_INTERFACE */

    /*
      Turn ON the flag in THD iff the user is granted SYSTEM_USER privilege.
      We must set the flag after all required roles are activated.
    */
    set_system_user_flag(thd);
    // Update the flag in THD based on if the user is granted CONNECTION_ADMIN
    // privilege
    set_connection_admin_flag(thd);
  }
  ret = 0;
end:
  if (mpvio.restrictions) mpvio.restrictions->~Restrictions();
  /* Ready to handle queries */
  return ret;
}

bool is_secure_transport(int vio_type) {
  switch (vio_type) {
    case VIO_TYPE_SSL:
    case VIO_TYPE_SHARED_MEMORY:
    case VIO_TYPE_SOCKET:
      return true;
  }
  return false;
}

static int generate_sha256_password(char *outbuf, unsigned int *buflen,
                                    const char *inbuf, unsigned int inbuflen) {
  /*
   Deprecate message for SHA-256 authentication plugin.
  */
  LogPluginErr(
      WARNING_LEVEL, ER_SERVER_WARN_DEPRECATED,
      Cached_authentication_plugins::get_plugin_name(PLUGIN_SHA256_PASSWORD),
      Cached_authentication_plugins::get_plugin_name(
          PLUGIN_CACHING_SHA2_PASSWORD));
  if (inbuflen > SHA256_PASSWORD_MAX_PASSWORD_LENGTH) return 1;

  THD *thd = current_thd;
  if (!thd->m_disable_password_validation) {
    if (my_validate_password_policy(inbuf, inbuflen)) return 1;
  }
  if (inbuflen == 0) {
    *buflen = 0;
    return 0;
  }
  char *buffer = (char *)my_malloc(PSI_NOT_INSTRUMENTED,
                                   CRYPT_MAX_PASSWORD_SIZE + 1, MYF(0));
  if (buffer == nullptr) return 1;
  my_make_scrambled_password(buffer, inbuf, inbuflen);
  memcpy(outbuf, buffer, CRYPT_MAX_PASSWORD_SIZE);
  /*
    if buffer specified by server is smaller than the buffer given
    by plugin then return error
  */
  if (*buflen < strlen(buffer)) {
    my_free(buffer);
    return 1;
  }
  *buflen = strlen(buffer);
  my_free(buffer);
  return 0;
}

static int validate_sha256_password_hash(char *const inbuf,
                                         unsigned int buflen) {
  if ((inbuf && inbuf[0] == '$' && inbuf[1] == '5' && inbuf[2] == '$' &&
       buflen < CRYPT_MAX_PASSWORD_SIZE + 1) ||
      buflen == 0)
    return 0;
  return 1;
}

static int set_sha256_salt(const char *password [[maybe_unused]],
                           unsigned int password_len [[maybe_unused]],
                           unsigned char *salt [[maybe_unused]],
                           unsigned char *salt_len) {
  *salt_len = 0;
  return 0;
}

/**
  Interface for querying the MYSQL_PUBLIC_VIO about encryption state.

*/

static int my_vio_is_encrypted(MYSQL_PLUGIN_VIO *vio) {
  MPVIO_EXT *mpvio = (MPVIO_EXT *)vio;
  return (mpvio->vio_is_encrypted);
}

/*
  The unused parameters must be here due to function pointer casting
  in sql_show.cc.
*/
int show_rsa_public_key(THD *, SHOW_VAR *var [[maybe_unused]], char *) {
  var->type = SHOW_CHAR;
  var->value = const_cast<char *>(g_sha256_rsa_keys->get_public_key_as_pem());
  return 0;
}

void deinit_rsa_keys(void) {
  if (g_sha256_rsa_keys) {
    g_sha256_rsa_keys->free_memory();
    delete g_sha256_rsa_keys;
    g_sha256_rsa_keys = nullptr;
  }
  if (g_caching_sha2_rsa_keys) {
    g_caching_sha2_rsa_keys->free_memory();
    delete g_caching_sha2_rsa_keys;
    g_caching_sha2_rsa_keys = nullptr;
  }
}

// Wraps a FILE handle, to ensure we always close it when returning.
class FileCloser {
  FILE *m_file;

 public:
  FileCloser(FILE *to_be_closed) : m_file(to_be_closed) {}
  ~FileCloser() {
    if (m_file != nullptr) fclose(m_file);
  }
};

/**
  Loads the RSA key pair from disk and store them in a global variable.

 @see init_ssl()

 @return Error code
   @retval false Success
   @retval true Error
*/

bool init_rsa_keys(void) {
  if ((strcmp(auth_rsa_private_key_path, AUTH_DEFAULT_RSA_PRIVATE_KEY) == 0 &&
       strcmp(auth_rsa_public_key_path, AUTH_DEFAULT_RSA_PUBLIC_KEY) == 0) ||
      (strcmp(caching_sha2_rsa_private_key_path,
              AUTH_DEFAULT_RSA_PRIVATE_KEY) == 0 &&
       strcmp(caching_sha2_rsa_public_key_path, AUTH_DEFAULT_RSA_PUBLIC_KEY) ==
           0)) {
    /**
      Presence of only a private key file and a public temp file implies that
      server crashed after creating the private key file and could not create a
      public key file. Hence removing the private key file.
    */
    if (access(AUTH_DEFAULT_RSA_PRIVATE_KEY, F_OK) == 0 &&
        access(AUTH_DEFAULT_RSA_PUBLIC_KEY, F_OK) == -1) {
      if (access((std::string{AUTH_DEFAULT_RSA_PUBLIC_KEY} + ".temp").c_str(),
                 F_OK) == 0 &&
          access((std::string{AUTH_DEFAULT_RSA_PRIVATE_KEY} + ".temp").c_str(),
                 F_OK) == -1)
        remove(AUTH_DEFAULT_RSA_PRIVATE_KEY);
    }
    // Removing temp files
    remove((std::string{AUTH_DEFAULT_RSA_PRIVATE_KEY} + ".temp").c_str());
    remove((std::string{AUTH_DEFAULT_RSA_PUBLIC_KEY} + ".temp").c_str());
  }

  if (!do_auto_rsa_keys_generation()) return true;

  if (!(g_sha256_rsa_keys = new Rsa_authentication_keys(
            &auth_rsa_private_key_path, &auth_rsa_public_key_path)))
    return true;
  if (!(g_caching_sha2_rsa_keys =
            new Rsa_authentication_keys(&caching_sha2_rsa_private_key_path,
                                        &caching_sha2_rsa_public_key_path))) {
    delete g_sha256_rsa_keys;
    g_sha256_rsa_keys = nullptr;
    return true;
  }

  return (g_sha256_rsa_keys->read_rsa_keys() ||
          g_caching_sha2_rsa_keys->read_rsa_keys());
}

static MYSQL_PLUGIN plugin_info_ptr;

static int init_sha256_password_handler(MYSQL_PLUGIN plugin_ref) {
  plugin_info_ptr = plugin_ref;
  return 0;
}

/**
  Compare a clear text password with a stored hash

  Checks if a stored hash is produced using a clear text password.
  To do that first it extracts the scramble from the hash. Then
  calculates a new hash using the extracted scramble and the supplied
  password. And finally compares the two scrambles.

  @arg hash              pointer to the hashed data
  @arg hash_length       length of the hashed data
  @arg cleartext         pointer to the clear text password
  @arg cleartext_length  length of the cleat text password
  @arg[out] is_error     non-zero in case of error extracting the salt
  @retval 0              the hash was created with that password
  @retval non-zero       the hash was created with a different password
*/
static int compare_sha256_password_with_hash(const char *hash,
                                             unsigned long hash_length,
                                             const char *cleartext,
                                             unsigned long cleartext_length,
                                             int *is_error) {
  char stage2[CRYPT_MAX_PASSWORD_SIZE + 1];
  const char *user_salt_begin;
  const char *user_salt_end;

  DBUG_TRACE;
  assert(cleartext_length <= SHA256_PASSWORD_MAX_PASSWORD_LENGTH);

  if (cleartext_length > SHA256_PASSWORD_MAX_PASSWORD_LENGTH) return -1;

  /*
    Fetch user authentication_string and extract the password salt
  */
  user_salt_begin = hash;
  user_salt_end = hash + hash_length;
  if (extract_user_salt(&user_salt_begin, &user_salt_end) !=
      CRYPT_SALT_LENGTH) {
    *is_error = 1;
    return -1;
  }

  *is_error = 0;

  /* Create hash digest */
  my_crypt_genhash(stage2, CRYPT_MAX_PASSWORD_SIZE, cleartext, cleartext_length,
                   user_salt_begin, (const char **)nullptr);

  /* Compare the newly created hash digest with the password record */
  const int result = memcmp(hash, stage2, hash_length);

  return result;
}

/**

 @param vio Virtual input-, output interface
 @param [out] info Connection information

 Authenticate the user by receiving a RSA or TLS encrypted password and
 calculate a hash digest which should correspond to the user record digest

 RSA keys are assumed to be pre-generated and supplied when server starts. If
 the client hasn't got a public key it can request one.

 TLS certificates and keys are assumed to be pre-generated and supplied when
 server starts.

*/

static int sha256_password_authenticate(MYSQL_PLUGIN_VIO *vio,
                                        MYSQL_SERVER_AUTH_INFO *info) {
  char scramble[SCRAMBLE_LENGTH + 1];
  uchar *pkt;
  int pkt_len;
  int cipher_length = 0;
  unsigned char plain_text[MAX_CIPHER_LENGTH + 1];
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_PKEY *private_key = nullptr;
  EVP_PKEY *public_key = nullptr;
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *private_key = nullptr;
  RSA *public_key = nullptr;
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  DBUG_TRACE;

  /*
   Deprecate message for SHA-256 authentication plugin.
  */
  LogPluginErr(
      WARNING_LEVEL, ER_SERVER_WARN_DEPRECATED,
      Cached_authentication_plugins::get_plugin_name(PLUGIN_SHA256_PASSWORD),
      Cached_authentication_plugins::get_plugin_name(
          PLUGIN_CACHING_SHA2_PASSWORD));
  generate_user_salt(scramble, SCRAMBLE_LENGTH + 1);

  /*
    Note: The nonce is split into 8 + 12 bytes according to
    https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_v10.html
    Native authentication sent 20 bytes + '\0' character = 21 bytes.
    This plugin must do the same to stay consistent with historical behavior
    if it is set to operate as a default plugin.
  */
  if (vio->write_packet(vio, (unsigned char *)scramble, SCRAMBLE_LENGTH + 1))
    return CR_ERROR;

  /*
    After the call to read_packet() the user name will appear in
    mpvio->acl_user and info will contain current data.
  */
  if ((pkt_len = vio->read_packet(vio, &pkt)) == -1) return CR_ERROR;

  /*
    If first packet is a 0 byte then the client isn't sending any password
    else the client will send a password.

    The original intention was that the password is a string[NUL] but this
    never got enforced properly so now we have to accept that an empty packet
    is a blank password, thus the check for pkt_len == 0 has to be made too.
  */
  if ((pkt_len == 0 || pkt_len == 1) && *pkt == 0) {
    info->password_used = PASSWORD_USED_NO;
    /*
      Send OK signal; the authentication might still be rejected based on
      host mask.
    */
    if (info->auth_string_length == 0) {
      if (sha256_password_proxy_users) {
        *info->authenticated_as = PROXY_FLAG;
        DBUG_PRINT("info", ("sha256_password_proxy_users is enabled \
                             , setting authenticated_as to NULL"));
      }
      return CR_OK;
    } else
      return CR_ERROR;
  } else
    info->password_used = PASSWORD_USED_YES;

  if (!my_vio_is_encrypted(vio)) {
    /*
      Since a password is being used it must be encrypted by RSA since no
      other encryption is being active.
    */
    private_key = g_sha256_rsa_keys->get_private_key();
    public_key = g_sha256_rsa_keys->get_public_key();

    /*
      Without the keys encryption isn't possible.
    */
    if (private_key == nullptr || public_key == nullptr) {
      LogPluginErr(ERROR_LEVEL, ER_SHA_PWD_AUTH_REQUIRES_RSA_OR_SSL);
      return CR_ERROR;
    }

    if ((cipher_length = g_sha256_rsa_keys->get_cipher_length()) >
        MAX_CIPHER_LENGTH) {
      LogPluginErr(ERROR_LEVEL, ER_SHA_PWD_RSA_KEY_TOO_LONG,
                   g_sha256_rsa_keys->get_cipher_length(), MAX_CIPHER_LENGTH);
      return CR_ERROR;
    }

    /*
      Client sent a "public key request"-packet ?
      If the first packet is 1 then the client will require a public key
      before encrypting the password.
    */
    if (pkt_len == 1 && *pkt == 1) {
      const uint pem_length =
          static_cast<uint>(strlen(g_sha256_rsa_keys->get_public_key_as_pem()));
      if (vio->write_packet(vio,
                            pointer_cast<const uchar *>(
                                g_sha256_rsa_keys->get_public_key_as_pem()),
                            pem_length))
        return CR_ERROR;
      /* Get the encrypted response from the client */
      if ((pkt_len = vio->read_packet(vio, &pkt)) == -1) return CR_ERROR;
    }

    /*
      The packet will contain the cipher used. The length of the packet
      must correspond to the expected cipher length.
    */
    if (pkt_len != cipher_length) return CR_ERROR;

    /* Decrypt password */
    if (decrypt_RSA_private_key(pkt, cipher_length, plain_text,
                                sizeof(plain_text) - 1, private_key))
      return CR_ERROR;

    plain_text[cipher_length] = '\0';  // safety
    xor_string((char *)plain_text, cipher_length, (char *)scramble,
               SCRAMBLE_LENGTH);

    /*
      Set packet pointers and length for the hash digest function below
    */
    pkt = plain_text;
    pkt_len = strlen((char *)plain_text) + 1;  // include \0 intentionally.

    if (pkt_len == 1) return CR_ERROR;
  }  // if(!my_vio_is_encrypted())

  /* Don't process the password if it is longer than maximum limit */
  if (pkt_len > SHA256_PASSWORD_MAX_PASSWORD_LENGTH + 1) return CR_ERROR;

  /* A password was sent to an account without a password */
  if (info->auth_string_length == 0 && info->additional_auth_string_length == 0)
    return CR_ERROR;

  int is_error = 0;
  int result = compare_sha256_password_with_hash(
      info->auth_string, info->auth_string_length, (const char *)pkt,
      pkt_len - 1, &is_error);

  if (is_error) {
    /* User salt is not correct */
    LogPluginErr(ERROR_LEVEL, ER_SHA_PWD_SALT_FOR_USER_CORRUPT,
                 info->user_name);
    return CR_ERROR;
  }

  if (result && info->additional_auth_string_length) {
    result = compare_sha256_password_with_hash(
        info->additional_auth_string, info->additional_auth_string_length,
        (const char *)pkt, pkt_len - 1, &is_error);
    if (is_error) {
      /* User salt is not correct */
      LogPluginErr(ERROR_LEVEL, ER_SHA_PWD_SALT_FOR_USER_CORRUPT,
                   info->user_name);
      return CR_ERROR;
    }
    if (result == 0) {
      MPVIO_EXT *mpvio = (MPVIO_EXT *)vio;
      const char *username =
          *info->authenticated_as ? info->authenticated_as : "";
      const char *hostname = mpvio->acl_user->host.get_host();
      LogPluginErr(INFORMATION_LEVEL,
                   ER_SHA256_PASSWORD_SECOND_PASSWORD_USED_INFORMATION,
                   username, hostname ? hostname : "");
    }
  }

  if (result == 0) {
    if (sha256_password_proxy_users) {
      *info->authenticated_as = PROXY_FLAG;
      DBUG_PRINT("info", ("sha256_password_proxy_users is enabled \
						   , setting authenticated_as to NULL"));
    }
    return CR_OK;
  }

  return CR_ERROR;
}

static MYSQL_SYSVAR_STR(
    private_key_path, auth_rsa_private_key_path,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_NOPERSIST,
    "A fully qualified path to the private RSA key used for authentication",
    nullptr, nullptr, AUTH_DEFAULT_RSA_PRIVATE_KEY);
static MYSQL_SYSVAR_STR(
    public_key_path, auth_rsa_public_key_path,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_NOPERSIST,
    "A fully qualified path to the public RSA key used for authentication",
    nullptr, nullptr, AUTH_DEFAULT_RSA_PUBLIC_KEY);
static MYSQL_SYSVAR_BOOL(
    auto_generate_rsa_keys, auth_rsa_auto_generate_rsa_keys,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_NOPERSIST,
    "Auto generate RSA keys at server startup if corresponding "
    "system variables are not specified and key files are not present "
    "at the default location.",
    nullptr, nullptr, true);

static SYS_VAR *sha256_password_sysvars[] = {
    MYSQL_SYSVAR(private_key_path), MYSQL_SYSVAR(public_key_path),
    MYSQL_SYSVAR(auto_generate_rsa_keys), nullptr};

typedef std::string Sql_string_t;

/**
  Exception free resize

  @param [in,out] content string handle
  @param [in] size New size

  @retval false  Error
  @retval true  Successfully resized
*/
static bool resize_no_exception(Sql_string_t &content, size_t size) {
  try {
    content.resize(size);
  } catch (const std::length_error &) {
    return false;
  } catch (std::bad_alloc &) {
    return false;
  }
  return true;
}

/**

  FILE_IO : Wrapper around std::fstream
  1> Provides READ/WRITE handle to a file
  2> Records error on READ/WRITE operations
  3> Closes file before destruction

*/

class File_IO {
 public:
  File_IO(const File_IO &src)
      : m_file_name(src.file_name()),
        m_read(src.read_mode()),
        m_error_state(src.get_error()),
        m_file(-1) {
    file_open();
  }

  File_IO &operator=(const File_IO &src) {
    m_file_name = src.file_name();
    m_read = src.read_mode();
    file_open();
    return *this;
  }

  ~File_IO() { close(); }

  /*
    Close an already open file.
  */
  void close() {
    if (file_is_open()) {
      my_close(m_file, MYF(MY_WME));
      m_file = -1;
    }
  }

  /*
    Get name of the file. Used by copy constructor
  */
  const Sql_string_t &file_name() const { return m_file_name; }

  /*
    Get file IO mode. Used by copy constructor.
  */
  bool read_mode() const { return m_read; }

  /*
    Get READ/WRITE error status.
  */
  bool get_error() const { return m_error_state; }

  /*
    Set error. Used by >> and << functions.
  */
  void set_error() { m_error_state = true; }

  void reset_error() { m_error_state = false; }

  File_IO &operator>>(Sql_string_t &s);
  File_IO &operator<<(const Sql_string_t &output_string);

 protected:
  File_IO() = default;
  File_IO(const Sql_string_t filename, bool read)
      : m_file_name(filename), m_read(read), m_error_state(false), m_file(-1) {
    file_open();
  }

  /**
    A constructor to create the class with the right umask mode
    @param filename name of the file
    @param mode the create attributes to pass to my_create()
  */
  File_IO(const Sql_string_t filename, MY_MODE mode)
      : m_file_name(filename), m_read(false), m_error_state(false), m_file(-1) {
    m_file = my_create(m_file_name.c_str(), mode, O_WRONLY, MYF(MY_WME));
  }

  void file_open() {
    m_file =
        my_open(m_file_name.c_str(),
                m_read ? O_RDONLY : O_WRONLY | O_TRUNC | O_CREAT, MYF(MY_WME));
  }

  bool file_is_open() { return m_file >= 0; }

 private:
  Sql_string_t m_file_name;
  bool m_read;
  bool m_error_state;
  File m_file;
  /* Only File_creator can create File_IO */
  friend class File_creator;
};

/**
  Read an open file.

  @param [out] s String buffer

  Assumption : Caller will free string buffer

  @returns File_IO reference. Optionally sets error.
*/
File_IO &File_IO::operator>>(Sql_string_t &s) {
  assert(read_mode() && file_is_open());

  const my_off_t off = my_seek(m_file, 0, SEEK_END, MYF(MY_WME));
  if (off == MY_FILEPOS_ERROR || resize_no_exception(s, off) == false)
    set_error();
  else {
    if (MY_FILEPOS_ERROR == my_seek(m_file, 0, SEEK_SET, MYF(MY_WME)) ||
        (size_t)-1 ==
            my_read(m_file, reinterpret_cast<uchar *>(&s[0]), s.size(), MYF(0)))
      set_error();
    close();
  }
  return *this;
}

/**
  Write into an open file

  @param [in] output_string Content to be written

  Assumption : string must be non-empty.

  @returns File_IO reference. Optionally sets error.
*/
File_IO &File_IO::operator<<(const Sql_string_t &output_string) {
  assert(!read_mode() && file_is_open());

  if (!output_string.size() ||
      MY_FILE_ERROR ==
          my_write(m_file,
                   reinterpret_cast<const uchar *>(output_string.data()),
                   output_string.length(), MYF(MY_NABP | MY_WME)))
    set_error();
  else
    my_sync(m_file, MYF(MY_WME));

  close();
  return *this;
}

/*
  Helper class to create a File_IO handle.
  Can be extended in future to set more file specific properties.
  Frees allocated memory in destructor.
*/
class File_creator {
 public:
  File_creator() = default;

  ~File_creator() {
    for (std::vector<File_IO *>::iterator it = m_file_vector.begin();
         it != m_file_vector.end(); ++it)
      delete (*it);
  }

  /*
    Note : Do not free memory.
  */
  File_IO *operator()(const Sql_string_t filename, bool read = false) {
    File_IO *f = new File_IO(filename, read);
    m_file_vector.push_back(f);
    return f;
  }

  /*
    Note : Do not free memory.
  */
  File_IO *operator()(const Sql_string_t filename, MY_MODE mode) {
    File_IO *f = new File_IO(filename, mode);
    m_file_vector.push_back(f);
    return f;
  }

 private:
  std::vector<File_IO *> m_file_vector;
};

/*
  This class encapsulates OpenSSL specific details of RSA key generation.
  It provides interfaces to:

  1> Get RSA structure
  2> Get EVP_PKEY structure
  3> Write Private/Public key into a string
  4> Free RSA/EVP_PKEY structures
*/
class RSA_gen {
 public:
  RSA_gen(uint32_t key_size, uint32_t exponent = RSA_F4)
      : m_key_size(key_size), m_exponent(exponent) {}

  ~RSA_gen() = default;

  /**
    Passing key type is a violation against the principle of generic
    programming when this operator is used in an algorithm
    but it at the same time increases usefulness of this class when used
    stand alone.
   */
  /* generate RSA keys */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_PKEY *operator()(void) {
    EVP_PKEY *rsa = nullptr;
    BIGNUM *exponent = BN_new();
    if (!exponent) return nullptr;
    EVP_PKEY_CTX *rsa_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!rsa_ctx) {
      BN_free(exponent);
      return nullptr;
    }
    if (BN_set_word(exponent, m_exponent) != 1 ||
        EVP_PKEY_keygen_init(rsa_ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(rsa_ctx, m_key_size) <= 0 ||
        EVP_PKEY_CTX_set1_rsa_keygen_pubexp(rsa_ctx, exponent) <= 0 ||
        EVP_PKEY_keygen(rsa_ctx, &rsa) <= 0) {
      BN_free(exponent);
      EVP_PKEY_CTX_free(rsa_ctx);
      return nullptr;
    }
    BN_free(exponent);
    EVP_PKEY_CTX_free(rsa_ctx);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *operator()(void) {
    RSA *rsa = RSA_new();
    if (!rsa) return nullptr;
    BIGNUM *e = BN_new();
    if (!e) {
      RSA_free(rsa);
      return nullptr;
    }
    if (!BN_set_word(e, m_exponent) ||
        !RSA_generate_key_ex(rsa, m_key_size, e, nullptr)) {
      RSA_free(rsa);
      BN_free(e);
      return nullptr;
    }
    BN_free(e);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

    return rsa;  // pass ownership
  }

 private:
  uint32_t m_key_size;
  uint32_t m_exponent;
};

#if OPENSSL_VERSION_NUMBER < 0x30000000L
static EVP_PKEY *evp_pkey_generate(RSA *rsa) {
  if (rsa) {
    EVP_PKEY *pkey = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(pkey, rsa);
    return pkey;
  }
  return nullptr;
}
#endif /* OPENSSL_VERSION_NUMBER < 0x30000000L */

/**
  Write private key in a string buffer

  @param [in] rsa Handle to RSA structure where private key is stored

  @returns Sql_string_t object with private key stored in it.
*/
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static Sql_string_t rsa_priv_key_write(EVP_PKEY *rsa) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
static Sql_string_t rsa_priv_key_write(RSA *rsa) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  assert(rsa);
  BIO *buf = BIO_new(BIO_s_mem());
  Sql_string_t read_buffer;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  if (PEM_write_bio_PrivateKey(buf, rsa, nullptr, nullptr, 0, nullptr,
                               nullptr)) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  if (PEM_write_bio_RSAPrivateKey(buf, rsa, nullptr, nullptr, 0, nullptr,
                                  nullptr)) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    size_t len = BIO_pending(buf);
    if (resize_no_exception(read_buffer, len + 1) == true) {
      BIO_read(buf, const_cast<char *>(read_buffer.c_str()), len);
      read_buffer[len] = '\0';
    }
  }
  BIO_free(buf);
  return read_buffer;
}

/**
  Write public key in a string buffer

  @param [in] rsa Handle to RSA structure where public key is stored

  @returns Sql_string_t object with public key stored in it.
*/
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static Sql_string_t rsa_pub_key_write(EVP_PKEY *rsa) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
static Sql_string_t rsa_pub_key_write(RSA *rsa) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  assert(rsa);
  BIO *buf = BIO_new(BIO_s_mem());
  Sql_string_t read_buffer;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  if (PEM_write_bio_PUBKEY(buf, rsa)) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  if (PEM_write_bio_RSA_PUBKEY(buf, rsa)) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    size_t len = BIO_pending(buf);
    if (resize_no_exception(read_buffer, len + 1) == true) {
      BIO_read(buf, const_cast<char *>(read_buffer.c_str()), len);
      read_buffer[len] = '\0';
    }
  }
  BIO_free(buf);
  return read_buffer;
}

/*
  This class encapsulates OpenSSL specific details of X509 certificate
  generation. It provides interfaces to:

  1> Generate X509 certificate
  2> Read/Write X509 certificate from/to a string
  3> Read/Write Private key from/to a string
  4> Free X509/EVP_PKEY structures
*/
class X509_gen {
 public:
  X509 *operator()(EVP_PKEY *pkey, const Sql_string_t cn, uint32_t serial,
                   uint32_t notbefore, uint32_t notafter, bool self_sign = true,
                   X509 *ca_x509 = nullptr, EVP_PKEY *ca_pkey = nullptr) {
    X509 *x509 = X509_new();
    X509_EXTENSION *ext = nullptr;
    X509V3_CTX v3ctx;
    X509_NAME *name = nullptr;

    assert(cn.length() <= MAX_CN_NAME_LENGTH);
    assert(serial != 0);
    assert(self_sign || (ca_x509 != nullptr && ca_pkey != nullptr));
    if (!x509) goto err;

    /** Set certificate version */
    if (!X509_set_version(x509, 2)) goto err;

    /** Set serial number */
    if (!ASN1_INTEGER_set(X509_get_serialNumber(x509), serial)) goto err;

    /** Set certificate validity */
    if (!X509_gmtime_adj(X509_get_notBefore(x509), notbefore) ||
        !X509_gmtime_adj(X509_get_notAfter(x509), notafter))
      goto err;

    /** Set public key */
    if (!X509_set_pubkey(x509, pkey)) goto err;

    /** Set CN value in subject */
    name = X509_get_subject_name(x509);
    if (!name) goto err;

    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)cn.c_str(), -1, -1,
                                    0))
      goto err;

    /** Set Issuer */
    if (!X509_set_issuer_name(
            x509, self_sign ? name : X509_get_subject_name(ca_x509)))
      goto err;

    /** Add X509v3 extensions */
    X509V3_set_ctx(&v3ctx, self_sign ? x509 : ca_x509, x509, nullptr, nullptr,
                   0);

    /** Add CA:TRUE / CA:FALSE inforamation */
    if (!(ext = X509V3_EXT_conf_nid(
              nullptr, &v3ctx, NID_basic_constraints,
              self_sign ? const_cast<char *>("critical,CA:TRUE")
                        : const_cast<char *>("critical,CA:FALSE"))))
      goto err;
    X509_add_ext(x509, ext, -1);
    X509_EXTENSION_free(ext);

    /** Sign using SHA256 */
    if (!X509_sign(x509, self_sign ? pkey : ca_pkey, EVP_sha256())) goto err;

    return x509;
  err:
    if (x509) X509_free(x509);
    return nullptr;
  }
};

/**
  Read a X509 certificate into X509 format

  @param [in] input_string Content of X509 certificate file.

  @returns Handle to X509 structure.

  Assumption : Caller will free X509 object
*/
static X509 *x509_cert_read(const Sql_string_t &input_string) {
  X509 *x509 = nullptr;

  if (!input_string.size()) return x509;

  BIO *buf = BIO_new(BIO_s_mem());
  BIO_write(buf, input_string.c_str(), input_string.size());
  x509 = PEM_read_bio_X509(buf, nullptr, nullptr, nullptr);
  BIO_free(buf);
  return x509;
}

/**
  Write X509 certificate into a string

  @param [in] cert Certificate information in X509 format.

  @returns certificate information in string format.
*/
static Sql_string_t x509_cert_write(X509 *cert) {
  assert(cert);
  BIO *buf = BIO_new(BIO_s_mem());
  Sql_string_t read_buffer;
  if (PEM_write_bio_X509(buf, cert)) {
    size_t len = BIO_pending(buf);
    if (resize_no_exception(read_buffer, len + 1) == true) {
      BIO_read(buf, const_cast<char *>(read_buffer.c_str()), len);
      read_buffer[len] = '\0';
    }
  }
  BIO_free(buf);
  return read_buffer;
}

/**
  Read Private key into EVP_PKEY structure

  @param [in] input_string Content of private key file.

  @returns Handle to EVP_PKEY structure.

  Assumption : Caller will free EVP_PKEY object
*/
static EVP_PKEY *x509_key_read(const Sql_string_t &input_string) {
  EVP_PKEY *pkey = nullptr;

  if (!input_string.size()) return pkey;

  BIO *buf = BIO_new(BIO_s_mem());
  BIO_write(buf, input_string.c_str(), input_string.size());
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  pkey = PEM_read_bio_PrivateKey(buf, nullptr, nullptr, nullptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *rsa = PEM_read_bio_RSAPrivateKey(buf, nullptr, nullptr, nullptr);
  pkey = evp_pkey_generate(rsa);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  BIO_free(buf);
  return pkey;
}

/**
  Write X509 certificate into a string

  @param [in] pkey Private key information.

  @returns private key information in string format.
*/
static Sql_string_t x509_key_write(EVP_PKEY *pkey) {
  assert(pkey);
  BIO *buf = BIO_new(BIO_s_mem());
  Sql_string_t read_buffer;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  if (PEM_write_bio_PrivateKey(buf, pkey, nullptr, nullptr, 10, nullptr,
                               nullptr)) {
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *rsa = EVP_PKEY_get1_RSA(pkey);
  if (PEM_write_bio_RSAPrivateKey(buf, rsa, nullptr, nullptr, 10, nullptr,
                                  nullptr)) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    size_t len = BIO_pending(buf);
    if (resize_no_exception(read_buffer, len + 1) == true) {
      BIO_read(buf, const_cast<char *>(read_buffer.c_str()), len);
      read_buffer[len] = '\0';
    }
  }
  BIO_free(buf);
#if OPENSSL_VERSION_NUMBER < 0x30000000L
  RSA_free(rsa);
#endif /* OPENSSL_VERSION_NUMBER < 0x30000000L */
  return read_buffer;
}

/**
  Algorithm to create X509 certificate.
  Relies on:
  1> RSA key generator
  2> X509 certificate generator
  3> FILE reader/writer

  Overwrites key/certificate files if already present.

  @param [in] rsa_gen RSA generator
  @param [in] cn Common name field of X509 certificate.
  @param [in] serial Certificate serial number
  @param [in] cert_filename File name for X509 certificate
  @param [in] key_filename File name for private key
  @param [in] filecr File creator
  @param [in] ca_key_file CA private key file
  @param [in] ca_cert_file CA certificate file

  @returns generation status
    @retval false Error in key/certificate generation.
    @retval true key/certificate files are generated successfully.
*/

template <typename RSA_generator_func, typename File_creation_func>
bool create_x509_certificate(RSA_generator_func &rsa_gen, const Sql_string_t cn,
                             uint32_t serial, const Sql_string_t cert_filename,
                             const Sql_string_t key_filename,
                             File_creation_func &filecr,
                             const Sql_string_t ca_key_file = "",
                             const Sql_string_t ca_cert_file = "") {
  bool ret_val = true;
  bool self_sign = true;
  Sql_string_t ca_key_str;
  Sql_string_t ca_cert_str;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_PKEY *rsa = nullptr;
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *rsa = nullptr;
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  EVP_PKEY *pkey = nullptr;
  EVP_PKEY *ca_key = nullptr;
  X509 *x509 = nullptr;
  X509 *ca_x509 = nullptr;
  File_IO *x509_key_file_ostream = nullptr;
  File_IO *x509_cert_file_ostream = nullptr;
  File_IO *x509_ca_key_file_istream = nullptr;
  File_IO *x509_ca_cert_file_istream = nullptr;
  X509_gen x509_gen;
  MY_MODE file_creation_mode = get_file_perm(USER_READ | USER_WRITE);

  x509_key_file_ostream = filecr(key_filename, file_creation_mode);

  /* Generate private key for X509 certificate */
  rsa = rsa_gen();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  DBUG_EXECUTE_IF("null_rsa_error", {
    EVP_PKEY_free(rsa);
    rsa = nullptr;
  });
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  DBUG_EXECUTE_IF("null_rsa_error", {
    RSA_free(rsa);
    rsa = nullptr;
  });
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  if (!rsa) {
    LogErr(ERROR_LEVEL, ER_X509_NEEDS_RSA_PRIVKEY);
    ret_val = false;
    goto end;
  }

  /* Obtain EVP_PKEY */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  pkey = rsa;
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  pkey = evp_pkey_generate(rsa);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  /* Write private key information to file and set file permission */
  (*x509_key_file_ostream) << x509_key_write(pkey);
  DBUG_EXECUTE_IF("key_file_write_error",
                  { x509_key_file_ostream->set_error(); });
  if (x509_key_file_ostream->get_error()) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_WRITE_KEY, key_filename.c_str());
    ret_val = false;
    goto end;
  }

  /*
    Read CA key/certificate files in PEM format.
  */
  if (ca_key_file.size() && ca_cert_file.size()) {
    x509_ca_key_file_istream = filecr(ca_key_file, true);
    x509_ca_cert_file_istream = filecr(ca_cert_file, true);
    (*x509_ca_key_file_istream) >> ca_key_str;
    ca_key = x509_key_read(ca_key_str);
    DBUG_EXECUTE_IF("ca_key_read_error", {
      EVP_PKEY_free(ca_key);
      ca_key = nullptr;
    });
    if (!ca_key) {
      LogErr(ERROR_LEVEL, ER_X509_CANT_READ_CA_KEY, ca_key_file.c_str());
      ret_val = false;
      goto end;
    }

    (*x509_ca_cert_file_istream) >> ca_cert_str;
    ca_x509 = x509_cert_read(ca_cert_str);
    DBUG_EXECUTE_IF("ca_cert_read_error", {
      X509_free(ca_x509);
      ca_x509 = nullptr;
    });
    if (!ca_x509) {
      LogErr(ERROR_LEVEL, ER_X509_CANT_READ_CA_CERT, ca_cert_file.c_str());
      ret_val = false;
      goto end;
    }

    self_sign = false;
  }

  /* Create X509 certificate with validity of 10 year */
  x509 = x509_gen(pkey, cn, serial, 0, 365L * 24 * 60 * 60 * 10, self_sign,
                  ca_x509, ca_key);
  DBUG_EXECUTE_IF("x509_cert_generation_error", {
    X509_free(x509);
    x509 = nullptr;
  });
  if (!x509) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_CREATE_CERT);
    ret_val = false;
    goto end;
  }

  /* Write X509 certificate to file and set permission */
  x509_cert_file_ostream = filecr(cert_filename);
  (*x509_cert_file_ostream) << x509_cert_write(x509);
  DBUG_EXECUTE_IF("cert_pub_key_write_error",
                  { x509_cert_file_ostream->set_error(); });
  if (x509_cert_file_ostream->get_error()) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_WRITE_CERT, cert_filename.c_str());
    ret_val = false;
    goto end;
  }

  if (my_chmod(cert_filename.c_str(),
               USER_READ | USER_WRITE | GROUP_READ | OTHERS_READ,
               MYF(MY_FAE + MY_WME))) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_CHMOD_KEY, cert_filename.c_str());
    ret_val = false;
    goto end;
  }

end:

  if (pkey) EVP_PKEY_free(pkey); /* Frees rsa too */
  if (ca_key) EVP_PKEY_free(ca_key);
  if (x509) X509_free(x509);
  if (ca_x509) X509_free(ca_x509);

  return ret_val;
}

/**
  Algorithm to generate RSA key pair.
  Relies on:
  1> RSA generator
  2> File reader/writer

  Overwrites existing Private/Public key file if any.

  @param [in] rsa_gen RSA key pair generator
  @param [in] priv_key_filename File name of private key
  @param [in] pub_key_filename File name of public key
  @param [in] filecr File creator

  @returns status of RSA key pair generation.
    @retval false Error in RSA key pair generation.
    @retval true Private/Public keys are successfully generated.
*/
template <typename RSA_generator_func, typename File_creation_func>
bool create_RSA_key_pair(RSA_generator_func &rsa_gen,
                         const Sql_string_t priv_key_filename,
                         const Sql_string_t pub_key_filename,
                         File_creation_func &filecr) {
  std::string temp_priv_key_filename = priv_key_filename + ".temp";
  std::string temp_pub_key_filename = pub_key_filename + ".temp";
  bool ret_val = true;
  File_IO *priv_key_file_ostream = nullptr;
  File_IO *pub_key_file_ostream = nullptr;
  MY_MODE file_creation_mode = get_file_perm(USER_READ | USER_WRITE);
  MY_MODE saved_umask = umask(~(file_creation_mode));

  assert(temp_priv_key_filename.size() && temp_pub_key_filename.size());

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_PKEY *rsa;
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  RSA *rsa;
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  rsa = rsa_gen();

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  DBUG_EXECUTE_IF("null_rsa_error", {
    EVP_PKEY_free(rsa);
    rsa = nullptr;
  });
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  DBUG_EXECUTE_IF("null_rsa_error", {
    RSA_free(rsa);
    rsa = nullptr;
  });
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  if (!rsa) {
    LogErr(ERROR_LEVEL, ER_AUTH_CANT_CREATE_RSA_PAIR);
    ret_val = false;
    goto end;
  }
  DBUG_EXECUTE_IF("no_key_files", DBUG_SUICIDE(););

  priv_key_file_ostream = filecr(temp_priv_key_filename, file_creation_mode);
  DBUG_EXECUTE_IF("empty_priv_key_temp_file", DBUG_SUICIDE(););

  (*priv_key_file_ostream) << rsa_priv_key_write(rsa);
  DBUG_EXECUTE_IF("key_file_write_error",
                  { priv_key_file_ostream->set_error(); });
  if (priv_key_file_ostream->get_error()) {
    LogErr(ERROR_LEVEL, ER_AUTH_CANT_WRITE_PRIVKEY, priv_key_filename.c_str());
    ret_val = false;
    goto end;
  }
  if (my_chmod(temp_priv_key_filename.c_str(), USER_READ | USER_WRITE,
               MYF(MY_FAE + MY_WME))) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_CHMOD_KEY, priv_key_filename.c_str());
    ret_val = false;
    goto end;
  }
  DBUG_EXECUTE_IF("valid_priv_key_temp_file", DBUG_SUICIDE(););

  pub_key_file_ostream = filecr(temp_pub_key_filename);
  DBUG_EXECUTE_IF("valid_priv_key_temp_file_empty_pub_key_temp_file",
                  DBUG_SUICIDE(););

  (*pub_key_file_ostream) << rsa_pub_key_write(rsa);

  DBUG_EXECUTE_IF("cert_pub_key_write_error",
                  { pub_key_file_ostream->set_error(); });

  if (pub_key_file_ostream->get_error()) {
    LogErr(ERROR_LEVEL, ER_AUTH_CANT_WRITE_PUBKEY, pub_key_filename.c_str());
    ret_val = false;
    goto end;
  }
  if (my_chmod(temp_pub_key_filename.c_str(),
               USER_READ | USER_WRITE | GROUP_READ | OTHERS_READ,
               MYF(MY_FAE + MY_WME))) {
    LogErr(ERROR_LEVEL, ER_X509_CANT_CHMOD_KEY, pub_key_filename.c_str());
    ret_val = false;
    goto end;
  }
  DBUG_EXECUTE_IF("valid_key_temp_files", DBUG_SUICIDE(););

  rename(temp_priv_key_filename.c_str(), priv_key_filename.c_str());
  DBUG_EXECUTE_IF("valid_pub_key_temp_file_valid_priv_key_file",
                  DBUG_SUICIDE(););
  rename(temp_pub_key_filename.c_str(), pub_key_filename.c_str());
  DBUG_EXECUTE_IF("valid_key_files", DBUG_SUICIDE(););

end:
  if (rsa)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY_free(rsa);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
    RSA_free(rsa);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  umask(saved_umask);
  return ret_val;
}

/**
  Check auto_generate_certs option and generate
  SSL certificates if required.

  SSL Certificates are generated iff following conditions are met.
  1> auto_generate_certs is set to ON.
  2> None of the SSL system variables are specified.
  3> Following files are not present in data directory.
     a> ca.pem
     b> server_cert.pem
     c> server_key.pem

  If above mentioned conditions are satisfied, following action will be taken:

  1> 6 File are generated and placed data directory:
     a> ca.pem
     b> ca_key.pem
     c> server_cert.pem
     d> server_key.pem
     e> client_cert.pem
     f> client_key.pem

     ca.pem is self signed auto generated CA certificate. server_cert.pem
     and client_cert.pem are signed using auto generated CA.

     ca_key.pem, client_cert.pem and client_key.pem are overwritten if
     they are present in data directory.

  Path of following system variables are set if certificates are either
  generated or already present in data directory.
  a> ssl-ca
  b> ssl-cert
  c> ssl-key

  Assumption : auto_detect_ssl() is called before control reaches to
  do_auto_cert_generation().

  @param [in] auto_detection_status Status of SSL artifacts detection process
  @param [out] ssl_ca  pointer to the generated CA certificate file
  @param [out] ssl_key pointer to the generated key file
  @param [out] ssl_cert pointer to the generated certificate file.

  @retval true i Generation is successful or skipped
  @retval false Generation failed.
*/
bool do_auto_cert_generation(ssl_artifacts_status auto_detection_status,
                             const char **ssl_ca, const char **ssl_key,
                             const char **ssl_cert) {
  if (opt_auto_generate_certs == true) {
    /*
      Do not generate SSL certificates/RSA keys,
      If any of the SSL option was specified.
    */

    int sec_level = security_level();

    if (auto_detection_status == SSL_ARTIFACTS_VIA_OPTIONS) {
      LogErr(INFORMATION_LEVEL, ER_AUTH_SSL_CONF_PREVENTS_CERT_GENERATION);
      return true;
    } else if (auto_detection_status == SSL_ARTIFACTS_AUTO_DETECTED ||
               auto_detection_status == SSL_ARTIFACT_TRACES_FOUND) {
      LogErr(INFORMATION_LEVEL, ER_AUTH_USING_EXISTING_CERTS);
      return true;
    } else {
      assert(auto_detection_status == SSL_ARTIFACTS_NOT_FOUND);

      /* Initialize the key pair generator. It can also be used stand alone */
      RSA_gen rsa_gen(rsa_key_sizes[sec_level]);

      /*
         Initialize the file creator.
       */
      File_creator fcr;
      Sql_string_t ca_name = "MySQL_Server_";
      Sql_string_t server_name = "MySQL_Server_";
      Sql_string_t client_name = "MySQL_Server_";

      ca_name.append(MYSQL_SERVER_VERSION);
      ca_name.append("_Auto_Generated_CA_Certificate");
      server_name.append(MYSQL_SERVER_VERSION);
      server_name.append("_Auto_Generated_Server_Certificate");
      client_name.append(MYSQL_SERVER_VERSION);
      client_name.append("_Auto_Generated_Client_Certificate");

      /*
        Maximum length of X509 certificate subject is 64.
        Make sure that constructed strings are within valid
        bounds or change them to minimal default strings
      */
      if (ca_name.length() > MAX_CN_NAME_LENGTH ||
          server_name.length() > MAX_CN_NAME_LENGTH ||
          client_name.length() > MAX_CN_NAME_LENGTH) {
        ca_name.clear();
        ca_name.append("MySQL_Server_Auto_Generated_CA_Certificate");
        server_name.clear();
        server_name.append("MySQL_Server_Auto_Generated_Server_Certificate");
        client_name.clear();
        client_name.append("MySQL_Server_Auto_Generated_Client_Certificate");
      }

      /* Create and write the certa and keys on disk */
      if ((create_x509_certificate(rsa_gen, ca_name, 1, DEFAULT_SSL_CA_CERT,
                                   DEFAULT_SSL_CA_KEY, fcr) == false) ||
          (create_x509_certificate(
               rsa_gen, server_name, 2, DEFAULT_SSL_SERVER_CERT,
               DEFAULT_SSL_SERVER_KEY, fcr, DEFAULT_SSL_CA_KEY,
               DEFAULT_SSL_CA_CERT) == false) ||
          (create_x509_certificate(
               rsa_gen, client_name, 3, DEFAULT_SSL_CLIENT_CERT,
               DEFAULT_SSL_CLIENT_KEY, fcr, DEFAULT_SSL_CA_KEY,
               DEFAULT_SSL_CA_CERT) == false)) {
        return false;
      }
      *ssl_ca = DEFAULT_SSL_CA_CERT;
      *ssl_cert = DEFAULT_SSL_SERVER_CERT;
      *ssl_key = DEFAULT_SSL_SERVER_KEY;
      LogErr(INFORMATION_LEVEL, ER_AUTH_CERTS_SAVED_TO_DATADIR);
    }
    return true;
  } else {
    LogErr(INFORMATION_LEVEL, ER_AUTH_CERT_GENERATION_DISABLED);
    return true;
  }
}

/*
 Generate RSA keys

 @param [in] auto_generate Variable to control key generation
 @param [in] priv_key_path Path to store/check private key
 @param [in] pub_key_path  Path to store/check public key
 @param [in] message       Message part to be logged

 @returns status of key generation
   @retval true  Success
   @retval false Error generating keys
*/

static bool generate_rsa_keys(bool auto_generate, const char *priv_key_path,
                              const char *pub_key_path, const char *message) {
  DBUG_TRACE;
  if (auto_generate) {
    MY_STAT priv_stat, pub_stat;
    int sec_level = security_level();
    if (strcmp(priv_key_path, AUTH_DEFAULT_RSA_PRIVATE_KEY) ||
        strcmp(pub_key_path, AUTH_DEFAULT_RSA_PUBLIC_KEY)) {
      LogErr(INFORMATION_LEVEL, ER_AUTH_RSA_CONF_PREVENTS_KEY_GENERATION,
             message);
      return true;
    } else if (my_stat(AUTH_DEFAULT_RSA_PRIVATE_KEY, &priv_stat, MYF(0)) ||
               my_stat(AUTH_DEFAULT_RSA_PUBLIC_KEY, &pub_stat, MYF(0))) {
      LogErr(INFORMATION_LEVEL, ER_AUTH_KEY_GENERATION_SKIPPED_PAIR_PRESENT,
             message);
      return true;
    } else {
      /* Initialize the key pair generator. */
      RSA_gen rsa_gen(rsa_key_sizes[sec_level]);

      /* Initialize the file creator. */
      File_creator fcr;

      if (create_RSA_key_pair(rsa_gen, "private_key.pem", "public_key.pem",
                              fcr) == false)
        return false;

      LogErr(INFORMATION_LEVEL, ER_AUTH_KEYS_SAVED_TO_DATADIR, message);
      return true;
    }
  } else {
    LogErr(INFORMATION_LEVEL, ER_AUTH_KEY_GENERATION_DISABLED, message);
    return true;
  }
}

/*
  Generate RSA key pair.

  @returns Status of key generation
    @retval true Success
    @retval false Failure

  Check sha256_password_auto_generate_rsa_keys/
  caching_sha2_password_auto_generate_rsa_keys
  option and generate RSA key pair if required.

  RSA key pair is generated iff following conditions are met.
  1> sha256_password_auto_generate_rsa_keys/
     caching_sha2_password_auto_generate_rsa_keys is set to ON.
  2> sha256_password_private_key_path/caching_sha2_rsa_private_key_path
     or sha256_password_public_key_path/caching_sha2_rsa_public_key_path
     are pointing to non-default locations.
  3> Following files are not present in data directory.
     a> private_key.pem
     b> public_key.pem

  If above mentioned conditions are satisfied private_key.pem and
  public_key.pem files are generated and placed in data directory.
*/
static bool do_auto_rsa_keys_generation() {
  return (generate_rsa_keys(auth_rsa_auto_generate_rsa_keys,
                            auth_rsa_private_key_path, auth_rsa_public_key_path,
                            "--sha256_password_auto_generate_rsa_keys") &&
          generate_rsa_keys(caching_sha2_auto_generate_rsa_keys,
                            caching_sha2_rsa_private_key_path,
                            caching_sha2_rsa_public_key_path,
                            "--caching_sha2_password_auto_generate_rsa_keys"));
}

bool MPVIO_EXT::can_authenticate() {
  return (acl_user && acl_user->can_authenticate);
}

static struct st_mysql_auth sha256_password_handler = {
    MYSQL_AUTHENTICATION_INTERFACE_VERSION,
    Cached_authentication_plugins::get_plugin_name(PLUGIN_SHA256_PASSWORD),
    sha256_password_authenticate,
    generate_sha256_password,
    validate_sha256_password_hash,
    set_sha256_salt,
    AUTH_FLAG_USES_INTERNAL_STORAGE,
    compare_sha256_password_with_hash,
};

mysql_declare_plugin(sha256_password){
    MYSQL_AUTHENTICATION_PLUGIN, /* type constant    */
    &sha256_password_handler,    /* type descriptor  */
    Cached_authentication_plugins::get_plugin_name(
        PLUGIN_SHA256_PASSWORD),      /* Name             */
    PLUGIN_AUTHOR_ORACLE,             /* Author           */
    "SHA256 password authentication", /* Description      */
    PLUGIN_LICENSE_GPL,               /* License          */
    &init_sha256_password_handler,    /* Init function    */
    nullptr,                          /* Check uninstall  */
    nullptr,                          /* Deinit function  */
    0x0101,                           /* Version (1.0)    */
    nullptr,                          /* status variables */
    sha256_password_sysvars,          /* system variables */
    nullptr,                          /* config options   */
    0                                 /* flags            */
} mysql_declare_plugin_end;
