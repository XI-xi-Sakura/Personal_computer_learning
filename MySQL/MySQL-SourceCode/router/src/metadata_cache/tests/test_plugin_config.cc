/*
  Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

#include <typeinfo>

#include <gmock/gmock.h>

#include "../src/metadata_cache.h"
#include "../src/plugin_config.h"

#include "mysqlrouter/utils.h"  // ms_to_second_string
#include "router_test_helpers.h"
#include "test/helpers.h"

using ::testing::ContainerEq;
using ::testing::Eq;
using ::testing::StrEq;

// the Good

struct GoodTestData {
  struct {
    std::map<std::string, std::string> extra_config_lines;
  } input;

  struct {
    std::string user;
    std::chrono::milliseconds ttl;
    std::string metadata_cluster;
  } expected;
};

class MetadataCachePluginConfigGoodTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<GoodTestData> {};

std::ostream &operator<<(std::ostream &os, const GoodTestData &test_data) {
  return os << "user=" << test_data.expected.user << ", "
            << "ttl="
            << mysqlrouter::ms_to_seconds_string(test_data.expected.ttl) << ", "
            << "metadata_cluster=" << test_data.expected.metadata_cluster;
}

/**
 * check if valid configs can be parsed
 */
TEST_P(MetadataCachePluginConfigGoodTest, GoodConfigs) {
  GoodTestData test_data = GetParam();

  mysql_harness::Config config;
  mysql_harness::ConfigSection &section = config.add("metadata_cache", "");

  for (const auto &pair : test_data.input.extra_config_lines) {
    section.add(pair.first, pair.second);
  }

  MetadataCachePluginConfig plugin_config(&section);

  EXPECT_THAT(plugin_config.user, StrEq(test_data.expected.user));
  EXPECT_THAT(plugin_config.ttl, Eq(test_data.expected.ttl));
  EXPECT_THAT(plugin_config.cluster_name,
              StrEq(test_data.expected.metadata_cluster));
}

INSTANTIATE_TEST_SUITE_P(
    SomethingUseful, MetadataCachePluginConfigGoodTest,
    ::testing::ValuesIn(std::vector<GoodTestData>({
        // minimal config
        {{std::map<std::string, std::string>({
             {"user", "foo"},  // required
         })},
         {"foo", mysqlrouter::kDefaultMetadataTTLCluster, ""}},

        // TTL = 0.5 seconds
        {{std::map<std::string, std::string>({
             {
                 "user",
                 "foo",
             },  // required
             {
                 "ttl",
                 "0.5",
             },
         })},
         {"foo", std::chrono::milliseconds(500), ""}},

        // TTL = 0 seconds
        {{std::map<std::string, std::string>({
             {
                 "user",
                 "foo",
             },  // required
             {
                 "ttl",
                 "0",
             },
         })},
         {"foo", std::chrono::milliseconds(0), ""}},

        // TTL = 5 seconds
        {{std::map<std::string, std::string>({
             {
                 "user",
                 "foo",
             },  // required
             {
                 "ttl",
                 "5",
             },
         })},
         {"foo", std::chrono::milliseconds(5000), ""}},

        // metadata_cluster
        {{std::map<std::string, std::string>({
             {
                 "user",
                 "foo",
             },  // required
             {
                 "ttl",
                 "0.5",
             },
             {
                 "metadata_cluster",
                 "whatisthis",
             },
         })},

         {"foo", std::chrono::milliseconds(500), "whatisthis"}},
    })));

// the Bad
struct BadTestData {
  struct {
    std::map<std::string, std::string> extra_config_lines;
  } input;

  struct {
    const std::type_info &exception_type;
    std::string exception_msg;
  } expected;
};

class MetadataCachePluginConfigBadTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<BadTestData> {};

std::ostream &operator<<(std::ostream &os, const BadTestData &test_data) {
  return os << test_data.expected.exception_type.name();
}

/**
 * check if invalid configs fail properly
 */
TEST_P(MetadataCachePluginConfigBadTest, BadConfigs) {
  BadTestData test_data = GetParam();

  mysql_harness::Config config;
  mysql_harness::ConfigSection &section = config.add("metadata_cache", "");

  for (const auto &pair : test_data.input.extra_config_lines) {
    section.add(pair.first, pair.second);
  }

  try {
    MetadataCachePluginConfig plugin_config(&section);
    FAIL() << "should have failed";
  } catch (const std::exception &exc) {
    EXPECT_THAT(typeid(exc).name(),
                StrEq(test_data.expected.exception_type.name()));
    EXPECT_THAT(exc.what(), StrEq(test_data.expected.exception_msg));
  }
}

