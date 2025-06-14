/* Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQL_SERVER_TELEMETRY_LOGS_SERVICE_IMP_H
#define MYSQL_SERVER_TELEMETRY_LOGS_SERVICE_IMP_H

#include <mysql/components/services/mysql_server_telemetry_logs_service.h>
#include <mysql/plugin.h>

/**
  @file storage/perfschema/mysql_server_telemetry_logs_service_imp.h
  The performance schema implementation of server telemetry logs service.
*/
extern SERVICE_TYPE(mysql_server_telemetry_logs)
    SERVICE_IMPLEMENTATION(performance_schema, mysql_server_telemetry_logs);

void initialize_mysql_server_telemetry_logs_service();
void cleanup_mysql_server_telemetry_logs_service();

bool pfs_register_logger_v1(log_delivery_callback_t logger);
bool pfs_unregister_logger_v1(log_delivery_callback_t logger);
void pfs_notify_logger_v1(PSI_logger *logger, OTELLogLevel level,
                          const char *message, time_t timestamp,
                          const log_attribute_t *attr_array, size_t attr_count);

extern mysql_mutex_t LOCK_pfs_logging_callback;
#ifdef HAVE_PSI_SERVER_TELEMETRY_LOGS_INTERFACE
extern std::atomic<log_delivery_callback_t> g_telemetry_log;
#endif /* HAVE_PSI_SERVER_TELEMETRY_LOGS_INTERFACE */

#endif /* MYSQL_SERVER_TELEMETRY_LOGS_SERVICE_IMP_H */
