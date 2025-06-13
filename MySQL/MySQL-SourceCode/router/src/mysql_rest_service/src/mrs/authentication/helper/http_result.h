/*
  Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_HELPER_HTTP_RESULT_H_
#define ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_HELPER_HTTP_RESULT_H_

#include <map>
#include <string>

#include "http/base/status_code.h"
#include "mrs/interface/http_result.h"

namespace mrs {
namespace authentication {

using MapObject = std::map<std::string, std::string>;

mrs::interface::HttpResult get_problem_description(
    ::http::base::status_code::key_type status, const std::string &msg,
    MapObject o = MapObject());

mrs::interface::HttpResult get_problem_description(
    ::http::base::status_code::key_type status);

}  // namespace authentication
}  // namespace mrs

#endif  // ROUTER_SRC_REST_MRS_SRC_MRS_AUTHENTICATION_HELPER_HTTP_RESULT_H_
