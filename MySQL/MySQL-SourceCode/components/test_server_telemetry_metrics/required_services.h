/* Copyright (c) 2022, 2025, Oracle and/or its affiliates.

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

#ifndef TEST_SERVER_METRICS_REQUIRED_SERVICES_INCLUDED
#define TEST_SERVER_METRICS_REQUIRED_SERVICES_INCLUDED

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/mysql_server_telemetry_metrics_service.h>
#include <mysql/components/services/psi_metric.h>
#include <mysql/components/services/udf_registration.h>
#include <mysql/psi/mysql_metric.h>

/* A place to specify component-wide declarations, including declarations of
 *   placeholders for Service dependencies. */

extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_server_telemetry_metrics_v1,
                                       metrics_v1_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(udf_registration, udf_registration_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_string_factory,
                                       string_factory_srv);
extern REQUIRES_SERVICE_PLACEHOLDER_AS(mysql_string_converter,
                                       string_converter_srv);
extern REQUIRES_PSI_METRIC_SERVICE_PLACEHOLDER;

#endif /* TEST_SERVER_METRICS_REQUIRED_SERVICES_INCLUDED */
