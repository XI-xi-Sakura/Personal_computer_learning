/* Copyright (c) 2012, 2025, Oracle and/or its affiliates.

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

/**
  @file include/crypt_genhash_impl.h
*/

#ifndef CRYPT_HASHGEN_IMPL_H
#define CRYPT_HASHGEN_IMPL_H
#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN ROUNDS_DEFAULT
#define ROUNDS_MAX (0xFFF * 1000)
#define MIXCHARS 32
#define CRYPT_SALT_LENGTH 20
#define CRYPT_MAGIC_LENGTH 3
#define CRYPT_PARAM_LENGTH 13
#define SHA256_HASH_LENGTH 43
#define CRYPT_MAX_PASSWORD_SIZE                                  \
  (CRYPT_SALT_LENGTH + SHA256_HASH_LENGTH + CRYPT_MAGIC_LENGTH + \
   CRYPT_PARAM_LENGTH)
#define MAX_PLAINTEXT_LENGTH 256

#include <stddef.h>

#include "my_macros.h"

int extract_user_salt(const char **salt_begin, const char **salt_end);
char *my_crypt_genhash(char *ctbuffer, size_t ctbufflen, const char *plaintext,
                       size_t plaintext_len, const char *switchsalt,
                       const char **params, unsigned int *num_rounds = nullptr);
void generate_user_salt(char *buffer, int buffer_len);
void xor_string(char *to, int to_len, char *pattern, int pattern_len);

// extern "C" since it is an (undocumented) part of the libmysql ABI.
extern "C" void my_make_scrambled_password(char *to, const char *password,
                                           size_t pass_len);

#endif
