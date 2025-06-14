/* Copyright (c) 2014, 2025, Oracle and/or its affiliates.

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
#include <cstddef>
#include <string>
#include <vector>

#include "my_inttypes.h"
#include "plugin/group_replication/include/member_info.h"
#include "plugin/group_replication/include/services/notification/notification.h"

using std::string;
using std::vector;

namespace gcs_member_info_unittest {

class ClusterMemberInfoTest : public ::testing::Test {
 protected:
  ClusterMemberInfoTest() = default;

  void SetUp() override {
    string const hostname("pc_hostname");
    string const uuid("781f947c-db4a-11e3-98d1-f01faf1a1c44");
    uint const port = 4444;
    uint const plugin_version = 0x000400;
    uint const write_set_algorithm = 1;
    string executed_gtid("aaaa:1-10");
    string purged_gtid("pppp:1-9");
    uint const lower_case_table_names = 0;
    bool const default_table_encryption = false;
    string retrieved_gtid("bbbb:1-10");
    ulonglong const gtid_assignment_block_size = 9223372036854775807ULL;
    bool const in_primary_mode = false;
    bool const has_enforces_update_everywhere_checks = false;
    uint const member_weight = 70;
    std::string const member_recovery_endpoints = "DEFAULT";
    std::string const member_view_change_uuid = "AUTOMATIC";
    bool const allow_single_leader = true;
    bool const most_uptodate = true;

    gcs_member_id = new Gcs_member_identifier("stuff");

    Group_member_info::Group_member_status const status =
        Group_member_info::MEMBER_OFFLINE;

    Member_version local_member_plugin_version(plugin_version);
    local_node = new Group_member_info(
        hostname.c_str(), port, uuid.c_str(), write_set_algorithm,
        gcs_member_id->get_member_id(), status, local_member_plugin_version,
        gtid_assignment_block_size, Group_member_info::MEMBER_ROLE_PRIMARY,
        in_primary_mode, has_enforces_update_everywhere_checks, member_weight,
        lower_case_table_names, default_table_encryption,
        member_recovery_endpoints.c_str(), member_view_change_uuid.c_str(),
        allow_single_leader, most_uptodate, PSI_NOT_INSTRUMENTED);
    local_node->update_gtid_sets(executed_gtid, purged_gtid, retrieved_gtid);
  }

  void TearDown() override {
    delete gcs_member_id;
    delete local_node;
  }

  Group_member_info *local_node;
  Gcs_member_identifier *gcs_member_id;
};

TEST_F(ClusterMemberInfoTest, EncodeDecodeIdempotencyTest) {
  auto *encoded = new vector<uchar>();
  local_node->encode(encoded);

  Group_member_info decoded_local_node(&encoded->front(), encoded->size(),
                                       PSI_NOT_INSTRUMENTED);

  ASSERT_EQ(local_node->get_port(), decoded_local_node.get_port());
  ASSERT_EQ(local_node->get_hostname(), decoded_local_node.get_hostname());
  ASSERT_EQ(local_node->get_uuid(), decoded_local_node.get_uuid());
  ASSERT_EQ(local_node->get_write_set_extraction_algorithm(),
            decoded_local_node.get_write_set_extraction_algorithm());
  ASSERT_EQ(local_node->get_gcs_member_id().get_member_id(),
            decoded_local_node.get_gcs_member_id().get_member_id());
  ASSERT_EQ(local_node->get_recovery_status(),
            decoded_local_node.get_recovery_status());
  ASSERT_EQ(local_node->get_member_version().get_version(),
            decoded_local_node.get_member_version().get_version());
  ASSERT_EQ(local_node->get_gtid_executed(),
            decoded_local_node.get_gtid_executed());
  ASSERT_EQ(local_node->get_gtid_purged(),
            decoded_local_node.get_gtid_purged());
  ASSERT_EQ(local_node->get_gtid_retrieved(),
            decoded_local_node.get_gtid_retrieved());
  ASSERT_EQ(local_node->get_gtid_assignment_block_size(),
            decoded_local_node.get_gtid_assignment_block_size());
  ASSERT_EQ(local_node->get_role(), decoded_local_node.get_role());
  ASSERT_EQ(local_node->get_member_weight(),
            decoded_local_node.get_member_weight());
  ASSERT_EQ(local_node->get_view_change_uuid(),
            decoded_local_node.get_view_change_uuid());
  ASSERT_EQ(local_node->get_allow_single_leader(),
            decoded_local_node.get_allow_single_leader());
  ASSERT_EQ(local_node->get_component_primary_election_enabled(),
            decoded_local_node.get_component_primary_election_enabled());
  delete encoded;
}

class ClusterMemberInfoManagerTest : public ::testing::Test {
 protected:
  ClusterMemberInfoManagerTest() = default;

  void SetUp() override {
    string const hostname("pc_hostname");
    string const uuid("8d7r947c-dr4a-17i3-59d1-f01faf1kkc44");
    uint const port = 4444;
    uint const write_set_algorithm = 1;
    uint const lower_case_table_names = 0;
    bool const default_table_encryption = false;
    uint const plugin_version = 0x000400;
    gcs_member_id = new Gcs_member_identifier("stuff");
    ulonglong const gtid_assignment_block_size = 9223372036854775807ULL;
    bool const in_primary_mode = false;
    bool const has_enforces_update_everywhere_checks = false;
    uint const member_weight = 80;
    std::string const member_recovery_endpoints = "DEFAULT";
    std::string const member_view_change_uuid =
        "8896eb66-8684-11eb-8dcd-0242ac130003";
    bool const allow_single_leader = false;
    bool const most_uptodate = false;

    Group_member_info::Group_member_status const status =
        Group_member_info::MEMBER_OFFLINE;

    Member_version local_member_plugin_version(plugin_version);
    local_node = new Group_member_info(
        hostname.c_str(), port, uuid.c_str(), write_set_algorithm,
        gcs_member_id->get_member_id(), status, local_member_plugin_version,
        gtid_assignment_block_size, Group_member_info::MEMBER_ROLE_SECONDARY,
        in_primary_mode, has_enforces_update_everywhere_checks, member_weight,
        lower_case_table_names, default_table_encryption,
        member_recovery_endpoints.c_str(), member_view_change_uuid.c_str(),
        allow_single_leader, most_uptodate, PSI_NOT_INSTRUMENTED);

    cluster_member_mgr =
        new Group_member_info_manager(local_node, PSI_NOT_INSTRUMENTED);
  }

  void TearDown() override {
    delete cluster_member_mgr;
    delete gcs_member_id;
    delete local_node;
  }

  Group_member_info_manager_interface *cluster_member_mgr;
  Group_member_info *local_node;
  Gcs_member_identifier *gcs_member_id;
};

TEST_F(ClusterMemberInfoManagerTest, GetLocalInfoByUUIDTest) {
  // Add another member info in order to make this test more realistic
  string const hostname("pc_hostname2");
  string const uuid("781f947c-db4a-22e3-99d4-f01faf1a1c44");
  uint const port = 4444;
  uint const write_set_algorithm = 1;
  uint const lower_case_table_names = 0;
  bool const default_table_encryption = false;
  uint const plugin_version = 0x000400;
  Gcs_member_identifier const gcs_member_id("another_stuff");
  string executed_gtid("aaaa:1-11");
  string purged_gtid("pppp:1-8");
  string retrieved_gtid("bbbb:1-11");
  ulonglong const gtid_assignment_block_size = 9223372036854775807ULL;
  bool const in_primary_mode = false;
  bool const has_enforces_update_everywhere_checks = false;
  uint const member_weight = 90;
  std::string const member_recovery_endpoints = "DEFAULT";
  std::string const member_view_change_uuid =
      "99f957fc-75c5-445d-98ae-7ea02e55c5be";
  bool const allow_single_leader = true;
  bool const most_uptodate = true;

  Group_member_info::Group_member_status const status =
      Group_member_info::MEMBER_OFFLINE;

  Member_version local_member_plugin_version(plugin_version);
  auto *new_member = new Group_member_info(
      hostname.c_str(), port, uuid.c_str(), write_set_algorithm,
      gcs_member_id.get_member_id(), status, local_member_plugin_version,
      gtid_assignment_block_size, Group_member_info::MEMBER_ROLE_PRIMARY,
      in_primary_mode, has_enforces_update_everywhere_checks, member_weight,
      lower_case_table_names, default_table_encryption,
      member_recovery_endpoints.c_str(), member_view_change_uuid.c_str(),
      allow_single_leader, most_uptodate, PSI_NOT_INSTRUMENTED);
  new_member->update_gtid_sets(executed_gtid, purged_gtid, retrieved_gtid);

  cluster_member_mgr->add(new_member);

  string const uuid_to_get("8d7r947c-dr4a-17i3-59d1-f01faf1kkc44");

  Group_member_info retrieved_local_info;
  const bool retrieved_local_info_not_found =
      cluster_member_mgr->get_group_member_info(uuid_to_get,
                                                retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);
  ASSERT_EQ(retrieved_local_info.get_uuid(), uuid_to_get);
}

TEST_F(ClusterMemberInfoManagerTest, UpdateStatusOfLocalObjectTest) {
  Notification_context ctx;
  cluster_member_mgr->update_member_status(
      local_node->get_uuid(), Group_member_info::MEMBER_ONLINE, ctx);

  ASSERT_EQ(Group_member_info::MEMBER_ONLINE,
            local_node->get_recovery_status());
}

TEST_F(ClusterMemberInfoManagerTest, UpdateGtidSetsOfLocalObjectTest) {
  string executed_gtid("aaaa:1-10");
  string purged_gtid("pppp:1-7");
  string retrieved_gtid("bbbb:1-10");

  cluster_member_mgr->update_gtid_sets(local_node->get_uuid(), executed_gtid,
                                       purged_gtid, retrieved_gtid);

  ASSERT_EQ(executed_gtid, local_node->get_gtid_executed());
  ASSERT_EQ(purged_gtid, local_node->get_gtid_purged());
  ASSERT_EQ(retrieved_gtid, local_node->get_gtid_retrieved());
}

TEST_F(ClusterMemberInfoManagerTest, GetLocalInfoByUUIDAfterEncodingTest) {
  auto *encoded = new vector<uchar>();
  cluster_member_mgr->encode(encoded);

  Group_member_info_list *decoded_members =
      cluster_member_mgr->decode(&encoded->front(), encoded->size());

  cluster_member_mgr->update(decoded_members);

  delete decoded_members;
  delete encoded;

  string const uuid_to_get("8d7r947c-dr4a-17i3-59d1-f01faf1kkc44");

  Group_member_info retrieved_local_info;
  const bool retrieved_local_info_not_found =
      cluster_member_mgr->get_group_member_info(uuid_to_get,
                                                retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);

  ASSERT_EQ(local_node->get_port(), retrieved_local_info.get_port());
  ASSERT_EQ(local_node->get_hostname(), retrieved_local_info.get_hostname());
  ASSERT_EQ(local_node->get_uuid(), retrieved_local_info.get_uuid());
  ASSERT_EQ(local_node->get_gcs_member_id().get_member_id(),
            retrieved_local_info.get_gcs_member_id().get_member_id());
  ASSERT_EQ(local_node->get_recovery_status(),
            retrieved_local_info.get_recovery_status());
  ASSERT_EQ(local_node->get_write_set_extraction_algorithm(),
            retrieved_local_info.get_write_set_extraction_algorithm());
  ASSERT_EQ(local_node->get_gtid_executed(),
            retrieved_local_info.get_gtid_executed());
  ASSERT_EQ(local_node->get_gtid_purged(),
            retrieved_local_info.get_gtid_purged());
  ASSERT_EQ(local_node->get_gtid_retrieved(),
            retrieved_local_info.get_gtid_retrieved());
  ASSERT_EQ(local_node->get_gtid_assignment_block_size(),
            retrieved_local_info.get_gtid_assignment_block_size());
  ASSERT_EQ(local_node->get_role(), retrieved_local_info.get_role());
  ASSERT_EQ(local_node->get_member_weight(),
            retrieved_local_info.get_member_weight());
  ASSERT_EQ(local_node->get_view_change_uuid(),
            retrieved_local_info.get_view_change_uuid());
  ASSERT_EQ(local_node->get_allow_single_leader(),
            retrieved_local_info.get_allow_single_leader());
  ASSERT_EQ(local_node->get_component_primary_election_enabled(),
            retrieved_local_info.get_component_primary_election_enabled());
}

TEST_F(ClusterMemberInfoManagerTest,
       UpdateStatusOfLocalObjectAfterExchangeTest) {
  Notification_context ctx;
  auto *encoded = new vector<uchar>();
  cluster_member_mgr->encode(encoded);

  Group_member_info_list *decoded_members =
      cluster_member_mgr->decode(&encoded->front(), encoded->size());

  cluster_member_mgr->update(decoded_members);

  delete decoded_members;
  delete encoded;

  cluster_member_mgr->update_member_status(
      local_node->get_uuid(), Group_member_info::MEMBER_ONLINE, ctx);

  ASSERT_EQ(Group_member_info::MEMBER_ONLINE,
            local_node->get_recovery_status());

  string executed_gtid("cccc:1-11");
  string purged_gtid("pppp:1-11");
  string retrieved_gtid("dddd:1-11");

  cluster_member_mgr->update_gtid_sets(local_node->get_uuid(), executed_gtid,
                                       purged_gtid, retrieved_gtid);

  ASSERT_EQ(executed_gtid, local_node->get_gtid_executed());
  ASSERT_EQ(retrieved_gtid, local_node->get_gtid_retrieved());

  Group_member_info retrieved_local_info;
  const bool retrieved_local_info_not_found =
      cluster_member_mgr->get_group_member_info(local_node->get_uuid(),
                                                retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);
  ASSERT_EQ(Group_member_info::MEMBER_ONLINE,
            retrieved_local_info.get_recovery_status());

  ASSERT_EQ(executed_gtid, retrieved_local_info.get_gtid_executed());
  ASSERT_EQ(retrieved_gtid, retrieved_local_info.get_gtid_retrieved());
}

TEST_F(ClusterMemberInfoManagerTest, EncodeDecodeLargeSets) {
  // Add another member info in order to make this test more realistic
  string const hostname("pc_hostname2");
  string const uuid("781f947c-db4a-22e3-99d4-f01faf1a1c44");
  uint const port = 4444;
  uint const write_set_algorithm = 1;
  uint const lower_case_table_names = 0;
  bool const default_table_encryption = false;
  uint const plugin_version = 0x000400;
  Gcs_member_identifier const gcs_member_id("another_stuff");
  string executed_gtid("aaaa:1-11:12-14:16-20:22-30");
  string purged_gtid("pppp:1-11:12-14:17-20:26-39:50-78");
  // Add an huge gtid string (bigger then 16 bits )
  string retrieved_gtid(70000, 'a');
  ulonglong const gtid_assignment_block_size = 9223372036854775807ULL;
  bool const in_primary_mode = false;
  bool const has_enforces_update_everywhere_checks = false;
  uint const member_weight = 40;
  std::string const member_recovery_endpoints = "DEFAULT";
  std::string const member_view_change_uuid =
      "ba1c4c32-1887-4ccf-bc5a-8f6165d19ea3";
  bool const allow_single_leader = true;
  bool const most_uptodate = true;

  Group_member_info::Group_member_status const status =
      Group_member_info::MEMBER_OFFLINE;

  Member_version local_member_plugin_version(plugin_version);
  auto *new_member = new Group_member_info(
      hostname.c_str(), port, uuid.c_str(), write_set_algorithm,
      gcs_member_id.get_member_id(), status, local_member_plugin_version,
      gtid_assignment_block_size, Group_member_info::MEMBER_ROLE_PRIMARY,
      in_primary_mode, has_enforces_update_everywhere_checks, member_weight,
      lower_case_table_names, default_table_encryption,
      member_recovery_endpoints.c_str(), member_view_change_uuid.c_str(),
      allow_single_leader, most_uptodate, PSI_NOT_INSTRUMENTED);
  new_member->update_gtid_sets(executed_gtid, purged_gtid, retrieved_gtid);

  cluster_member_mgr->add(new_member);

  string const uuid_to_get("8d7r947c-dr4a-17i3-59d1-f01faf1kkc44");

  Group_member_info retrieved_local_info;
  bool retrieved_local_info_not_found =
      cluster_member_mgr->get_group_member_info(uuid_to_get,
                                                retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);
  ASSERT_EQ(retrieved_local_info.get_uuid(), uuid_to_get);

  auto *encoded = new vector<uchar>();
  cluster_member_mgr->encode(encoded);

  Group_member_info_list *decoded_members =
      cluster_member_mgr->decode(&encoded->front(), encoded->size());
  delete encoded;

  cluster_member_mgr->update(decoded_members);

  delete decoded_members;

  ASSERT_EQ(2U, cluster_member_mgr->get_number_of_members());

  retrieved_local_info_not_found =
      cluster_member_mgr->get_group_member_info(uuid, retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);

  ASSERT_EQ(port, retrieved_local_info.get_port());
  ASSERT_EQ(hostname, retrieved_local_info.get_hostname());
  ASSERT_EQ(executed_gtid, retrieved_local_info.get_gtid_executed());
  ASSERT_EQ(retrieved_gtid, retrieved_local_info.get_gtid_retrieved());
  ASSERT_EQ(write_set_algorithm,
            retrieved_local_info.get_write_set_extraction_algorithm());

  retrieved_local_info_not_found = cluster_member_mgr->get_group_member_info(
      uuid_to_get, retrieved_local_info);

  ASSERT_TRUE(false == retrieved_local_info_not_found);

  ASSERT_EQ(local_node->get_port(), retrieved_local_info.get_port());
  ASSERT_EQ(local_node->get_hostname(), retrieved_local_info.get_hostname());
  ASSERT_EQ(local_node->get_uuid(), retrieved_local_info.get_uuid());
  ASSERT_EQ(local_node->get_gcs_member_id().get_member_id(),
            retrieved_local_info.get_gcs_member_id().get_member_id());
  ASSERT_EQ(local_node->get_recovery_status(),
            retrieved_local_info.get_recovery_status());
  ASSERT_EQ(local_node->get_write_set_extraction_algorithm(),
            retrieved_local_info.get_write_set_extraction_algorithm());
  ASSERT_EQ(local_node->get_gtid_executed(),
            retrieved_local_info.get_gtid_executed());
  ASSERT_EQ(local_node->get_gtid_purged(),
            retrieved_local_info.get_gtid_purged());
  ASSERT_EQ(local_node->get_gtid_retrieved(),
            retrieved_local_info.get_gtid_retrieved());
  ASSERT_EQ(local_node->get_gtid_assignment_block_size(),
            retrieved_local_info.get_gtid_assignment_block_size());
  ASSERT_EQ(local_node->get_role(), retrieved_local_info.get_role());
  ASSERT_EQ(local_node->get_member_weight(),
            retrieved_local_info.get_member_weight());
  ASSERT_EQ(local_node->get_lower_case_table_names(),
            retrieved_local_info.get_lower_case_table_names());
  ASSERT_EQ(local_node->get_default_table_encryption(),
            retrieved_local_info.get_default_table_encryption());
  ASSERT_EQ(local_node->get_view_change_uuid(),
            retrieved_local_info.get_view_change_uuid());
  ASSERT_EQ(local_node->get_allow_single_leader(),
            retrieved_local_info.get_allow_single_leader());
  ASSERT_EQ(local_node->get_component_primary_election_enabled(),
            retrieved_local_info.get_component_primary_election_enabled());
}

}  // namespace gcs_member_info_unittest
