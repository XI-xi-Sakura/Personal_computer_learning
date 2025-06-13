/*
  Copyright (c) 2023, 2024 Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef ROUTER_TESTS_MRS_CLIENT_BASIC_AUTHENTICATION_H_
#define ROUTER_TESTS_MRS_CLIENT_BASIC_AUTHENTICATION_H_

#include "client/http_client_request.h"
#include "client/session_type.h"

namespace mrs_client {

class Authentication {
 public:
  Result do_basic_flow(HttpClientRequest *request, std::string url,
                       const std::string &user, const std::string &password,
                       const SessionType st,
                       const std::optional<std::string> &auth_app);
  Result do_basic_json_flow(HttpClientRequest *request, std::string url,
                            const std::string &user,
                            const std::string &password, const SessionType st,
                            const std::optional<std::string> &auth_app);
  Result do_scram_get_flow(HttpClientRequest *request, std::string url,
                           const std::string &user, const std::string &password,
                           const SessionType st,
                           const std::optional<std::string> &auth_app);
  Result do_scram_post_flow(HttpClientRequest *request, std::string url,
                            const std::string &user,
                            const std::string &password, const SessionType st,
                            const std::optional<std::string> &auth_app);
};

}  // namespace mrs_client

#endif  // ROUTER_TESTS_MRS_CLIENT_BASIC_AUTHENTICATION_H_
