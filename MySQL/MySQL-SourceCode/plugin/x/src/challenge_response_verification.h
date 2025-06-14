/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is designed to work with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms,
 * as designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have either included with
 * the program or referenced in the documentation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef PLUGIN_X_SRC_CHALLENGE_RESPONSE_VERIFICATION_H_
#define PLUGIN_X_SRC_CHALLENGE_RESPONSE_VERIFICATION_H_

#include <string>

#include "plugin/x/src/interface/account_verification.h"
#include "plugin/x/src/interface/sha256_password_cache.h"

namespace xpl {

/**
  Class for doing account verification for the challenge response authentication
*/
class Challenge_response_verification : public iface::Account_verification {
 public:
  explicit Challenge_response_verification(iface::SHA256_password_cache *cache);

  const std::string &get_salt() const override;
  static std::string generate_salt();

 protected:
  const std::string k_salt;
  iface::SHA256_password_cache *m_sha256_password_cache;
};

}  // namespace xpl

#endif  // PLUGIN_X_SRC_CHALLENGE_RESPONSE_VERIFICATION_H_
