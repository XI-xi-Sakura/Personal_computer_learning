/*
  Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#include <chrono>

#include <gmock/gmock.h>

#ifdef RAPIDJSON_NO_SIZETYPEDEFINE
#include "my_rapidjson_size_t.h"
#endif
#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include "dim.h"
#include "harness_assert.h"
#include "keyring/keyring_manager.h"
#include "mock_server_rest_client.h"
#include "mock_server_testutils.h"
#include "mysql/harness/string_utils.h"  // mysql_harness::split_string
#include "mysqlrouter/cluster_metadata.h"
#include "random_generator.h"
#include "rest_api_testutils.h"
#include "router_component_clusterset.h"
#include "router_component_test.h"
#include "router_component_testutils.h"
#include "router_config.h"  // MYSQL_ROUTER_VERSION
#include "socket_operations.h"
#include "tcp_port_pool.h"

using namespace std::chrono_literals;
using namespace std::string_literals;
using mysqlrouter::ClusterType;

// for the test with no param
class RouterClusterSetBootstrapTest : public RouterComponentClusterSetTest {
 protected:
  ProcessWrapper &launch_router_for_bootstrap(
      std::vector<std::string> params, int expected_exit_code = EXIT_SUCCESS,
      const bool disable_rest = true, const bool add_report_host = true) {
    if (disable_rest) params.push_back("--disable-rest");
    if (add_report_host) params.push_back("--report-host=dont.query.dns");
    params.push_back("--conf-set-option=DEFAULT.plugin_folder=" +
                     ProcessManager::get_plugin_dir().str());

    return ProcessManager::launch_router(
        params, expected_exit_code, /*catch_stderr=*/true, /*with_sudo=*/false,
        /*wait_for_notify_ready=*/std::chrono::seconds(-1),
        RouterComponentBootstrapTest::kBootstrapOutputResponder);
  }

  using NodeAddress = std::pair<std::string, uint16_t>;
  TempDirectory bootstrap_directory;
  uint64_t view_id{1};
};

struct TargetClusterTestParams {
  // which cluster from the CS should be used as a param for --bootstrap
  unsigned bootstrap_cluster_id;
  // which node from the selected cluster should be used as a param for
  // --bootstrap
  unsigned bootstrap_node_id;
  // what should be the value for --conf-target-cluster (if empty do not use
  // this parameter in the bootstrap command)
  std::string target_cluster_param;
  // what should be the value for --conf-target-cluster-by-name (if empty do not
  // use this parameter in the bootstrap command)
  std::string target_cluster_by_name_param;

  // id of the target Cluster within ClusterSet
  unsigned target_cluster_id;

  // what is the expected value of the target_cluster in the configuration file
  // created by the bootstrap
  std::string expected_target_cluster;

  // vector of strings expected on the console after the bootstrap
  std::vector<std::string> expected_output_strings;

  bool new_executable{false};
};

class ClusterSetBootstrapTargetClusterTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<TargetClusterTestParams> {};

