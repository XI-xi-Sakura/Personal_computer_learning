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

#include "rest_connection_pool_status.h"

#include <ctime>

#ifdef RAPIDJSON_NO_SIZETYPEDEFINE
#include "my_rapidjson_size_t.h"
#endif

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include "mysqlrouter/connection_pool.h"
#include "mysqlrouter/connection_pool_component.h"
#include "mysqlrouter/rest_api_utils.h"

bool RestConnectionPoolStatus::on_handle_request(
    http::base::Request &req, const std::string & /* base_path */,
    const std::vector<std::string> &path_matches) {
  if (!ensure_no_params(req)) return true;

  auto inst = ConnectionPoolComponent::get_instance().get(path_matches[1]);

  if (!inst) {
    send_rfc7807_not_found_error(req);
    return true;
  }

  auto &out_hdrs = req.get_output_headers();
  out_hdrs.add("Content-Type", "application/json");

  rapidjson::Document json_doc;
  {
    rapidjson::Document::AllocatorType &allocator = json_doc.GetAllocator();

    json_doc.SetObject()
        .AddMember<uint64_t>("idleServerConnections",
                             inst->current_pooled_connections(), allocator)
        .AddMember<uint64_t>("stashedServerConnections",
                             inst->current_stashed_connections(), allocator);
  }
  send_json_document(req, HttpStatusCode::Ok, json_doc);

  return true;
}
