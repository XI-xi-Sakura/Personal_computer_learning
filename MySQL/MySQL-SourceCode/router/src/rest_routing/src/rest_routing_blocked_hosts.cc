/*
  Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include "rest_routing_blocked_hosts.h"

#ifdef RAPIDJSON_NO_SIZETYPEDEFINE
#include "my_rapidjson_size_t.h"
#endif

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include "mysqlrouter/rest_api_utils.h"
#include "mysqlrouter/routing_component.h"

bool RestRoutingBlockedHosts::on_handle_request(
    http::base::Request &req, const std::string & /* base_path */,
    const std::vector<std::string> &path_matches) {
  if (!ensure_no_params(req)) return true;

  MySQLRoutingAPI inst =
      MySQLRoutingComponent::get_instance().api(path_matches[1]);

  if (!inst) {
    send_rfc7807_not_found_error(req);
    return true;
  }

  auto &out_hdrs = req.get_output_headers();
  out_hdrs.add("Content-Type", "application/json");

#if 0
  // handle If-Modified-Since

  last_modified_ = ::time(nullptr);

  if (!ensure_modified_since(req, last_modified_)) return true;
#endif

  rapidjson::Document json_doc;
  {
    rapidjson::Document::AllocatorType &allocator = json_doc.GetAllocator();

    rapidjson::Value el(rapidjson::kArrayType);

    MySQLRoutingAPI inst =
        MySQLRoutingComponent::get_instance().api(path_matches[1]);

    for (const auto &client_addr : inst.get_blocked_client_hosts()) {
      el.PushBack(
          rapidjson::Value(client_addr.c_str(), client_addr.size(), allocator)
              .Move(),
          allocator);
    }

    json_doc.SetObject().AddMember("items", el, allocator);
  }
  send_json_document(req, HttpStatusCode::Ok, json_doc);

  return true;
}
