# Copyright (c) 2014, 2025, Oracle and/or its affiliates.
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
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

MSVC_CPPCHECK_DISABLE()

SET(GROUP_REPLICATION_SOURCES
  src/applier.cc
  src/applier_channel_state_observer.cc
  src/asynchronous_channels_state_observer.cc
  src/auto_increment.cc
  src/autorejoin.cc
  src/certifier.cc
  src/certification/certified_gtid.cc
  src/certification/gtid_generator.cc
  src/certification/gtid_generator_for_sidno.cc
  src/compatibility_module.cc
  src/consistency_manager.cc
  src/delayed_plugin_initialization.cc
  src/gcs_event_handlers.cc
  src/gcs_logger.cc
  src/gcs_mysql_network_provider.cc
  src/gcs_operations.cc
  src/gcs_plugin_messages.cc
  src/gcs_view_modification_notifier.cc
  src/gr_compression.cc
  src/gr_decompression.cc
  src/group_actions/communication_protocol_action.cc
  src/group_actions/group_action.cc
  src/group_actions/group_action_coordinator.cc
  src/group_actions/multi_primary_migration_action.cc
  src/group_actions/primary_election_action.cc
  src/group_actions/group_actions_transaction_controller.cc
  src/handlers/applier_handler.cc
  src/handlers/certification_handler.cc
  src/handlers/event_cataloger.cc
  src/hold_transactions.cc
  src/leave_group_on_failure.cc
  src/member_info.cc
  src/member_version.cc
  src/mysql_version_gcs_protocol_map.cc
  src/observer_server_actions.cc
  src/observer_server_channels.cc
  src/observer_server_state.cc
  src/observer_trans.cc
  src/opt_tracker.cc
  src/perfschema/pfs.cc
  src/perfschema/table_replication_group_configuration_version.cc
  src/perfschema/table_replication_group_member_actions.cc
  src/perfschema/table_communication_information.cc
  src/pipeline_factory.cc
  src/pipeline_stats.cc
  src/plugin.cc
  src/plugin_handlers/consensus_leaders_handler.cc
  src/plugin_handlers/group_partition_handling.cc
  src/plugin_handlers/member_actions_handler.cc
  src/plugin_handlers/member_actions_handler_configuration.cc
  src/plugin_handlers/metrics_handler.cc
  src/plugin_handlers/offline_mode_handler.cc
  src/plugin_handlers/primary_election_invocation_handler.cc
  src/plugin_handlers/primary_election_most_uptodate.cc
  src/plugin_handlers/primary_election_primary_process.cc
  src/plugin_handlers/primary_election_secondary_process.cc
  src/plugin_handlers/primary_election_utils.cc
  src/plugin_handlers/primary_election_validation_handler.cc
  src/plugin_handlers/read_mode_handler.cc
  src/plugin_handlers/recovery_metadata.cc
  src/plugin_handlers/recovery_metadata_joiner_information.cc
  src/plugin_handlers/remote_clone_handler.cc
  src/plugin_handlers/server_ongoing_transactions_handler.cc
  src/plugin_handlers/stage_monitor_handler.cc
  src/plugin_messages/group_action_message.cc
  src/plugin_messages/group_service_message.cc
  src/plugin_messages/group_validation_message.cc
  src/plugin_messages/recovery_message.cc
  src/plugin_messages/recovery_metadata_message.cc
  src/plugin_messages/recovery_metadata_message_compressed_parts.cc
  src/plugin_messages/single_primary_message.cc
  src/plugin_messages/sync_before_execution_message.cc
  src/plugin_messages/transaction_message.cc
  src/plugin_messages/transaction_prepared_message.cc
  src/plugin_messages/transaction_with_guarantee_message.cc
  src/plugin_observers/channel_observation_manager.cc
  src/plugin_observers/group_event_observer.cc
  src/plugin_observers/group_transaction_observation_manager.cc
  src/plugin_observers/recovery_metadata_observer.cc
  src/plugin_variables/recovery_endpoints.cc
  src/plugin_psi.cc
  src/plugin_status_variables.cc
  src/plugin_utils.cc
  src/ps_information.cc
  src/recovery.cc
  src/recovery_channel_state_observer.cc
  src/recovery_state_transfer.cc
  src/replication_threads_api.cc
  src/services/management/management.cc
  src/services/message_service/message_service.cc
  src/services/notification/notification.cc
  src/services/status_service/status_service.cc
  src/services/system_variable/get_system_variable.cc
  src/services/system_variable/set_system_variable.cc
  src/services/flow_control/get_metrics.cc
  src/services/registry.cc
  src/services/server_services_references.cc
  src/sql_service/sql_command_test.cc
  src/sql_service/sql_resultset.cc
  src/sql_service/sql_service_command.cc
  src/sql_service/sql_service_context.cc
  src/sql_service/sql_service_context_base.cc
  src/sql_service/sql_service_interface.cc
  src/thread/mysql_thread.cc
  src/udf/udf_communication_protocol.cc
  src/udf/udf_member_actions.cc
  src/udf/udf_multi_primary.cc
  src/udf/udf_registration.cc
  src/udf/udf_single_primary.cc
  src/udf/udf_utils.cc
  src/udf/udf_write_concurrency.cc
)

