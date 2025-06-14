/*
  Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#include "mysqlrouter/component/http_auth_realm_component.h"

#include <memory>
#include <mutex>
#include <string>

#include "http_auth_error.h"
#include "http_auth_realm.h"

void HttpAuthRealmComponent::add_realm(const std::string &name,
                                       std::shared_ptr<HttpAuthRealm> realm) {
  std::lock_guard<std::mutex> const lk(realms_m_);

  auth_realms_[name] = std::move(realm);
}

void HttpAuthRealmComponent::remove_realm(const std::string &name) {
  std::lock_guard<std::mutex> const lk(realms_m_);

  const auto it = auth_realms_.find(name);
  if (it != auth_realms_.end()) {
    auth_realms_.erase(it);
  }
}

std::shared_ptr<HttpAuthRealm> HttpAuthRealmComponent::get(
    const std::string &inst) {
  std::lock_guard<std::mutex> const lk(realms_m_);

  auto it = auth_realms_.find(inst);
  if (it == auth_realms_.end()) {
    return nullptr;
  }

  return it->second;
}

std::error_code HttpAuthRealmComponent::authenticate(
    const std::string &inst, const std::string &username,
    const std::string &authdata) {
  if (auto realm = get(inst)) {
    return realm->authenticate(username, authdata);
  }
  return make_error_code(HttpAuthErrc::kRealmNotFound);
}

HttpAuthRealmComponent &HttpAuthRealmComponent::get_instance() {
  static HttpAuthRealmComponent instance;

  return instance;
}
