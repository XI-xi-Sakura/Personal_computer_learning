/* Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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

#ifndef HAVE_SECURITY_CONTEXT_WRAPPER_H
#define HAVE_SECURITY_CONTEXT_WRAPPER_H

#include <mysql/components/services/dynamic_privilege.h>
#include <mysql/components/services/log_shared.h>
#include <mysql/service_parser.h>

namespace connection_control {
class Security_context_wrapper {
 public:
  explicit Security_context_wrapper(MYSQL_THD thd);
  ~Security_context_wrapper() = default;
  const char *get_proxy_user();
  const char *get_priv_user();
  const char *get_priv_host();
  const char *get_user();
  const char *get_host();
  const char *get_ip();

 private:
  const char *get_property(const char *property);
  Security_context_handle m_sctx;
  bool m_valid = false;
};
}  // namespace connection_control
#endif  // !HAVE_SECURITY_CONTEXT_WRAPPER_H