TEST_P(ClusterSetBootstrapTargetClusterTest, ClusterSetBootstrapTargetCluster) {
  const auto target_cluster_id = GetParam().target_cluster_id;
  const std::string expected_target_cluster =
      GetParam().expected_target_cluster;

  ClusterSetOptions cs_options;
  cs_options.target_cluster_id = target_cluster_id;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.expected_target_cluster = expected_target_cluster;
  create_clusterset(cs_options);

  const unsigned bootstrap_cluster_id = GetParam().bootstrap_cluster_id;
  const unsigned bootstrap_node_id = GetParam().bootstrap_node_id;
  const std::string target_cluster_param =
      GetParam().target_cluster_param.empty()
          ? ""
          : "--conf-target-cluster=" + GetParam().target_cluster_param;

  const std::string target_cluster_by_name_param =
      GetParam().target_cluster_by_name_param.empty()
          ? ""
          : "--conf-target-cluster-by-name=" +
                GetParam().target_cluster_by_name_param;
  const auto &expected_output_strings = GetParam().expected_output_strings;

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[bootstrap_cluster_id]
                             .nodes[bootstrap_node_id]
                             .classic_port),
      "-d", bootstrap_directory.name()};

  if (!target_cluster_param.empty()) {
    bootstrap_params.push_back(target_cluster_param);
  }
  if (!target_cluster_by_name_param.empty()) {
    bootstrap_params.push_back(target_cluster_by_name_param);
  }

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_SUCCESS);

  check_exit_code(router, EXIT_SUCCESS);

  const std::string conf_file_path =
      bootstrap_directory.name() + "/mysqlrouter.conf";

  const std::string state_file_path =
      bootstrap_directory.name() + "/data/state.json";

  // check the state file that was produced
  // [@FR12]
  check_state_file(state_file_path, ClusterType::GR_CS,
                   cs_options.topology.uuid,
                   cs_options.topology.get_md_servers_classic_ports(), view_id);

  const std::string config_file_str = get_file_output(conf_file_path);

  // [@FR2]
  EXPECT_TRUE(pattern_found(config_file_str, "cluster_type=gr"))
      << config_file_str;
  EXPECT_TRUE(pattern_found(config_file_str, "ttl=5")) << config_file_str;

  const std::string router_console_output = router.get_full_output();
  for (const auto &expected_output_string : expected_output_strings) {
    EXPECT_TRUE(pattern_found(router_console_output, expected_output_string))
        << router_console_output;
  }
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetBootstrapTargetCluster, ClusterSetBootstrapTargetClusterTest,
    ::testing::Values(
        // we bootstrap against the consecutive nodes (0, 1, 2) of the first
        // cluster which is the PRIMARY cluster; both "--conf-target-cluster"
        // and "--conf-target-cluster-by-name" parameters are empty (not used)
        // so per requirement we are expected to configure
        // empty target cluster
        // [@FR1]
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 1,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 2,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        // we bootstrap against the nodes of the other clusters which is
        // are REPLICA clusters; both "--conf-target-cluster" and
        // "--conf-target-cluster-by-name" parameters are empty (not used) so
        // per requirement we are expected to configure empty target cluster
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 1,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 1,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 1,
                                /*bootstrap_node_id*/ 1,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 1,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 1,
                                /*bootstrap_node_id*/ 2,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 1,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        // second Replica Cluster, nodes 0-2
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 2,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 1,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 2,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 2,
                                /*--conf-target-cluster*/ "",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 2,
                                /*expected_target_cluster*/ "",
                                /*expected_output_strings*/ {}},
        // we bootstrap against the nodes of the first Cluster which is the
        // PRIMARY Cluster; the "--conf-target-cluster=current" so
        // per requirement we are expected to configure
        // target_cluster=UUID-OF-PRIMARY-CLUSTER
        // NOTE: since we are using "current" on the Primary cluster we expect
        // the warning on the console
        // NOTE: also checks that the "current" option is case insensitive
        // [@FR3.1.1] [@FR3.3] [@TS_R2_1/1]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "current",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/
            {"WARNING: Option --conf-target-cluster=current was used to "
             "bootstrap against the Primary Cluster. Note that the Router will "
             "not follow the new Primary Cluster in case of the Primary "
             "Cluster change in the ClusterSet"}},
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "Current",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/
            {"WARNING: Option --conf-target-cluster=current was used to "
             "bootstrap against the Primary Cluster. Note that the Router will "
             "not follow the new Primary Cluster in case of the Primary "
             "Cluster change in the ClusterSet"}},
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "CURRENT",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/
            {"WARNING: Option --conf-target-cluster=current was used to "
             "bootstrap against the Primary Cluster. Note that the Router will "
             "not follow the new Primary Cluster in case of the Primary "
             "Cluster change in the ClusterSet"}},
        // [@TS_R2_1/2]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            /*--conf-target-cluster*/ "currenT",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/
            {"WARNING: Option --conf-target-cluster=current was used to "
             "bootstrap against the Primary Cluster. Note that the Router will "
             "not follow the new Primary Cluster in case of the Primary "
             "Cluster change in the ClusterSet"}},

        // we bootstrap against the nodes of the second Cluster which is the
        // REPLICA Cluster; the "--conf-target-cluster=current" so
        // per requirement we are expected to configure
        // target_cluster=UUID-OF-REPLICA-CLUSTER
        // NOTE: since this is not the PRIMARY cluster we do not expect the
        // warning now NOTE: also checks that the "current" option is case
        // insensitive
        // [@FR3.2] [@TS_R2_1/3]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "current",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 1,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
            /*expected_output_strings*/
            {""}},
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "Current",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 1,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
            /*expected_output_strings*/
            {""}},
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "CURRENT",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 1,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
            /*expected_output_strings*/
            {""}},
        // [@TS_R2_1/4]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 1,
            /*--conf-target-cluster*/ "current",
            /*--conf-target-cluster-by-name*/ "",
            /*target_cluster_id*/ 1,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
            /*expected_output_strings*/
            {""}},

        // we bootstrap against various ClusterSet nodes using
        // "--conf-target-cluster=primary" so we expect target_cluster=primary
        // NOTE: also checks that the "current" option is case insensitive
        // [@FR3.2] [@FR3.3] [@TS_R3_1/1]
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "primary",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "Primary",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "PRIMARY",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        // [@TS_R3_1/2]
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 0,
                                /*bootstrap_node_id*/ 2,
                                /*--conf-target-cluster*/ "primarY",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        // [@TS_R3_1/3]
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 1,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "primary",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        // [@TS_R3_1/4]
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "primary",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 1,
                                /*--conf-target-cluster*/ "Primary",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},
        TargetClusterTestParams{/*bootstrap_cluster_id*/ 2,
                                /*bootstrap_node_id*/ 0,
                                /*--conf-target-cluster*/ "PRIMARY",
                                /*--conf-target-cluster-by-name*/ "",
                                /*target_cluster_id*/ 0,
                                /*expected_target_cluster*/ "primary",
                                /*expected_output_strings*/
                                {""}},

        // we bootstrap against various ClusterSet nodes using
        // "--conf-target-cluster-name=<CLUSTER-NAME>" so we expect
        // target_cluster=<CLUSTER_GR_UUID>
        // [@FR3.4]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            /*--conf-target-cluster*/ "",
            /*--conf-target-cluster-by-name*/ "cluster-name-1",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/ {}},
        // [@TS_R5_1/2]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            /*--conf-target-cluster*/ "",
            /*--conf-target-cluster-by-name*/ "cluster-name-1",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/ {}},
        // [@TS_R5_1/2]
        TargetClusterTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            /*--conf-target-cluster*/ "",
            /*--conf-target-cluster-by-name*/ "cluster-name-1",
            /*target_cluster_id*/ 0,
            /*expected_target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*expected_output_strings*/ {}}));

