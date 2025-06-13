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

#ifndef ROUTER_SRC_MYSQL_REST_SERVICE_SRC_MRS_ENDPOINT_HANDLER_AUTHENTICATION_AUTHORIZE_COMPLETED_H_
#define ROUTER_SRC_MYSQL_REST_SERVICE_SRC_MRS_ENDPOINT_HANDLER_AUTHENTICATION_AUTHORIZE_COMPLETED_H_

#include <optional>
#include <string>
#include <vector>

#include "helper/media_type.h"
#include "mrs/endpoint/handler/authentication/handler_authorize_base.h"
#include "mrs/interface/authorize_manager.h"

namespace mrs {
namespace endpoint {
namespace handler {

class HandlerAuthorizeCompleted : public HandlerAuthorizeBase {
 public:
  HandlerAuthorizeCompleted(const Protocol protocol,
                            const std::string &url_host,
                            const UniversalId service_id,
                            const std::string &service_path,
                            const std::string &rest_path_matcher,
                            const std::string &options,
                            const std::string &page_content_custom,
                            interface::AuthorizeManager *auth_manager);

  Authorization requires_authentication() const override;
  bool may_check_access() const override;
  UniversalId get_service_id() const override;
  UniversalId get_db_object_id() const override;
  UniversalId get_schema_id() const override;
  const std::string &get_service_path() const override;
  const std::string &get_schema_path() const override;
  const std::string &get_db_object_path() const override;
  uint32_t get_access_rights() const override;

  HttpResult handle_get(RequestContext *ctxt) override;
  HttpResult handle_post(RequestContext *ctxt,
                         const std::vector<uint8_t> &document) override;
  HttpResult handle_delete(RequestContext *ctxt) override;
  HttpResult handle_put(RequestContext *ctxt) override;

 private:
  UniversalId service_id_;
  const std::string service_path_;
  const std::string page_content_custom_;
  std::string copy_url_;
  std::string copy_path_;
};

}  // namespace handler
}  // namespace endpoint
}  // namespace mrs

#endif  // ROUTER_SRC_MYSQL_REST_SERVICE_SRC_MRS_ENDPOINT_HANDLER_AUTHENTICATION_AUTHORIZE_COMPLETED_H_
