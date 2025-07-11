/* Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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
#ifndef DYNAMIC_PRIVILEGES_IMPL_H
#define DYNAMIC_PRIVILEGES_IMPL_H
#include <mysql/components/service.h>
#include <mysql/components/service_implementation.h>
#include <stddef.h>

DEFINE_SERVICE_HANDLE(Security_context_handle);

/**
  Interface implementation for registering and checking global dynamic
  privileges.
*/
class dynamic_privilege_services_impl {
 public:
  static DEFINE_BOOL_METHOD(register_privilege, (const char *privilege_str,
                                                 size_t privilege_str_len));

  static DEFINE_BOOL_METHOD(unregister_privilege, (const char *privilege_str,
                                                   size_t privilege_str_len));

  /**
    Check if the supplied security context has the specified privilege
    identifier granted to it.
    @return
       @retval true The privilege was granted.
       @retval false Access is defined - no such privilege.
  */
  static DEFINE_BOOL_METHOD(has_global_grant,
                            (Security_context_handle, const char *privilege_str,
                             size_t privilege_str_len));

  static DEFINE_BOOL_METHOD(add_deprecated,
                            (const char *priv_name, size_t priv_name_len));

  static DEFINE_BOOL_METHOD(remove_deprecated,
                            (const char *priv_name, size_t priv_name_len));
};
bool dynamic_privilege_init(void);
#endif /* MYSQL_SERVER_DYNAMIC_LOADER_PATH_FILTER_H */