struct ClusterSetUseGrNotificationTestParams {
  std::vector<std::string> bootstrap_params;
  std::vector<std::string> expected_config_lines;
};

class ClusterSetConfUseGrNotificationParamTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<
          ClusterSetUseGrNotificationTestParams> {};

/**
 * @test
 *       verify that using various values for --conf-use-gr-notifications
 * creates proper config file entries.
 */
TEST_P(ClusterSetConfUseGrNotificationParamTest,
       ClusterSetConfUseGrNotificationParam) {
  TempDirectory bootstrap_directory;

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  create_clusterset(cs_options);

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[0].nodes[0].classic_port),
      "-d", bootstrap_directory.name()};

  bootstrap_params.insert(bootstrap_params.end(),
                          GetParam().bootstrap_params.begin(),
                          GetParam().bootstrap_params.end());

  // launch the router in bootstrap mode
  auto &router = launch_router_for_bootstrap(bootstrap_params);

  check_exit_code(router, EXIT_SUCCESS);

  const std::string state_file_path =
      bootstrap_directory.name() + "/data/state.json";

  // check the state file that was produced
  // [@FR12]
  check_state_file(state_file_path, ClusterType::GR_CS,
                   cs_options.topology.uuid,
                   cs_options.topology.get_md_servers_classic_ports(), view_id);

  // check if the expected config options were added to the configuration file
  const auto conf_file_content =
      get_file_output(bootstrap_directory.name() + "/mysqlrouter.conf");
  for (const auto &expected_config_line : GetParam().expected_config_lines) {
    EXPECT_THAT(mysql_harness::split_string(conf_file_content, '\n'),
                ::testing::Contains(expected_config_line));
  }
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetConfUseGrNotificationParam,
    ClusterSetConfUseGrNotificationParamTest,
    ::testing::Values(
        // 0) --conf-use-gr-notifications with no param: GR Notifications should
        // be enabled and TTL=5 seconds
        // [@FR5]
        // [@FR5.2.3]
        ClusterSetUseGrNotificationTestParams{
            {"--conf-use-gr-notifications"},
            {"use_gr_notifications=1", "ttl=5"}},
        // 1) --conf-use-gr-notifications=1: GR Notifications should be enabled
        // and TTL=5 seconds
        // [@FR5]
        ClusterSetUseGrNotificationTestParams{
            {"--conf-use-gr-notifications=1"},
            {"use_gr_notifications=1", "ttl=5"}},
        // 2) no --conf-use-gr-notifications param: GR Notifications should be
        // enabled and TTL=5 seconds
        // [@FR5]
        // [@FR5.1]
        // [@FR5.2]
        // [@FR5.2.2]
        ClusterSetUseGrNotificationTestParams{
            {}, {"use_gr_notifications=1", "ttl=5"}},
        // 3) --conf-use-gr-notification=0: GR Notifications should be disabled
        // and TTL=5 seconds
        // [@FR5.2]
        // [@FR5.2.1]
        ClusterSetUseGrNotificationTestParams{
            {"--conf-use-gr-notifications=0"},
            {"use_gr_notifications=0", "ttl=5"}}));

