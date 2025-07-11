# Copyright (c) 2015, 2025, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

INCLUDE(GenerateExportHeader)

ADD_LIBRARY(sql_lexer STATIC
  sql_lexer.cc
  sql_lexer_thd.cc
  ${CMAKE_SOURCE_DIR}/sql/sql_lex_hash.cc
  )
TARGET_INCLUDE_DIRECTORIES(sql_lexer
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# generate sql_yacc.h
ADD_DEPENDENCIES(sql_lexer GenServerSource)

ADD_LIBRARY(routing_frontend_lib STATIC
  routing.cc
)

TARGET_LINK_LIBRARIES(routing_frontend_lib
  PUBLIC
  harness_stdx)

TARGET_INCLUDE_DIRECTORIES(
  routing_frontend_lib
PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../include/
  ${CMAKE_CURRENT_BINARY_DIR}/../include/
  ${CMAKE_CURRENT_SOURCE_DIR}/../../router/include/
  ${CMAKE_CURRENT_BINARY_DIR}/../../router/include/
)

## routing_connections is used by "routing" and "connection_pool"
ROUTER_ADD_SHARED_LIBRARY(routing_connections
  channel.cc

  OUTPUT_NAME "mysqlrouter_routing_connections"

  LINK_LIBRARIES
  PUBLIC
  harness_stdx
  harness_tls
  mysql_protocol
  )

ROUTER_ADD_SHARED_LIBRARY(routing
  mysql_routing.cc
  destination.cc
  destination_error.cc
  destinations_option_parser.cc
  dest_metadata_cache.cc
  dest_static.cc
  context.cc
  mysql_routing_common.cc
  connection_container.cc
  routing_component.cc
  destination_ssl_context.cc
  classic_connection_base.cc
  classic_connection.cc
  x_connection.cc
  connection.cc
  blocked_endpoints.cc
  routing_guidelines_adapter.cc

  await_client_or_server.cc
  forwarding_processor.cc
  processor.cc

  classic_auth.cc
  classic_auth_cleartext.cc
  classic_auth_caching_sha2.cc
  classic_auth_native.cc
  classic_auth_sha256_password.cc
  classic_command.cc
  classic_connect.cc
  classic_flow.cc
  classic_forwarder.cc
  classic_frame.cc
  classic_lazy_connect.cc

  classic_auth_cleartext_forwarder.cc
  classic_auth_caching_sha2_forwarder.cc
  classic_auth_forwarder.cc
  classic_auth_native_forwarder.cc
  classic_auth_openid_connect_forwarder.cc
  classic_auth_sha256_password_forwarder.cc
  classic_binlog_dump_forwarder.cc
  classic_change_user_forwarder.cc
  classic_clone_forwarder.cc
  classic_debug_forwarder.cc
  classic_greeting_forwarder.cc
  classic_init_schema_forwarder.cc
  classic_kill_forwarder.cc
  classic_list_fields_forwarder.cc
  classic_ping_forwarder.cc
  classic_query_forwarder.cc
  classic_quit_forwarder.cc
  classic_register_replica_forwarder.cc
  classic_reload_forwarder.cc
  classic_reset_connection_forwarder.cc
  classic_set_option_forwarder.cc
  classic_statistics_forwarder.cc
  classic_stmt_close_forwarder.cc
  classic_stmt_execute_forwarder.cc
  classic_stmt_fetch_forwarder.cc
  classic_stmt_param_append_data_forwarder.cc
  classic_stmt_prepare_forwarder.cc
  classic_stmt_reset_forwarder.cc

  classic_auth_cleartext_sender.cc
  classic_auth_caching_sha2_sender.cc
  classic_auth_native_sender.cc
  classic_auth_openid_connect_sender.cc
  classic_auth_sha256_password_sender.cc
  classic_change_user_sender.cc
  # classic_greeting_sender.cc
  classic_init_schema_sender.cc
  classic_query_sender.cc
  classic_quit_sender.cc
  classic_reset_connection_sender.cc
  classic_set_option_sender.cc

  classic_greeting_receiver.cc

  classic_query_param.cc
  classic_session_tracker.cc

  show_warnings_parser.cc
  sql_splitting_allowed.cc
  sql_value.cc
  start_transaction_parser.cc
  implicit_commit_parser.cc
  router_require.cc

  openssl_msg.cc

  OUTPUT_NAME "mysqlrouter_routing"

  DEPENDENCIES GenLiteProtos

  LINK_LIBRARIES
  PUBLIC
  harness_stdx
  harness_tls
  mysql_protocol
  router_utils
  routing_connections
  router_mysqlxmessages
  routing_frontend_lib
  ext::libprotobuf
  metadata_cache
  connection_pool
  destination_status
  sql_lexer
  routing_guidelines
  )

IF(SOLARIS)
  TARGET_LINK_LIBRARIES(routing
    PRIVATE -lnsl
    PRIVATE -lsocket)
ENDIF()

DOWNGRADE_STRINGOP_WARNINGS(routing)

TARGET_COMPILE_DEFINITIONS(routing PRIVATE
  MYSQL_ROUTER_LOG_DOMAIN="routing")

# The Plugin
add_harness_plugin(routing_plugin
 OUTPUT_NAME "routing"
 SOURCES
 routing_plugin.cc
 plugin_config.cc
 REQUIRES routing
 DESTINATION "${ROUTER_INSTALL_PLUGINDIR}"
 )
TARGET_LINK_LIBRARIES(routing_plugin PRIVATE extra::rapidjson)

GENERATE_EXPORT_HEADER(routing_plugin
  EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/../include/mysqlrouter/routing_plugin_export.h)
