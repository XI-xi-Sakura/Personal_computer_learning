# Copyright (c) 2017, 2025, Oracle and/or its affiliates.
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

SET (DEB_RULES_DEBUG_CMAKE
"
	mkdir debug && cd debug && \\
	cmake .. \\
		-DBUILD_CONFIG=mysql_release \\
		-DCMAKE_INSTALL_PREFIX=/usr \\
		-DCMAKE_BUILD_TYPE=Debug \\
		-DMYSQL_MAINTAINER_MODE=0 \\
		-DINSTALL_DOCDIR=share/mysql/docs \\
		-DINSTALL_LIBDIR=lib/$(DEB_HOST_MULTIARCH) \\
		-DSYSCONFDIR=/etc/mysql \\
		-DMYSQL_UNIX_ADDR=/var/run/mysqld/mysqld.sock \\
		-DWITH_MECAB=system \\
		-DWITH_ZLIB=${DEB_ZLIB_OPTION} \\
		-DWITH_NUMA=ON \\
		-DCOMPILATION_COMMENT=\"MySQL ${DEB_PRODUCTNAMEC} - ${DEB_LICENSENAME} - Debug\" \\
		-DCOMPILATION_COMMENT_SERVER=\"MySQL ${DEB_PRODUCTNAMEC} Server - ${DEB_LICENSENAME} - Debug\" \\
		-DINSTALL_LAYOUT=DEB \\
		-DREPRODUCIBLE_BUILD=OFF \\
		-DMYSQL_MAINTAINER_MODE=0 \\
		-DDEB_PRODUCT=${DEB_PRODUCT} \\
		${DEB_CMAKE_EXTRAS}
")

SET (DEB_RULES_DEBUG_MAKE
"
	cd debug && \\
	$(MAKE) $(JOBS) VERBOSE=1
")

SET (DEB_RULES_DEBUG_EXTRA
"
	# The ini file isn't built for debug, so copy over from the standard build
	install -g root -o root -m 0755 debian/tmp/usr/lib/mysql/plugin/daemon_example.ini debian/tmp/usr/lib/mysql/plugin/debug
")

SET (DEB_INSTALL_DEBUG_SERVER
"
# debug binary
usr/sbin/mysqld-debug
")

SET (DEB_INSTALL_DEBUG_SERVER_PLUGINS
"
# debug plugins
usr/lib/mysql/plugin/debug/adt_null.so
usr/lib/mysql/plugin/debug/auth_socket.so
usr/lib/mysql/plugin/debug/component_log_filter_dragnet.so
usr/lib/mysql/plugin/debug/component_log_sink_json.so
usr/lib/mysql/plugin/debug/component_log_sink_syseventlog.so
usr/lib/mysql/plugin/debug/component_mysqlbackup.so
usr/lib/mysql/plugin/debug/component_validate_password.so
usr/lib/mysql/plugin/debug/component_query_attributes.so
usr/lib/mysql/plugin/debug/component_reference_cache.so
usr/lib/mysql/plugin/debug/component_connection_control.so
usr/lib/mysql/plugin/debug/ddl_rewriter.so
usr/lib/mysql/plugin/debug/group_replication.so
usr/lib/mysql/plugin/debug/connection_control.so
usr/lib/mysql/plugin/debug/keyring_udf.so
usr/lib/mysql/plugin/debug/libpluginmecab.so
usr/lib/mysql/plugin/debug/locking_service.so
usr/lib/mysql/plugin/debug/mypluglib.so
usr/lib/mysql/plugin/debug/mysql_clone.so
usr/lib/mysql/plugin/debug/mysql_no_login.so
usr/lib/mysql/plugin/debug/rewriter.so
usr/lib/mysql/plugin/debug/semisync_master.so
usr/lib/mysql/plugin/debug/semisync_slave.so
usr/lib/mysql/plugin/debug/semisync_source.so
usr/lib/mysql/plugin/debug/semisync_replica.so
usr/lib/mysql/plugin/debug/validate_password.so
usr/lib/mysql/plugin/debug/component_audit_api_message_emit.so
usr/lib/mysql/plugin/debug/component_keyring_file.so
")

SET (DEB_INSTALL_DEBUG_TEST_PLUGINS
"
usr/lib/mysql/plugin/debug/auth.so
usr/lib/mysql/plugin/debug/auth_test_plugin.so
usr/lib/mysql/plugin/debug/authentication_ldap_sasl_client.so
usr/lib/mysql/plugin/debug/authentication_webauthn_client.so
usr/lib/mysql/plugin/debug/authentication_kerberos_client.so
usr/lib/mysql/plugin/debug/authentication_openid_connect_client.so
usr/lib/mysql/plugin/debug/authentication_oci_client.so
usr/lib/mysql/plugin/debug/mysql_native_password.so
usr/lib/mysql/plugin/debug/component_example_component1.so
usr/lib/mysql/plugin/debug/component_example_component2.so
usr/lib/mysql/plugin/debug/component_example_component3.so
usr/lib/mysql/plugin/debug/component_log_sink_test.so
usr/lib/mysql/plugin/debug/component_test_execute_prepared_statement.so
usr/lib/mysql/plugin/debug/component_test_execute_regular_statement.so
usr/lib/mysql/plugin/debug/component_test_mysql_signal_handler.so
usr/lib/mysql/plugin/debug/component_test_string_service.so
usr/lib/mysql/plugin/debug/component_test_string_service_charset.so
usr/lib/mysql/plugin/debug/component_test_string_service_long.so
usr/lib/mysql/plugin/debug/component_test_pfs_notification.so
usr/lib/mysql/plugin/debug/component_test_pfs_resource_group.so
usr/lib/mysql/plugin/debug/component_test_udf_registration.so
usr/lib/mysql/plugin/debug/component_test_host_application_signal.so
usr/lib/mysql/plugin/debug/component_test_mysql_current_thread_reader.so
usr/lib/mysql/plugin/debug/component_test_mysql_runtime_error.so
usr/lib/mysql/plugin/debug/component_test_component_deinit.so
usr/lib/mysql/plugin/debug/component_test_mysql_command_services.so
usr/lib/mysql/plugin/debug/test_services_command_services.so
usr/lib/mysql/plugin/debug/component_udf_reg_3_func.so
usr/lib/mysql/plugin/debug/component_udf_reg_avg_func.so
usr/lib/mysql/plugin/debug/component_udf_reg_int_func.so
usr/lib/mysql/plugin/debug/component_udf_reg_int_same_func.so
usr/lib/mysql/plugin/debug/component_udf_reg_only_3_func.so
usr/lib/mysql/plugin/debug/component_udf_reg_real_func.so
usr/lib/mysql/plugin/debug/component_udf_unreg_3_func.so
usr/lib/mysql/plugin/debug/component_udf_unreg_int_func.so
usr/lib/mysql/plugin/debug/component_udf_unreg_real_func.so
usr/lib/mysql/plugin/debug/conflicting_variables.so
usr/lib/mysql/plugin/debug/daemon_example.ini
usr/lib/mysql/plugin/debug/ha_example.so
usr/lib/mysql/plugin/debug/ha_mock.so
usr/lib/mysql/plugin/debug/libdaemon_example.so
usr/lib/mysql/plugin/debug/libtest_framework.so
usr/lib/mysql/plugin/debug/libtest_services.so
usr/lib/mysql/plugin/debug/libtest_services_threaded.so
usr/lib/mysql/plugin/debug/libtest_session_detach.so
usr/lib/mysql/plugin/debug/libtest_session_attach.so
usr/lib/mysql/plugin/debug/libtest_session_info.so
usr/lib/mysql/plugin/debug/libtest_session_in_thd.so
usr/lib/mysql/plugin/debug/libtest_sql_2_sessions.so
usr/lib/mysql/plugin/debug/libtest_sql_all_col_types.so
usr/lib/mysql/plugin/debug/libtest_sql_cmds_1.so
usr/lib/mysql/plugin/debug/libtest_sql_commit.so
usr/lib/mysql/plugin/debug/libtest_sql_complex.so
usr/lib/mysql/plugin/debug/libtest_sql_errors.so
usr/lib/mysql/plugin/debug/libtest_sql_lock.so
usr/lib/mysql/plugin/debug/libtest_sql_processlist.so
usr/lib/mysql/plugin/debug/libtest_sql_replication.so
usr/lib/mysql/plugin/debug/libtest_sql_shutdown.so
usr/lib/mysql/plugin/debug/libtest_sql_sleep_is_connected.so
usr/lib/mysql/plugin/debug/libtest_sql_stmt.so
usr/lib/mysql/plugin/debug/libtest_sql_sqlmode.so
usr/lib/mysql/plugin/debug/libtest_sql_stored_procedures_functions.so
usr/lib/mysql/plugin/debug/libtest_sql_views_triggers.so
usr/lib/mysql/plugin/debug/libtest_sql_reset_connection.so
usr/lib/mysql/plugin/debug/libtest_x_sessions_deinit.so
usr/lib/mysql/plugin/debug/libtest_x_sessions_init.so
usr/lib/mysql/plugin/debug/qa_auth_client.so
usr/lib/mysql/plugin/debug/qa_auth_interface.so
usr/lib/mysql/plugin/debug/qa_auth_server.so
usr/lib/mysql/plugin/debug/replication_observers_example_plugin.so
usr/lib/mysql/plugin/debug/rewrite_example.so
usr/lib/mysql/plugin/debug/test_udf_services.so
usr/lib/mysql/plugin/debug/udf_example.so
usr/lib/mysql/plugin/debug/test_security_context.so
usr/lib/mysql/plugin/debug/test_services_plugin_registry.so
usr/lib/mysql/plugin/debug/test_services_host_application_signal.so
usr/lib/mysql/plugin/debug/component_test_status_var_service.so
usr/lib/mysql/plugin/debug/component_test_status_var_service_int.so
usr/lib/mysql/plugin/debug/component_test_status_var_service_reg_only.so
usr/lib/mysql/plugin/debug/component_test_status_var_service_str.so
usr/lib/mysql/plugin/debug/component_test_status_var_service_unreg_only.so
usr/lib/mysql/plugin/debug/component_test_system_variable_source.so
usr/lib/mysql/plugin/debug/component_test_sys_var_service.so
usr/lib/mysql/plugin/debug/component_test_sys_var_service_int.so
usr/lib/mysql/plugin/debug/component_test_sys_var_service_same.so
usr/lib/mysql/plugin/debug/component_pfs_example_component_population.so
usr/lib/mysql/plugin/debug/component_test_sys_var_service_str.so
usr/lib/mysql/plugin/debug/component_test_session_var_service.so
usr/lib/mysql/plugin/debug/component_test_backup_lock_service.so
usr/lib/mysql/plugin/debug/pfs_example_plugin_employee.so
usr/lib/mysql/plugin/debug/component_pfs_example.so
usr/lib/mysql/plugin/debug/component_mysqlx_global_reset.so
usr/lib/mysql/plugin/debug/component_test_audit_api_message.so
usr/lib/mysql/plugin/debug/component_test_udf_aggregate.so
usr/lib/mysql/plugin/debug/component_test_udf_services.so
usr/lib/mysql/plugin/debug/component_test_mysql_system_variable_set.so
usr/lib/mysql/plugin/debug/component_test_table_access.so
usr/lib/mysql/plugin/debug/component_test_sensitive_system_variables.so
usr/lib/mysql/plugin/debug/component_test_status_var_reader.so
usr/lib/mysql/plugin/debug/component_test_server_telemetry_logs_client.so
usr/lib/mysql/plugin/debug/component_test_server_telemetry_logs_export.so
usr/lib/mysql/plugin/debug/component_test_server_telemetry_metrics.so
usr/lib/mysql/plugin/debug/component_test_server_telemetry_traces.so
usr/lib/mysql/plugin/debug/component_test_mysql_thd_store_service.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_consumer_c.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_consumer_b.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_consumer_a.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_producer_a.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_consumer.so
usr/lib/mysql/plugin/debug/component_test_event_tracking_producer_b.so
")

IF (DEB_PRODUCT STREQUAL "commercial")
  # Add debug versions of commercial plugins, if enabled
  IF (DEFINED DEB_WITH_DEBUG)
    SET (DEB_INSTALL_DEBUG_SERVER_PLUGINS "${DEB_INSTALL_DEBUG_SERVER_PLUGINS}
usr/lib/mysql/plugin/debug/audit_log.so
usr/lib/mysql/plugin/debug/authentication_pam.so
usr/lib/mysql/plugin/debug/authentication_ldap_sasl.so
usr/lib/mysql/plugin/debug/authentication_kerberos.so
usr/lib/mysql/plugin/debug/authentication_openid_connect.so
usr/lib/mysql/plugin/debug/authentication_ldap_simple.so
usr/lib/mysql/plugin/debug/telemetry_client.so
usr/lib/mysql/plugin/debug/data_masking.so
usr/lib/mysql/plugin/debug/keyring_okv.so
usr/lib/mysql/plugin/debug/keyring_hashicorp.so
usr/lib/mysql/plugin/debug/thread_pool.so
usr/lib/mysql/plugin/debug/firewall.so
usr/lib/mysql/plugin/debug/component_keyring_encrypted_file.so
usr/lib/mysql/plugin/debug/component_keyring_hashicorp.so
usr/lib/mysql/plugin/debug/component_keyring_oci.so
usr/lib/mysql/plugin/debug/component_enterprise_encryption.so
usr/lib/mysql/plugin/debug/component_masking.so
usr/lib/mysql/plugin/debug/component_masking_functions.so
usr/lib/mysql/plugin/debug/component_mle.so
usr/lib/mysql/plugin/debug/component_scheduler.so
usr/lib/mysql/plugin/debug/component_telemetry.so
usr/lib/mysql/plugin/debug/component_option_tracker.so
usr/lib/mysql/plugin/debug/component_group_replication_flow_control_stats.so
usr/lib/mysql/plugin/debug/component_group_replication_elect_prefers_most_updated.so
usr/lib/mysql/plugin/debug/component_group_replication_resource_manager.so
usr/lib/mysql/plugin/debug/component_replication_applier_metrics.so
usr/lib/mysql/plugin/debug/authentication_webauthn.so
")
  ENDIF()

  IF (DEB_AWS_SDK)
    SET (DEB_CMAKE_EXTRAS "${DEB_CMAKE_EXTRAS} -DWITH_KEYRING_AWS=ON -DWITH_AWS_SDK=${DEB_AWS_SDK} -DDEB_AWS_SDK=${DEB_AWS_SDK}")
    SET (DEB_INSTALL_DEBUG_SERVER_PLUGINS "${DEB_INSTALL_DEBUG_SERVER_PLUGINS}
usr/lib/mysql/plugin/debug/keyring_aws.so
")

    IF (DEFINED ENV{AWS_VER})
      IF ($ENV{AWS_VER} STREQUAL "1.11")
	      SET (DEB_INSTALL_DEBUG_SERVER_PLUGINS "${DEB_INSTALL_DEBUG_SERVER_PLUGINS}
usr/lib/mysql/plugin/debug/component_keyring_aws.so
")
      ELSE()
        MESSAGE(STATUS "Unsupported AWS SDK version: $ENV{AWS_VER}, skip packaging component_keyring_aws.")
      ENDIF()
    ELSE()
      MESSAGE(STATUS "Environment variable AWS_VER not set, skip packaging component_keyring_aws.")
    ENDIF()
  ENDIF()
  SET (DEB_INSTALL_DEBUG_TEST_PLUGINS "${DEB_INSTALL_DEBUG_TEST_PLUGINS}
usr/lib/mysql/plugin/debug/component_test_global_priv_registration.so
usr/lib/mysql/plugin/debug/component_test_page_track_component.so
")

ENDIF()
SET (DEB_CONTROL_DEBUG
"
Package: mysql-${DEB_PRODUCTNAME}-server-debug
Architecture: any
Section: debug
Depends: \${misc:Depends}, mysql-${DEB_PRODUCTNAME}-server (= \${binary:Version})
Description: Debug binaries for MySQL Server

Package: mysql-${DEB_PRODUCTNAME}-test-debug
Architecture: any
Section: debug
Depends: mysql-${DEB_PRODUCTNAME}-test, \${misc:Depends}
Description: Debug binaries for MySQL Testsuite
")
