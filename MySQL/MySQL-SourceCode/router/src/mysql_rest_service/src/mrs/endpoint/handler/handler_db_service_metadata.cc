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

#include "mrs/endpoint/handler/handler_db_service_metadata.h"

#include "mrs/endpoint/handler/helper/url_paths.h"
#include "mrs/endpoint/handler/helper/utilities.h"
#include "mrs/endpoint/handler/helper/utils_proto.h"
#include "mrs/http/error.h"

namespace mrs {
namespace endpoint {
namespace handler {

using HttpResult = mrs::rest::Handler::HttpResult;
using Authorization = mrs::rest::Handler::Authorization;

namespace {

auto get_regex_path_service_metadata(
    std::weak_ptr<DbServiceEndpoint> endpoint) {
  using namespace std::string_literals;

  auto endpoint_srvc = lock(endpoint);
  if (!endpoint_srvc) return ""s;

  return regex_path_service_metadata(endpoint_srvc->get_url_path());
}

}  // namespace

HandlerDbServiceMetadata::HandlerDbServiceMetadata(
    std::weak_ptr<DbServiceEndpoint> endpoint,
    mrs::interface::AuthorizeManager *auth_manager)
    : Handler(handler::get_protocol(endpoint), get_endpoint_host(endpoint),
              /*regex-path: ^/service/_metadata$*/
              {get_regex_path_service_metadata(endpoint)},
              get_endpoint_options(lock(endpoint)), auth_manager),
      endpoint_{endpoint} {
  auto ep = lock(endpoint_);
  entry_ = ep->get();
}

HttpResult HandlerDbServiceMetadata::handle_get(rest::RequestContext *) {
  auto endpoint = lock_or_throw_unavail(endpoint_);

  return entry_->metadata ? std::string(*entry_->metadata) : "{}";
}

HttpResult HandlerDbServiceMetadata::handle_post(
    [[maybe_unused]] rest::RequestContext *ctxt,
    [[maybe_unused]] const std::vector<uint8_t> &document) {
  assert(false && "Should be never called. Stubbing parent class.");
  throw http::Error(HttpStatusCode::Forbidden);
}

HttpResult HandlerDbServiceMetadata::handle_delete(
    [[maybe_unused]] rest::RequestContext *ctxt) {
  assert(false && "Should be never called. Stubbing parent class.");
  throw http::Error(HttpStatusCode::Forbidden);
}

HttpResult HandlerDbServiceMetadata::handle_put(
    [[maybe_unused]] rest::RequestContext *ctxt) {
  assert(false && "Should be never called. Stubbing parent class.");
  throw http::Error(HttpStatusCode::Forbidden);
}

uint32_t HandlerDbServiceMetadata::get_access_rights() const {
  using Operation = mrs::database::entry::Operation;

  return Operation::valueRead;
}

Authorization HandlerDbServiceMetadata::requires_authentication() const {
  return Authorization::kNotNeeded;
}

UniversalId HandlerDbServiceMetadata::get_service_id() const {
  return entry_->id;
}

UniversalId HandlerDbServiceMetadata::get_schema_id() const { return {}; }

UniversalId HandlerDbServiceMetadata::get_db_object_id() const { return {}; }

const std::string &HandlerDbServiceMetadata::get_service_path() const {
  return entry_->url_context_root;
}

const std::string &HandlerDbServiceMetadata::get_schema_path() const {
  return empty_path();
}

const std::string &HandlerDbServiceMetadata::get_db_object_path() const {
  return empty_path();
}

}  // namespace handler
}  // namespace endpoint
}  // namespace mrs
