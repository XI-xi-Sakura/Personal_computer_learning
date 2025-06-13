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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_ENDPOINT_HANDLER_HANDLER_CONTENT_FILE_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_ENDPOINT_HANDLER_HANDLER_CONTENT_FILE_H_

#include <memory>
#include <string>
#include <vector>

#include "mrs/database/entry/db_service.h"
#include "mrs/endpoint/content_file_endpoint.h"
#include "mrs/endpoint/content_set_endpoint.h"
#include "mrs/endpoint/handler/persistent/persistent_data_content_file.h"
#include "mrs/interface/authorize_manager.h"
#include "mrs/interface/query_factory.h"
#include "mrs/rest/handler.h"
#include "mrs/rest/response_cache.h"

namespace mrs {
namespace endpoint {
namespace handler {

class HandlerContentFile : public mrs::rest::Handler {
 public:
  using MySQLSession = Handler::SqlSession;
  using ContentFileEndpoint = mrs::endpoint::ContentFileEndpoint;
  using DbService = mrs::database::entry::DbService;
  using QueryFactory = mrs::interface::QueryFactory;
  using ContentFilePtr = ContentFileEndpoint::ContentFilePtr;
  using ContentSetPtr = ContentSetEndpoint::ContentSetPtr;
  using DbServicePtr = std::shared_ptr<DbService>;

 public:
  HandlerContentFile(
      std::weak_ptr<ContentFileEndpoint> endpoint,
      mrs::interface::AuthorizeManager *auth_manager,
      std::shared_ptr<PersistentDataContentFile> persistent_data_content_file);

  UniversalId get_service_id() const override;
  UniversalId get_db_object_id() const override;
  UniversalId get_schema_id() const override;
  const std::string &get_service_path() const override;
  const std::string &get_schema_path() const override;
  const std::string &get_db_object_path() const override;

  uint32_t get_access_rights() const override;
  Authorization requires_authentication() const override;

  void authorization(rest::RequestContext *ctxt) override;

  HttpResult handle_delete(rest::RequestContext *ctxt) override;
  HttpResult handle_put(rest::RequestContext *ctxt) override;
  HttpResult handle_post(rest::RequestContext *ctxt,
                         const std::vector<uint8_t> &document) override;
  HttpResult handle_get(rest::RequestContext *ctxt) override;

 private:
  std::weak_ptr<ContentFileEndpoint> endpoint_;
  ContentFilePtr entry_file_;
  ContentSetPtr entry_set_;
  DbServicePtr entry_service_;
  std::string version_;
  std::shared_ptr<PersistentDataContentFile> persistent_data_content_file_;
};

}  // namespace handler
}  // namespace endpoint
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_ENDPOINT_HANDLER_HANDLER_CONTENT_FILE_H_
