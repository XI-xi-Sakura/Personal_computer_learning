/* Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "plugin/group_replication/include/mysql_version_gcs_protocol_map.h"

namespace mysql_version_gcs_protocol_map_unittest {

class MysqlVerGcsProtoMapTest : public ::testing::Test {
 public:
  MysqlVerGcsProtoMapTest() = default;
};

TEST_F(MysqlVerGcsProtoMapTest, ParseMysqlVersionInvalidFormat) {
  std::vector<std::string> const versions = {
      "",        "a",       "5",       ".",       "..",       "5..",    "5.7.",
      "5.7.14.", "5.7.100", " 5.7.14", "5.7.14 ", " 5.7.14 ", "5 .7.14"};
  for (auto const &version : versions) {
    bool const valid = valid_mysql_version_string(version.c_str());
    ASSERT_FALSE(valid);
  }
}

TEST_F(MysqlVerGcsProtoMapTest, ParseMysqlVersionValidFormat) {
  std::vector<std::string> const versions = {"5.7.14", "8.0.16", "00.00.00",
                                             "99.99.99"};
  for (auto const &version : versions) {
    bool const valid = valid_mysql_version_string(version.c_str());
    ASSERT_TRUE(valid);
  }
}

TEST_F(MysqlVerGcsProtoMapTest, ConvertToMemberVersion) {
  std::vector<Member_version> const versions = {
      Member_version(0x050714), Member_version(0x080015),
      Member_version(0x000000), Member_version(0x999999)};
  for (auto const &version : versions) {
    Member_version const member_version =
        convert_to_member_version(version.get_version_string().c_str());
    ASSERT_EQ(version, member_version);
  }
}

TEST_F(MysqlVerGcsProtoMapTest, ConvertToMysqlVersion) {
  std::vector<std::pair<Gcs_protocol_version, Member_version>> const versions =
      {{Gcs_protocol_version::V1, Member_version(0x050714)},
       {Gcs_protocol_version::V2, Member_version(0x080016)},
       {Gcs_protocol_version::V3, Member_version(0x080027)}};
  for (auto const &version_pair : versions) {
    Member_version const member_version =
        convert_to_mysql_version(version_pair.first);
    ASSERT_EQ(version_pair.second, member_version);
  }
}

TEST_F(MysqlVerGcsProtoMapTest, ConvertToGcsProtocol) {
  std::vector<std::pair<Member_version, Gcs_protocol_version>> const versions =
      {{Member_version(0x050714), Gcs_protocol_version::V1},
       {Member_version(0x080015), Gcs_protocol_version::V1},
       {Member_version(0x080016), Gcs_protocol_version::V2},
       {Member_version(0x080027), Gcs_protocol_version::V3}};
  for (auto const &version_pair : versions) {
    Gcs_protocol_version const gcs_protocol =
        convert_to_gcs_protocol(version_pair.first, Member_version(0x080027));
    ASSERT_EQ(version_pair.second, gcs_protocol);
  }
}

}  // namespace mysql_version_gcs_protocol_map_unittest