struct BootstrapParametersErrorTestParams {
  // which cluster from the CS should be used as a param for --bootstrap
  unsigned bootstrap_cluster_id;
  // which node from the selected cluster should be used as a param for
  // --bootstrap
  unsigned bootstrap_node_id;

  std::vector<std::string> bootstrap_params;
  std::string expected_error;
};

class ClusterSetBootstrapParamsErrorTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<BootstrapParametersErrorTestParams> {
};

/**
 * @test
 *       verify the proper errors are reported for invalid --conf-target-cluster
 * and --conf-target-cluster-by-name uses
 */
TEST_P(ClusterSetBootstrapParamsErrorTest, ClusterSetBootstrapParamsError) {
  const unsigned bootstrap_cluster_id = GetParam().bootstrap_cluster_id;
  const unsigned bootstrap_node_id = GetParam().bootstrap_node_id;

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  create_clusterset(cs_options);

  std::vector<std::string> bootstrap_params{
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[bootstrap_cluster_id]
                             .nodes[bootstrap_node_id]
                             .classic_port),
      "--connect-timeout=1",
      "-d",
      bootstrap_directory.name(),
  };

  bootstrap_params.insert(bootstrap_params.end(),
                          GetParam().bootstrap_params.begin(),
                          GetParam().bootstrap_params.end());

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_FAILURE);

  // verify that appropriate message was logged

  EXPECT_NO_THROW(router.wait_for_exit());
  EXPECT_THAT(router.get_full_output(),
              ::testing::HasSubstr(GetParam().expected_error));

  check_exit_code(router, EXIT_FAILURE);
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetBootstrapParamsError, ClusterSetBootstrapParamsErrorTest,
    ::testing::Values(
        // verify that using empty string as value for --conf-target-cluster
        // leads to expected bootstrap error
        // [@FR3.3.2]
        // [@FTS_R4_1/1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster="},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        //[@FTS_R4_1/2]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=''"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/3]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=''"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        //[@FTS_R4_1/4]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster=\"\""},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/5]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=none"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/6]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=gr-id-1"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/7]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=current2"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/8]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=primary cluster"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // [@FTS_R4_1/9]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster=0"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // verify that using --conf-target-cluster-by-name with no value
        // leads to expected bootstrap error
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name"},
            "Error: option '--conf-target-cluster-by-name' expects a value, "
            "got nothing"},
        // verify that using --conf-target-cluster-by-name with no value
        // leads to expected bootstrap error
        // [@TS_R5_1/13]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 1,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name="},
            "Value for parameter '--conf-target-cluster-by-name' can't be "
            "empty"},
        // verify that using both --conf-target-cluster and
        // --conf-target-cluster-by-name leads to expected bootstrap error
        // [@FR3.5.1]
        // [@TS_R8_2/1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=primary",
             "--conf-target-cluster-by-name=cluster-name-1"},
            "Parameters '--conf-target-cluster' and "
            "'--conf-target-cluster-by-name' "
            "are mutually exclusive and can't be used together"},
        // [@TS_R8_2/2]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name=cluster-name-1",
             "--conf-target-cluster=primary"},
            "Parameters '--conf-target-cluster' and "
            "'--conf-target-cluster-by-name' "
            "are mutually exclusive and can't be used together"},
        // verify that using value other than 'primary' or 'current' for
        // --conf-target-cluster leads to expected bootstrap error
        // [@FR3.3.1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=invalid"},
            "Value for parameter '--conf-target-cluster' needs to be one of: "
            "['primary', 'current']"},
        // verify that using --conf-target-cluster with no value leads to
        // expected bootstrap error
        // [@FR3.3.2]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster"},
            "Error: option '--conf-target-cluster' expects a value, got "
            "nothing"}));

class ClusterSetBootstrapClusterNotFoundErrorTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<BootstrapParametersErrorTestParams> {
};

/**
 * @test
 *       verify the proper errors are reported if requested Cluster was not
 * found when bootstrapping
 */
