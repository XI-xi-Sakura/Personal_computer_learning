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

#include "helper/digester/sha256_digest.h"

namespace helper {
namespace digester {

Sha256Digest::Sha256Digest() : Sha256Digest(Digest::Type::Sha256) {}

Sha256Digest::Sha256Digest(Digest::Type type) : digest_(type) {}

void Sha256Digest::update(std::string_view data) {
  digest_.update(data.data(), data.size());
  all.append(data);
}

std::string Sha256Digest::finalize() {
  std::string res;
  res.resize(digest_.digest_size(Digest::Type::Sha256));
  digest_.finalize(res);
  return res;
}

}  // namespace digester
}  // namespace helper