INSTANTIATE_TEST_SUITE_P(
    SomethingUseful, MetadataCachePluginConfigBadTest,
    ::testing::ValuesIn(std::vector<BadTestData>({
        // user option is required
        {{
             std::map<std::string, std::string>(),
         },

         {
             typeid(mysql_harness::option_not_present),
             "option user in [metadata_cache] is required",
         }},
        // ttl is garbage
        {{
             std::map<std::string, std::string>({
                 {"user", "foo"},  // required
                 {"ttl", "garbage"},
             }),
         },
         {
             typeid(std::invalid_argument),
             "option ttl in [metadata_cache] needs value "
             "between 0 and 3600 inclusive, was 'garbage'",
         }},
        // ttl is too big
        {{
             std::map<std::string, std::string>({
                 {"user", "foo"},  // required
                 {"ttl", "3600.1"},
             }),
         },

         {
             typeid(std::invalid_argument),
             "option ttl in [metadata_cache] needs value "
             "between 0 and 3600 inclusive, was '3600.1'",
         }},
        // ttl is negative
        {{
             std::map<std::string, std::string>({
                 {"user", "foo"},  // required
                 {"ttl", "-0.1"},
             }),
         },
         {
             typeid(std::invalid_argument),
             "option ttl in [metadata_cache] needs value "
             "between 0 and 3600 inclusive, was '-0.1'",
         }},
        // close_connection_after_refresh
        {{
             std::map<std::string, std::string>({
                 {"user", "foo"},  // required
                 {"close_connection_after_refresh", "-0.1"},
             }),
         },
         {
             typeid(std::invalid_argument),
             "option close_connection_after_refresh in [metadata_cache] needs "
             "a value of either 0, 1, false or true, was '-0.1'",
         }},
    })));

// Valid millisecond configuration values
using GetOptionMillisecondsOkTestData =
    std::pair<std::string, std::chrono::milliseconds>;

class GetOptionMillisecondsOkTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<GetOptionMillisecondsOkTestData> {};

TEST_P(GetOptionMillisecondsOkTest, StringToMilliseconds) {
  GetOptionMillisecondsOkTestData test_data = GetParam();

  ASSERT_EQ(test_data.second,
            mysql_harness::MilliSecondsOption()(test_data.first, "someoption"));
}

INSTANTIATE_TEST_SUITE_P(
    OkData, GetOptionMillisecondsOkTest,
    ::testing::ValuesIn(std::vector<GetOptionMillisecondsOkTestData>({
        {"1.0", std::chrono::milliseconds(1000)},
        {"1", std::chrono::milliseconds(1000)},
        {"1.0", std::chrono::milliseconds(1000)},
        {"0.001", std::chrono::milliseconds(1)},
        {"0.0019", std::chrono::milliseconds(1)},
        {"0.002", std::chrono::milliseconds(2)},
        {"0.0020", std::chrono::milliseconds(2)},
        {"0.00200", std::chrono::milliseconds(2)},
        {"0.1", std::chrono::milliseconds(100)},
        {"0.0009", std::chrono::milliseconds(0)},
        {"0.011999", std::chrono::milliseconds(11)},
        {"1.6E2", std::chrono::milliseconds(160000)},
        {"1.6e2", std::chrono::milliseconds(160000)},
        {"1.6E+2", std::chrono::milliseconds(160000)},
        {"1.6E-2", std::chrono::milliseconds(16)},
        {"1.6E-0", std::chrono::milliseconds(1600)},
        {"1.6E+0", std::chrono::milliseconds(1600)},
        {"0.0", std::chrono::milliseconds(0)},
        {"0", std::chrono::milliseconds(0)},
        {"0.00000", std::chrono::milliseconds(0)},
        {"3600", std::chrono::milliseconds(3600000)},
        {"3600.0", std::chrono::milliseconds(3600000)},
        {"3600.0000", std::chrono::milliseconds(3600000)},
    })));

// Invalid millisecond configuration values
using GetOptionMillisecondsBadTestData = std::pair<std::string, std::string>;

class GetOptionMillisecondsBadTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<GetOptionMillisecondsBadTestData> {};

TEST_P(GetOptionMillisecondsBadTest, StringToMilliseconds) {
  GetOptionMillisecondsBadTestData test_data = GetParam();

  ASSERT_THROW_LIKE(mysql_harness::MilliSecondsOption(0.0, 3600.0)(
                        test_data.first, "someoption"),
                    std::invalid_argument, test_data.second);
}

INSTANTIATE_TEST_SUITE_P(
    OkData, GetOptionMillisecondsBadTest,
    ::testing::ValuesIn(std::vector<GetOptionMillisecondsBadTestData>({
        {"-1.0", "needs value between 0 and 3600 inclusive, was '-1.0'"},
        {"1,0", "needs value between 0 and 3600 inclusive, was '1,0'"},
        {"1xx", "needs value between 0 and 3600 inclusive, was '1xx'"},
        {"3600.1", "needs value between 0 and 3600 inclusive, was '3600.1'"},
        {"3600.001",
         "needs value between 0 and 3600 inclusive, was '3600.001'"},
    })));

int main(int argc, char *argv[]) {
  init_test_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