TEST_P(ClusterSetBootstrapClusterNotFoundErrorTest,
       ClusterSetBootstrapClusterNotFoundError) {
  const unsigned bootstrap_cluster_id = GetParam().bootstrap_cluster_id;
  const unsigned bootstrap_node_id = GetParam().bootstrap_node_id;

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.simulate_cluster_not_found = true;
  create_clusterset(cs_options);

  std::vector<std::string> bootstrap_params{
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[bootstrap_cluster_id]
                             .nodes[bootstrap_node_id]
                             .classic_port),
      "--connect-timeout=1",
      "-d",
      bootstrap_directory.name(),
  };

  bootstrap_params.insert(bootstrap_params.end(),
                          GetParam().bootstrap_params.begin(),
                          GetParam().bootstrap_params.end());

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_FAILURE);

  // verify that appropriate message was logged

  EXPECT_NO_THROW(router.wait_for_exit());
  EXPECT_THAT(router.get_full_output(),
              ::testing::HasSubstr(GetParam().expected_error));

  check_exit_code(router, EXIT_FAILURE);
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetBootstrapClusterNotFoundError,
    ClusterSetBootstrapClusterNotFoundErrorTest,
    ::testing::Values(
        // verify that using --conf-target-cluster=primary where PRIMARY
        // Cluster can't be found leads to a proper error
        // [@FR3.2.1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=primary"},
            "Error: Could not reach Primary Cluster for the ClusterSet"},

        // verify that using --conf-target-cluster-by-name=foo where foo is
        // not a cluster leads to a proper error
        // [@FR3.4.1]
        // [@TS_R5_1/19]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name=foo"},
            "Error: Could not find Cluster with selected name: 'foo'"},
        // [@TS_R5_1/12]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=primary"},
            "Error: Could not find Cluster with selected name: 'primary'"},
        // [@TS_R5_1/13]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=current"},
            "Error: Could not find Cluster with selected name: 'current'"},
        // [@TS_R5_1/14]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=''"},
            "Error: Could not find Cluster with selected name: ''"},
        // [@TS_R5_1/15]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=\" \""},
            "Error: Could not find Cluster with selected name: '\" \"'"},
        // [@TS_R5_1/16]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=0"},
            "Error: Could not find Cluster with selected name: '0'"},
        // [@TS_R5_1/1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name=00000000-0000-0000-0000-"
             "0000000000c1"},
            "Error: Could not find Cluster with selected name: "
            "'00000000-0000-0000-0000-0000000000c1'"},
        // [@TS_R5_1/8]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 2,
            /*bootstrap_node_id*/ 1,
            {"--conf-target-cluster-by-name=00000000-0000-0000-0000-"
             "0000000000c3"},
            "Error: Could not find Cluster with selected name: "
            "'00000000-0000-0000-0000-0000000000c3'"},
        // [@TS_R5_1/19]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name=\"foo\""},
            "Error: Could not find Cluster with selected name: '\"foo\"'"}));

/**
 * @test
 *       verify that Router fails whene there is no Primary Cluster while doing
 * the bootstrap
 * [@TS_R7_1]
 */
TEST_F(RouterClusterSetBootstrapTest, ClusterSetBootstrapNoPrimaryError) {
  const unsigned non_existing_cluster_id{5};

  ClusterSetOptions cs_options;
  cs_options.primary_cluster_id = non_existing_cluster_id;
  cs_options.tracefile = "bootstrap_clusterset.js";
  create_clusterset(cs_options);

  std::vector<std::string> bootstrap_params{
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[0].nodes[0].classic_port),
      "--connect-timeout=1",
      "-d",
      bootstrap_directory.name(),
      "--conf-target-cluster",
      "primary",
  };

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_FAILURE);

  // verify that appropriate message was logged
  EXPECT_NO_THROW(router.wait_for_exit());
  EXPECT_THAT(router.get_full_output(),
              ::testing::HasSubstr(
                  "Error: Could not reach Primary Cluster for the ClusterSet"));

  check_exit_code(router, EXIT_FAILURE);
}

class ClusterSetBootstrapParamsNoBootstrapErrorTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<BootstrapParametersErrorTestParams> {
};

/**
 * @test
 *       verify that --conf-target-cluster and --conf-target-cluster-by-name
 * params are only valid for bootstrap
 */
