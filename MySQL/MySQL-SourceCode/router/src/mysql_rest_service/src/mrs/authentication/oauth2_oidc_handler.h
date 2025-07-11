/*
 Copyright (c) 2024, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_OAUTH2_OIDC_HANDLER_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_OAUTH2_OIDC_HANDLER_H_

#include "mrs/authentication/oauth2_handler.h"
#include "mrs/database/entry/auth_app.h"
#include "mrs/http/session_manager.h"
#include "mrs/users/user_manager.h"

namespace mrs {
namespace authentication {

class Oauth2OidcHandler : public Oauth2Handler {
  using SessionManager = http::SessionManager;
  using UserManager = users::UserManager;
  using SessionKeyType = std::string;
  class SessionData;

  class RequestHandlerJsonSimpleObjectWithBearer
      : public RequestHandlerJsonSimpleObject {
   public:
    RequestHandlerJsonSimpleObjectWithBearer(OutJsonObjectKeyValues output,
                                             std::string token)
        : RequestHandlerJsonSimpleObject(output), token_{token} {}

    void before_send(Request *request) override;

   private:
    std::string token_;
  };

 public:
  Oauth2OidcHandler(const AuthApp &entry, QueryFactory *qf);
  ~Oauth2OidcHandler() override;

  const std::string &get_handler_name() const override;

 private:
  std::string get_url_direct_auth() const override;
  std::string get_url_location(GenericSessionData *data,
                               Url *url) const override;
  std::string get_url_validation(GenericSessionData *data) const override;

  std::string get_body_access_token_request(
      GenericSessionData *session_data) const override;

  RequestHandlerPtr get_request_handler_access_token(
      GenericSessionData *session_data) override;
  RequestHandlerPtr get_request_handler_verify_account(
      Session *session, GenericSessionData *session_data) override;
};

}  // namespace authentication
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_OAUTH2_OIDC_HANDLER_H_
