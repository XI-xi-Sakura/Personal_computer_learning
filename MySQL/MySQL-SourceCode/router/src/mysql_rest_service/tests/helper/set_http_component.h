/*
  Copyright (c) 2022, 2024, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_TESTS_HELPER_SET_HTTP_COMPONENT_H_
#define ROUTER_SRC_REST_MRS_TESTS_HELPER_SET_HTTP_COMPONENT_H_

#include <memory>

#include "mysqlrouter/component/http_server_component.h"

namespace helper {

class SetHttpComponent {
 private:
  class HttpWrapperHttpServerComponent : public HttpServerComponent {
   public:
    HttpWrapperHttpServerComponent(HttpServerComponent *other);
    void *add_route(
        const std::string &url_host, const std::string &url_regex,
        std::unique_ptr<http::base::RequestHandler,
                        std::default_delete<http::base::RequestHandler>>
            cb) override;
    void remove_route(const void *handler) override;
    void remove_route(const std::string &url_host,
                      const std::string &url_regex) override;
    void init(HttpServerCtxtPtr srv) override;
    bool is_ssl_configured() override;

    HttpServerComponent *other_;
  };

 public:
  SetHttpComponent(HttpServerComponent *component);
  ~SetHttpComponent();
};

}  // namespace helper

#endif  // ROUTER_SRC_REST_MRS_TESTS_HELPER_SET_HTTP_COMPONENT_H_