TEST_P(ClusterSetBootstrapParamsNoBootstrapErrorTest,
       ClusterSetBootstrapParamsNoBootstrapError) {
  // const uint16_t server_port = port_pool_.get_next_available();
  std::vector<std::string> router_params;

  router_params.insert(router_params.end(), GetParam().bootstrap_params.begin(),
                       GetParam().bootstrap_params.end());

  auto &router = launch_router_for_bootstrap(router_params, EXIT_FAILURE);

  //  // verify that appropriate message was logged

  EXPECT_NO_THROW(router.wait_for_exit());
  EXPECT_THAT(router.get_full_output(),
              ::testing::HasSubstr(GetParam().expected_error));

  check_exit_code(router, EXIT_FAILURE);
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetBootstrapParamsNoBootstrapError,
    ClusterSetBootstrapParamsNoBootstrapErrorTest,
    ::testing::Values(
        // 0) verify that using --conf-target-cluster when
        // not bootstrapping leads to expected error
        // [@FR3.5]
        // [@TS_R8_1/1]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=primary"},
            "Error: Option --conf-target-cluster can only "
            "be used together with -B/--bootstrap"},
        // 1) verify that using --conf-target-cluster-by-name when not
        // bootstrapping leads to expected error
        // [@FR3.5]
        // [@TS_R8_1/2]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster-by-name=cluster-name-1"},
            "Error: Option --conf-target-cluster-by-name can only "
            "be used together with -B/--bootstrap"},
        // [@TS_R8_1/3]
        BootstrapParametersErrorTestParams{
            /*bootstrap_cluster_id*/ 0,
            /*bootstrap_node_id*/ 0,
            {"--conf-target-cluster=primary",
             "--conf-target-cluster-by-name=cluster-name-1"},
            "Error: Parameters '--conf-target-cluster' and "
            "'--conf-target-cluster-by-name' are mutually exclusive and can't "
            "be used together"}));

static int get_session_init_count(const uint16_t http_port) {
  std::string server_globals =
      MockServerRestClient(http_port).get_globals_as_json_string();

  return get_int_field_value(server_globals, "session_count");
}

/**
 * @test
 *       verify that when user bootstraps using non-writable node, bootstrap
 * failover will first go to the nodes of the Cluster who's role is reported as
 * PRIMARY in the metadata, regardless of the order of those nodes returned by
 * the query
 *
 * For this we have a following scenario:
 * ClusterSet with 3 clusters
 * Cluster 1 is REPLICA
 * Cluster 2 is REPLICA
 * Cluster 3 is PRIMARY
 *
 * We use first node of Cluster 2 to bootstrap. We expect the failover, as this
 * node is not writable. The first node we are expected to failover to is the
 * first node of Cluster 3. We never expect to try to connect to Cluster 1.
 */
TEST_F(RouterClusterSetBootstrapTest, PrimaryClusterQueriedFirst) {
  const int target_cluster_id = 1;
  const int primary_cluster_id = 2;
  const std::string expected_target_cluster =
      "00000000-0000-0000-0000-0000000000g2";

  ClusterSetOptions cs_options;
  cs_options.target_cluster_id = target_cluster_id;
  cs_options.primary_cluster_id = primary_cluster_id;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.expected_target_cluster = expected_target_cluster;
  create_clusterset(cs_options);

  const unsigned bootstrap_node_id = 0;
  const std::string target_cluster_param = "--conf-target-cluster=current";

  // const auto &expected_output_strings = GetParam().expected_output_strings;

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[target_cluster_id]
                             .nodes[bootstrap_node_id]
                             .classic_port),
      "-d", bootstrap_directory.name(), target_cluster_param,
      "--logger.level=debug"};

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_SUCCESS);

  check_exit_code(router, EXIT_SUCCESS);

  // check that the only nodes that we connected to during the bootstrap are the
  // one used as a -B parameter (first node of the second cluster) and the
  // primary node (first node of the third cluster)
  for (size_t cluster_id = 0; cluster_id < cs_options.topology.clusters.size();
       ++cluster_id) {
    const auto &cluster = cs_options.topology.clusters[cluster_id];

    for (size_t node_id = 0; node_id < cluster.nodes.size(); ++node_id) {
      const int expected_session_count =
          (cluster_id == 1 && node_id == 0) || (cluster_id == 2 && node_id == 0)
              ? 1
              : 0;

      EXPECT_EQ(expected_session_count,
                get_session_init_count(cluster.nodes[node_id].http_port));
    }
  }
}

/**
 * @test
 *       verify that bootstrapping using Read Replica address fails and proper
 * error message is printed
 */
TEST_F(RouterClusterSetBootstrapTest, FailToBootstrapFromReadReplica) {
  const std::vector<size_t> gr_nodes_per_cluster{2, 2};
  const std::vector<size_t> read_replicas_per_cluster{1, 0};

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.gr_nodes_number = gr_nodes_per_cluster;
  cs_options.read_replicas_number = read_replicas_per_cluster;
  create_clusterset(cs_options);

  const auto &read_replica_node =
      cs_options.topology.clusters[0]
          .nodes[cs_options.topology.clusters[0].nodes.size() - 1];
  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" + std::to_string(read_replica_node.classic_port),
      "-d", bootstrap_directory.name()};

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_FAILURE);

  EXPECT_NO_THROW(router.wait_for_exit());
  EXPECT_THAT(
      router.get_full_output(),
      ::testing::HasSubstr("Error: Bootstrapping using the Read Replica "
                           "Instance address is not supported"));

  check_exit_code(router, EXIT_FAILURE);
}

