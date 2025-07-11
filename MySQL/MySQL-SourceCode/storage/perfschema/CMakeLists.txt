# Copyright (c) 2009, 2025, Oracle and/or its affiliates.
#
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

ADD_DEFINITIONS(-DMYSQL_SERVER)

#
# Maintainer: keep this list sorted, to avoid merge collisions.
# Tip: ls -1 *.h, ls -1 *.cc
#
SET(PERFSCHEMA_SOURCES
cursor_by_account.cc
cursor_by_account.h
cursor_by_error_log.h
cursor_by_host.h
cursor_by_thread.h
cursor_by_thread_connect_attr.h
cursor_by_user.h
ha_perfschema.h
terminology_use_previous.h
terminology_use_previous_enum.h
pfs.h
pfs_account.h
pfs_buffer_container.h
pfs_builtin_memory.h
pfs_column_types.h
pfs_column_values.h
pfs_con_slice.h
pfs_dd_version.h
pfs_defaults.h
pfs_digest.h
pfs_engine_table.h
pfs_error.h
pfs_events.h
pfs_events_stages.h
pfs_events_statements.h
pfs_events_transactions.h
pfs_events_waits.h
pfs_global.h
pfs_histogram.h
pfs_host.h
pfs_instr.h
pfs_instr_class.h
pfs_lock.h
pfs_memory.h
pfs_metrics_service_imp.h
pfs_name.h
pfs_plugin_table.h
pfs_prepared_stmt.h
pfs_program.h
pfs_server.h
pfs_setup_actor.h
pfs_setup_object.h
pfs_stat.h
pfs_stat.cc
pfs_status.h
pfs_timer.h
pfs_tls_channel.h
pfs_user.h
pfs_variable.h
pfs_visitor.h
mysql_server_telemetry_logs_service_imp.h
mysql_server_telemetry_logs_client_service_imp.h
mysql_server_telemetry_metrics_service_imp.h
mysql_server_telemetry_traces_service_imp.h
table_accounts.h
table_all_instr.h
table_data_lock_waits.h
table_data_locks.h
table_ees_by_account_by_error.h
table_ees_by_host_by_error.h
table_ees_by_thread_by_error.h
table_ees_by_user_by_error.h
table_ees_global_by_error.h
table_error_log.h
table_esgs_by_account_by_event_name.h
table_esgs_by_host_by_event_name.h
table_esgs_by_thread_by_event_name.h
table_esgs_by_user_by_event_name.h
table_esgs_global_by_event_name.h
table_esmh_by_digest.h
table_esmh_global.h
table_esms_by_account_by_event_name.h
table_esms_by_digest.h
table_esms_by_host_by_event_name.h
table_esms_by_program.h
table_esms_by_thread_by_event_name.h
table_esms_by_user_by_event_name.h
table_esms_global_by_event_name.h
table_ets_by_account_by_event_name.h
table_ets_by_host_by_event_name.h
table_ets_by_thread_by_event_name.h
table_ets_by_user_by_event_name.h
table_ets_global_by_event_name.h
table_events_stages.h
table_events_statements.h
table_events_transactions.h
table_events_waits.h
table_events_waits_summary.h
table_ews_by_account_by_event_name.h
table_ews_by_host_by_event_name.h
table_ews_by_thread_by_event_name.h
table_ews_by_user_by_event_name.h
table_ews_global_by_event_name.h
table_file_instances.h
table_file_summary_by_event_name.h
table_file_summary_by_instance.h
table_global_status.h
table_global_variables.h
table_helper.h
table_host_cache.h
table_hosts.h
table_keyring_component_status.h
table_log_status.h
table_md_locks.h
table_mems_by_account_by_event_name.h
table_mems_by_host_by_event_name.h
table_mems_by_thread_by_event_name.h
table_mems_by_user_by_event_name.h
table_mems_global_by_event_name.h
table_os_global_by_type.h
table_performance_timers.h
table_persisted_variables.h
table_plugin_table.h
table_prepared_stmt_instances.h
table_processlist.h
table_replication_applier_configuration.h
table_replication_applier_filters.h
table_replication_applier_global_filters.h
table_replication_applier_status.h
table_replication_applier_status_by_coordinator.h
table_replication_applier_status_by_worker.h
table_replication_connection_configuration.h
table_replication_connection_status.h
table_replication_group_member_stats.h
table_replication_group_members.h
table_session_account_connect_attrs.h
table_session_connect.h
table_session_connect_attrs.h
table_session_status.h
table_session_variables.h
table_setup_actors.h
table_setup_consumers.h
table_setup_instruments.h
table_setup_loggers.h
table_setup_meters.h
table_setup_metrics.h
table_setup_objects.h
table_setup_threads.h
table_socket_instances.h
table_socket_summary_by_event_name.h
table_socket_summary_by_instance.h
table_status_by_account.h
table_status_by_host.h
table_status_by_thread.h
table_status_by_user.h
table_sync_instances.h
table_table_handles.h
table_threads.h
table_tiws_by_index_usage.h
table_tiws_by_table.h
table_tls_channel_status.h
table_tlws_by_table.h
table_user_defined_functions.h
table_users.h
table_uvar_by_thread.h
table_variables_by_thread.h
table_variables_info.h
table_variables_metadata.h
table_global_variable_attributes.h
cursor_by_thread_connect_attr.h
table_session_connect.h
table_session_connect_attrs.h
table_session_account_connect_attrs.h
table_replication_asynchronous_connection_failover.h
table_replication_connection_configuration.h
table_replication_group_members.h
table_replication_connection_status.h
table_replication_applier_configuration.h
table_replication_applier_status.h
table_replication_applier_status_by_coordinator.h
table_replication_applier_status_by_worker.h
table_replication_group_member_stats.h
table_rpl_async_connection_failover_managed.h
table_log_status.h
table_plugin_table.h
table_replication_applier_filters.h
table_replication_applier_global_filters.h
telemetry_pfs_metrics.h
cursor_by_account.cc
cursor_by_error_log.cc
cursor_by_host.cc
cursor_by_thread.cc
cursor_by_thread_connect_attr.cc
cursor_by_user.cc
ha_perfschema.cc
terminology_use_previous.cc
pfs.cc
pfs_account.cc
pfs_autosize.cc
pfs_buffer_container.cc
pfs_builtin_memory.cc
pfs_column_types.cc
pfs_column_values.cc
pfs_con_slice.cc
pfs_data_lock.cc
pfs_defaults.cc
pfs_digest.cc
pfs_engine_table.cc
pfs_error.cc
pfs_events_stages.cc
pfs_events_statements.cc
pfs_events_transactions.cc
pfs_events_waits.cc
pfs_global.cc
pfs_histogram.cc
pfs_host.cc
pfs_instr.cc
pfs_instr_class.cc
pfs_memory.cc
pfs_metrics_service_imp.cc
pfs_name.cc
pfs_plugin_table.cc
pfs_prepared_stmt.cc
pfs_program.cc
pfs_server.cc
pfs_setup_actor.cc
pfs_setup_object.cc
pfs_status.cc
pfs_timer.cc
pfs_tls_channel.cc
pfs_user.cc
pfs_variable.cc
pfs_visitor.cc
mysql_server_telemetry_logs_service_imp.cc
mysql_server_telemetry_logs_client_service_imp.cc
mysql_server_telemetry_metrics_service_imp.cc
mysql_server_telemetry_traces_service_imp.cc
service_pfs_notification.cc
service_pfs_resource_group.cc
table_accounts.cc
table_all_instr.cc
table_binary_log_transaction_compression_stats.cc
table_data_lock_waits.cc
table_data_locks.cc
table_ees_by_account_by_error.cc
table_ees_by_host_by_error.cc
table_ees_by_thread_by_error.cc
table_ees_by_user_by_error.cc
table_ees_global_by_error.cc
table_error_log.cc
table_esgs_by_account_by_event_name.cc
table_esgs_by_host_by_event_name.cc
table_esgs_by_thread_by_event_name.cc
table_esgs_by_user_by_event_name.cc
table_esgs_global_by_event_name.cc
table_esmh_by_digest.cc
table_esmh_global.cc
table_esms_by_account_by_event_name.cc
table_esms_by_digest.cc
table_esms_by_host_by_event_name.cc
table_esms_by_program.cc
table_esms_by_thread_by_event_name.cc
table_esms_by_user_by_event_name.cc
table_esms_global_by_event_name.cc
table_ets_by_account_by_event_name.cc
table_ets_by_host_by_event_name.cc
table_ets_by_thread_by_event_name.cc
table_ets_by_user_by_event_name.cc
table_ets_global_by_event_name.cc
table_events_stages.cc
table_events_statements.cc
table_events_transactions.cc
table_events_waits.cc
table_events_waits_summary.cc
table_ews_by_account_by_event_name.cc
table_ews_by_host_by_event_name.cc
table_ews_by_thread_by_event_name.cc
table_ews_by_user_by_event_name.cc
table_ews_global_by_event_name.cc
table_file_instances.cc
table_file_summary_by_event_name.cc
table_file_summary_by_instance.cc
table_global_status.cc
table_global_variables.cc
table_helper.cc
table_host_cache.cc
table_hosts.cc
table_keyring_component_status.cc
table_keyring_keys.cc
table_log_status.cc
table_md_locks.cc
table_mems_by_account_by_event_name.cc
table_mems_by_host_by_event_name.cc
table_mems_by_thread_by_event_name.cc
table_mems_by_user_by_event_name.cc
table_mems_global_by_event_name.cc
table_os_global_by_type.cc
table_performance_timers.cc
table_persisted_variables.cc
table_plugin_table.cc
table_prepared_stmt_instances.cc
table_processlist.cc
table_replication_applier_configuration.cc
table_replication_applier_filters.cc
table_replication_applier_global_filters.cc
table_replication_applier_status.cc
table_replication_applier_status_by_coordinator.cc
table_replication_applier_status_by_worker.cc
table_replication_connection_configuration.cc
table_replication_connection_status.cc
table_replication_group_member_stats.cc
table_replication_group_members.cc
table_session_account_connect_attrs.cc
table_session_connect.cc
table_session_connect_attrs.cc
table_session_status.cc
table_session_variables.cc
table_setup_actors.cc
table_setup_consumers.cc
table_setup_instruments.cc
table_setup_loggers.cc
table_setup_meters.cc
table_setup_metrics.cc
table_setup_objects.cc
table_setup_threads.cc
table_socket_instances.cc
table_socket_summary_by_event_name.cc
table_socket_summary_by_instance.cc
table_status_by_account.cc
table_status_by_host.cc
table_status_by_thread.cc
table_status_by_user.cc
table_sync_instances.cc
table_table_handles.cc
table_threads.cc
table_tiws_by_index_usage.cc
table_tiws_by_table.cc
table_tls_channel_status.cc
table_tlws_by_table.cc
table_user_defined_functions.cc
table_users.cc
table_uvar_by_thread.cc
table_variables_by_thread.cc
table_variables_info.cc
table_variables_metadata.cc
table_global_variable_attributes.cc
cursor_by_thread_connect_attr.cc
table_session_connect.cc
table_session_connect_attrs.cc
table_session_account_connect_attrs.cc
table_replication_asynchronous_connection_failover.cc
table_replication_connection_configuration.cc
table_replication_group_members.cc
table_replication_connection_status.cc
table_replication_applier_configuration.cc
table_replication_applier_status.cc
table_replication_applier_status_by_coordinator.cc
table_replication_applier_status_by_worker.cc
table_replication_group_member_stats.cc
table_rpl_async_connection_failover_managed.cc
table_log_status.cc
table_plugin_table.cc
table_replication_applier_filters.cc
table_replication_applier_global_filters.cc
table_binary_log_transaction_compression_stats.cc
telemetry_pfs_metrics.cc
)
MYSQL_ADD_PLUGIN(perfschema ${PERFSCHEMA_SOURCES}
  STORAGE_ENGINE MANDATORY STATIC_ONLY
  LINK_LIBRARIES extra::rapidjson ext::zlib mysql_gtid extra::unordered_dense
  )
