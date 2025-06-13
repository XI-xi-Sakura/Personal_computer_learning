/*
  Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_AUTH_HANDLER_FACTORY_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_AUTH_HANDLER_FACTORY_H_

#include "collector/mysql_cache_manager.h"
#include "mrs/database/entry/auth_app.h"
#include "mrs/http/session_manager.h"
#include "mrs/interface/auth_handler_factory.h"
#include "mrs/interface/authorize_handler.h"
#include "mrs/interface/query_factory.h"

namespace mrs {
namespace authentication {

class AuthHandlerFactory : public interface::AuthHandlerFactory {
 public:
  using MysqlCacheManager = collector::MysqlCacheManager;
  using QueryFactory = mrs::interface::QueryFactory;
  using SessionManager = http::SessionManager;

 public:
  AuthHandlerFactory(QueryFactory *qf);

  AuthHandlerPtr create_basic_auth_handler(
      AuthorizeHandlerCallbakcs *cb, const AuthApp &entry,
      MysqlCacheManager *cache_manager) const override;
  AuthHandlerPtr create_facebook_auth_handler(
      AuthorizeHandlerCallbakcs *cb, const AuthApp &entry) const override;
  AuthHandlerPtr create_google_auth_handler(
      AuthorizeHandlerCallbakcs *cb, const AuthApp &entry) const override;
  AuthHandlerPtr create_oidc_auth_handler(AuthorizeHandlerCallbakcs *cb,
                                          const AuthApp &entry) const override;
  AuthHandlerPtr create_scram_auth_handler(
      AuthorizeHandlerCallbakcs *cb, const AuthApp &entry,
      const std::string &rd, SessionManager *session_manager) const override;

 private:
  QueryFactory *qf_;
};

}  // namespace authentication
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_AUTH_HANDLER_FACTORY_H_