SET(GMS_LISTENER_SERVICE_TEST_SOURCES
  src/services/notification/impl/gms_listener_test.cc
)

# Allow both WITHOUT_GROUP_REPLICATION=1 and WITH_GROUP_REPLICATION=0 to be
# specified.
# Furthermore, the MYSQL_ADD_PLUGIN will replace them with
# WITHOUT_GROUP_REPLICATION variant. This is because the plugin is defined with
# the MODULE_ONLY flag.
# This causes for all other GR-related directories that will be included later,
# to see only the WITHOUT_GROUP_REPLICATION variant, making the expression to
# test the flag much easier.
IF ((NOT (DEFINED WITHOUT_GROUP_REPLICATION AND WITHOUT_GROUP_REPLICATION)) AND
    (NOT (DEFINED WITH_GROUP_REPLICATION AND NOT WITH_GROUP_REPLICATION)))
  # Destination of generated protobuf files.
  SET(GR_GENERATE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  DISABLE_MISSING_PROFILE_WARNING()

  ADD_SUBDIRECTORY(libmysqlgcs)
  ADD_SUBDIRECTORY(protobuf)

  ADD_DEFINITIONS(-DMYSQL_SERVER)
  ADD_DEFINITIONS(-DLOG_SUBSYSTEM_TAG="Repl")
  ADD_DEFINITIONS(-DLOG_COMPONENT_TAG="group_replication")

  IF(WITH_UNIT_TESTS)
    ADD_LIBRARY(gr_unit_test_resource STATIC
      src/gcs_plugin_messages.cc
      src/member_info.cc
      src/member_version.cc
      src/compatibility_module.cc
      src/mysql_version_gcs_protocol_map.cc
      src/gcs_mysql_network_provider.cc
      src/replication_threads_api.cc
      src/plugin_psi.cc
      src/certification/gtid_generator.cc
      src/certification/gtid_generator_for_sidno.cc
    )
    SET_TARGET_PROPERTIES(gr_unit_test_resource
      PROPERTIES COMPILE_DEFINITIONS "DISABLE_PSI_MUTEX"
      )
    TARGET_COMPILE_FEATURES(gr_unit_test_resource PUBLIC cxx_std_20)
    TARGET_LINK_LIBRARIES(gr_unit_test_resource extra::rapidjson)
    TARGET_LINK_LIBRARIES(gr_unit_test_resource ext::zlib)
    TARGET_LINK_LIBRARIES(gr_unit_test_resource ext::libprotobuf-lite)
    TARGET_LINK_LIBRARIES(gr_unit_test_resource ${GR_PROTOBUF_LITE_LIB})
    ADD_DEPENDENCIES(gr_unit_test_resource GenError)
  ENDIF()

  ### Configuration ###

  IF(WIN32)
    ADD_DEFINITIONS(-DIMPORT_UNKNOWN_SQLSTATE)
  ENDIF()
ENDIF()


ADD_DEFINITIONS(-DLOG_COMPONENT_TAG="group_replication")

GET_TARGET_PROPERTY(lib_type ext::libprotobuf-lite TYPE)
IF((SOLARIS OR LINUX) AND lib_type STREQUAL "STATIC_LIBRARY")
  SET(LINK_WHOLE_ARCHIVE "-Wl,--whole-archive")
  SET(LINK_NO_WHOLE_ARCHIVE "-Wl,--no-whole-archive")
ENDIF()

# declare the plugin itself
MYSQL_ADD_PLUGIN(group_replication
  ${GROUP_REPLICATION_SOURCES}
  ${GMS_LISTENER_SERVICE_TEST_SOURCES}
  LINK_LIBRARIES
  ext::lz4
  ext::zlib
  ext::zstd
  OpenSSL::SSL OpenSSL::Crypto
  ${LINK_WHOLE_ARCHIVE} ext::libprotobuf-lite ${LINK_NO_WHOLE_ARCHIVE}
  ${GR_PROTOBUF_LITE_LIB}
  mysqlgcs
  extra::rapidjson
  library_mysys
  mysql_containers
  mysql_gtid
  MODULE_ONLY
  MODULE_OUTPUT_NAME "group_replication"
  )

# At this point the MYSQL_ADD_PLUGIN macro made the value of
# WITHOUT_GROUP_REPLICATION consistent. If it is true, then there are no
# targets and we don't need to do anymore in this file.
IF (DEFINED WITHOUT_GROUP_REPLICATION AND WITHOUT_GROUP_REPLICATION)
  RETURN()
ENDIF()

IF(UNIX_INSTALL_RPATH_ORIGIN_PRIV_LIBDIR)
  ADD_INSTALL_RPATH_FOR_PROTOBUF(group_replication)
ENDIF()
