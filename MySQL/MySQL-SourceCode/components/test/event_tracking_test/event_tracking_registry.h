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

#ifndef COMPONENTS_TEST_EVENT_TRACKING_REGISTRY_H
#define COMPONENTS_TEST_EVENT_TRACKING_REGISTRY_H

#include "mysql/components/services/dynamic_loader.h"
#include "mysql/components/services/registry.h"

using registry_type_t = SERVICE_TYPE_NO_CONST(registry);
using dynamic_loader_type_t = SERVICE_TYPE_NO_CONST(dynamic_loader);

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus
void init_registry();
void deinit_registry();
registry_type_t *get_service_registry();
dynamic_loader_type_t *get_dynamic_loader();
#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // !COMPONENTS_TEST_EVENT_TRACKING_REGISTRY_H
