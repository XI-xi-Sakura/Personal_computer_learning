/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "plugin/keyring/digest.h"
#include <cassert>
#include <cstring>

namespace keyring {
Digest::Digest(DigestKind digest_kind /*=SHA256*/) : is_empty(true) {
  set_digest_kind(digest_kind);
}

Digest::Digest(DigestKind digestKind, const char *value) : is_empty(true) {
  set_digest_kind(digestKind);
  assign(value);
}

void Digest::set_digest_kind(DigestKind digest_kind) {
  switch (digest_kind) {
    case SHA256:
      length = SHA256_DIGEST_LENGTH;
      value = new unsigned char[length];
      break;
    default:
      assert(0);
  }
}

Digest::~Digest() {
  memset(value, 0, length);
  delete[] value;
}

void Digest::assign(const char *value) {
  assert(value != nullptr);
  memcpy(this->value, value, length);
  is_empty = false;
}

bool Digest::operator==(const Digest &digest) const {
  return this->is_empty == digest.is_empty && this->length == digest.length &&
         memcmp(this->value, digest.value, this->length) == 0;
}

Digest &Digest::operator=(const Digest &digest) {
  if (this != &digest) {
    this->length = digest.length;
    this->is_empty = digest.is_empty;
    if (!digest.is_empty) memcpy(this->value, digest.value, digest.length);
  }
  return *this;
}

void Digest::compute(uchar *memory, size_t memory_size) {
  // We are using SHA_EVP256 method from mysys library which symbols are
  // exported  by mysqld.
  (void)::SHA_EVP256(memory, memory_size, value);
  is_empty = false;
}
}  // namespace keyring