/**
 * @test
 *       verify that Rad Replicas are not added to the state file as metadata
 * servers during bootstrap
 */
TEST_F(RouterClusterSetBootstrapTest, BootstrapWithReadReaplicas) {
  // let's configure the ClusterSet so that each of 3 clusters has 1 Read
  // Replica
  const std::vector<size_t> read_replicas_per_cluster{1, 1, 1};

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.read_replicas_number = read_replicas_per_cluster;
  create_clusterset(cs_options);

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[0].nodes[0].classic_port),
      "-d", bootstrap_directory.name()};

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_SUCCESS);

  check_exit_code(router, EXIT_SUCCESS);

  const std::string state_file_path =
      bootstrap_directory.name() + "/data/state.json";

  // the Read Replicas should not get written to the state file as metadata
  // servers
  check_state_file(state_file_path, ClusterType::GR_CS,
                   cs_options.topology.uuid,
                   cs_options.topology.get_md_servers_classic_ports(), view_id);
}

/**
 * @test
 *       Checks that the Router correctly exposes its configuration details when
 * bootstrapped against the ClusterSet.
 */
TEST_F(RouterClusterSetBootstrapTest, ConfigExposedInMetadata) {
  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.simulate_config_defaults_stored_is_null = true;
  create_clusterset(cs_options);

  std::vector<std::string> bootsrtap_params{
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[0].nodes[0].classic_port),
      "-d",
      bootstrap_directory.name(),
      "--conf-target-cluster=primary",
  };

  const auto http_port = cs_options.topology.clusters[0].nodes[0].http_port;

  auto &router = launch_router_for_bootstrap(bootsrtap_params, EXIT_SUCCESS);

  check_exit_code(router, EXIT_SUCCESS);

  RecordProperty("Worklog", "15649");
  RecordProperty("RequirementId", "FR1,FR1.1,FR2,FR3,FR3.1");
  RecordProperty("Description",
                 "Testing if the Router correctly exposes it's full static "
                 "configuration on bootstrap in the metadata when the target "
                 "is Clusterset.");

  // first validate the configuration json against general "public" schema for
  // the structure corectness
  const std::string public_config_schema =
      get_file_output(Path(ROUTER_SRC_DIR)
                          .join("src")
                          .join("harness")
                          .join("src")
                          .join("configuration_schema.json")
                          .str());

  validate_config_stored_in_md(http_port, public_config_schema);

  // then validate against strict schema that also checks the values specific to
  // this bootstrap
  const std::string strict_config_schema = get_file_output(
      get_data_dir()
          .join("default_bootstrap_configuration_schema_strict.json")
          .str());

  validate_config_stored_in_md(http_port, strict_config_schema);

  RecordProperty("Worklog", "15649");
  RecordProperty("RequirementId", "FR1,FR1.1,FR2,FR3,FR3.1");
  RecordProperty(
      "Description",
      "Testing if on bootstrap the Router correctly exposes "
      "ConfigurationChangesSchema and Defaults JSONs in "
      "mysql_innodb_cluster_metadata.cluster.router_options.->>"
      "Configuration.<router_version> when the target is ClusterSet.");

  // Check if proper UpdateSchema was written on BS
  const std::string public_config_update_schema_in_md =
      get_config_update_schema_stored_in_md(http_port);

  const std::string public_config_update_schema = get_json_in_pretty_format(
      get_file_output(Path(ROUTER_SRC_DIR)
                          .join("src")
                          .join("harness")
                          .join("src")
                          .join("configuration_update_schema.json")
                          .str()));

  EXPECT_STREQ(public_config_update_schema.c_str(),
               public_config_update_schema_in_md.c_str());

  // Check if proper Configuration Defaults were written on bootstrap
  const std::string public_configuration_defaults_in_md =
      get_config_defaults_stored_in_md(http_port) + "\n";

  const std::string public_configuration_defaults = get_file_output(
      get_data_dir().join("configuration_defaults_clusterset.json").str());

  EXPECT_STREQ(public_configuration_defaults.c_str(),
               public_configuration_defaults_in_md.c_str());
}

