/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#ifndef COMPRESSION_INCLUDED
#define COMPRESSION_INCLUDED

/**
  @file include/compression.h
*/

#include <sstream>
#include <string>
#include <vector>
#include "my_inttypes.h"
#include "mysql_com.h"

#define COMPRESSION_ALGORITHM_ZLIB "zlib"
#define COMPRESSION_ALGORITHM_ZSTD "zstd"
#define COMPRESSION_ALGORITHM_UNCOMPRESSED "uncompressed"
#define COMPRESSION_ALGORITHM_NAME_LENGTH_MAX 32
#define COMPRESSION_ALGORITHM_COUNT_MAX 3
#define COMPRESSION_ALGORITHM_NAME_BUFFER_SIZE                                 \
  ((COMPRESSION_ALGORITHM_NAME_LENGTH_MAX * COMPRESSION_ALGORITHM_COUNT_MAX) + \
   3)
#define PROTOCOL_COMPRESSION_DEFAULT_VALUE "zlib,zstd,uncompressed"

constexpr int default_zstd_compression_level = 3;

/* Helper functions to validate compression algorithm and level */
enum_compression_algorithm get_compression_algorithm(
    const std::string &name = std::string());
std::string get_compression_algorithm_name(enum_compression_algorithm);
void parse_compression_algorithms_list(const std::string &name,
                                       std::vector<std::string> &list);
bool is_zstd_compression_level_valid(uint level);
bool validate_compression_attributes(const std::string &algorithm_names,
                                     const std::string &channel_name,
                                     bool ignore_errors);

#endif /* COMPRESSION_INCLUDED */