IF(WITH_UNIT_TESTS)
  ADD_SUBDIRECTORY(unittest)
ENDIF()

# The top level CMakeLists.txt has an option DISABLE_ALL_PSI, default OFF.
# If it is ON, then disable everything below.

# Only disable threads when building without *any* instrumentation,
# as other instrumentations have a dependency on threads.
OPTION(DISABLE_PSI_THREAD "Exclude the performance schema thread instrumentation" ${DISABLE_ALL_PSI})

OPTION(DISABLE_PSI_MUTEX "Exclude the performance schema mutex instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_RWLOCK "Exclude the performance schema rwlock instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_COND "Exclude the performance schema condition instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_FILE "Exclude the performance schema file instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_TABLE "Exclude the performance schema table instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_SOCKET "Exclude the performance schema socket instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_STAGE "Exclude the performance schema stage instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_STATEMENT "Exclude the performance schema statement instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_SP "Exclude the performance schema stored procedure instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_PS "Exclude the performance schema prepared statements instances instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_IDLE "Exclude the performance schema idle instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_STATEMENT_DIGEST "Exclude the performance schema statement digest instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_METADATA "Exclude the performance schema metadata instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_MEMORY "Exclude the performance schema memory instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_TRANSACTION "Exclude the performance schema transaction instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_ERROR "Exclude the performance schema server error instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_DATA_LOCK "Exclude the performance schema data lock instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_TLS_CHANNEL "Exclude the performance schema tls channel instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_SERVER_TELEMETRY_TRACES "Exclude the performance schema server telemetry traces instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_SERVER_TELEMETRY_LOGS "Exclude the performance schema server telemetry logs instrumentation" ${DISABLE_ALL_PSI})
OPTION(DISABLE_PSI_METRICS "Exclude the performance schema server telemetry metrics instrumentation" ${DISABLE_ALL_PSI})

MARK_AS_ADVANCED(DISABLE_PSI_THREAD)

MARK_AS_ADVANCED(DISABLE_PSI_MUTEX)
MARK_AS_ADVANCED(DISABLE_PSI_RWLOCK)
MARK_AS_ADVANCED(DISABLE_PSI_COND)
MARK_AS_ADVANCED(DISABLE_PSI_FILE)
MARK_AS_ADVANCED(DISABLE_PSI_TABLE)
MARK_AS_ADVANCED(DISABLE_PSI_SOCKET)
MARK_AS_ADVANCED(DISABLE_PSI_STAGE)
MARK_AS_ADVANCED(DISABLE_PSI_STATEMENT)
MARK_AS_ADVANCED(DISABLE_PSI_SP)
MARK_AS_ADVANCED(DISABLE_PSI_PS)
MARK_AS_ADVANCED(DISABLE_PSI_IDLE)
MARK_AS_ADVANCED(DISABLE_PSI_STATEMENT_DIGEST)
MARK_AS_ADVANCED(DISABLE_PSI_METADATA)
MARK_AS_ADVANCED(DISABLE_PSI_MEMORY)
MARK_AS_ADVANCED(DISABLE_PSI_TRANSACTION)
MARK_AS_ADVANCED(DISABLE_PSI_ERROR)
MARK_AS_ADVANCED(DISABLE_PSI_DATA_LOCK)
MARK_AS_ADVANCED(DISABLE_PSI_TLS_CHANNEL)
MARK_AS_ADVANCED(DISABLE_PSI_SERVER_TELEMETRY_TRACES)
MARK_AS_ADVANCED(DISABLE_PSI_SERVER_TELEMETRY_LOGS)
MARK_AS_ADVANCED(DISABLE_PSI_METRICS)