struct ServerCompatTestParam {
  std::string description;
  std::string server_version;
  std::string expected_warning_msg;
};

class CheckServerCompatibilityTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<ServerCompatTestParam> {};

/**
 * @test
 *      Verifies that the server version is checked for compatibility when the
 * Router is bootstrapped against the ClusterSet
 */
TEST_P(CheckServerCompatibilityTest, Spec) {
  RecordProperty("Description", GetParam().description);

  ClusterSetOptions cs_options;
  cs_options.tracefile = "bootstrap_clusterset.js";
  create_clusterset(cs_options);

  const auto http_port = cs_options.topology.clusters[0].nodes[0].http_port;

  set_mock_server_version(http_port, GetParam().server_version);

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[0].nodes[0].classic_port),
      "-d", bootstrap_directory.name()};

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_SUCCESS);
  check_exit_code(router, EXIT_SUCCESS);

  if (!GetParam().expected_warning_msg.empty()) {
    const std::string router_console_output = router.get_full_output();
    EXPECT_TRUE(
        pattern_found(router_console_output, GetParam().expected_warning_msg))
        << router_console_output;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Spec, CheckServerCompatibilityTest,
    ::testing::Values(
        ServerCompatTestParam{
            "Server is the same version as Router - bootstrap OK",
            std::to_string(MYSQL_ROUTER_VERSION_MAJOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_MINOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_PATCH),
            ""},
        ServerCompatTestParam{
            "Server major version is higher than Router - bootstrap should "
            "issue a warning",
            std::to_string(MYSQL_ROUTER_VERSION_MAJOR + 1) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_MINOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_PATCH),
            "WARNING: MySQL Server version .* is higher than the Router "
            "version. You should upgrade the Router to match the MySQL Server "
            "version."},
        ServerCompatTestParam{
            "Server minor version is higher than Router - bootstrap should "
            "issue a warning",
            std::to_string(MYSQL_ROUTER_VERSION_MAJOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_MINOR + 1) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_PATCH),
            "WARNING: MySQL Server version .* is higher than the Router "
            "version. You should upgrade the Router to match the MySQL Server "
            "version."},
        ServerCompatTestParam{
            "Server patch version is higher than Router - bootstrap OK",
            std::to_string(MYSQL_ROUTER_VERSION_MAJOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_MINOR) + "." +
                std::to_string(MYSQL_ROUTER_VERSION_PATCH + 1),
            ""}));

struct LocalClusterTestParams {
  std::string expected_local_cluster;
  unsigned bootstrap_cluster_id;
  unsigned bootstrap_node_id;
};

class LocalClusterTest
    : public RouterClusterSetBootstrapTest,
      public ::testing::WithParamInterface<LocalClusterTestParams> {};

TEST_P(LocalClusterTest, LocalCluster) {
  const auto target_cluster_id = 0;

  ClusterSetOptions cs_options;
  cs_options.target_cluster_id = target_cluster_id;
  cs_options.tracefile = "bootstrap_clusterset.js";
  cs_options.expected_local_cluster = GetParam().expected_local_cluster;
  create_clusterset(cs_options);

  const unsigned bootstrap_cluster_id = GetParam().bootstrap_cluster_id;
  const unsigned bootstrap_node_id = GetParam().bootstrap_node_id;

  std::vector<std::string> bootstrap_params = {
      "--bootstrap=127.0.0.1:" +
          std::to_string(cs_options.topology.clusters[bootstrap_cluster_id]
                             .nodes[bootstrap_node_id]
                             .classic_port),
      "-d", bootstrap_directory.name()};

  auto &router = launch_router_for_bootstrap(bootstrap_params, EXIT_SUCCESS);

  check_exit_code(router, EXIT_SUCCESS);
}

INSTANTIATE_TEST_SUITE_P(
    LocalCluster, LocalClusterTest,
    ::testing::Values(LocalClusterTestParams{"cluster-name-1", /*cluster_id*/ 0,
                                             /*node_id*/ 0},
                      LocalClusterTestParams{"cluster-name-1", /*cluster_id*/ 0,
                                             /*node_id*/ 1},
                      LocalClusterTestParams{"cluster-name-2", /*cluster_id*/ 1,
                                             /*node_id*/ 0},
                      LocalClusterTestParams{"cluster-name-2", /*cluster_id*/ 1,
                                             /*node_id*/ 2},
                      LocalClusterTestParams{"cluster-name-3", /*cluster_id*/ 2,
                                             /*node_id*/ 0}));

int main(int argc, char *argv[]) {
  init_windows_sockets();
  ProcessManager::set_origin(Path(argv[0]).dirname());
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
