/*
  Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include <gmock/gmock.h>

#ifdef RAPIDJSON_NO_SIZETYPEDEFINE
#include "my_rapidjson_size_t.h"
#endif

#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>

#include "config_builder.h"
#include "mysql/harness/net_ts/buffer.h"
#include "mysql/harness/net_ts/internet.h"
#include "mysql/harness/net_ts/io_context.h"
#include "mysql/harness/utility/string.h"  // ::join
#include "mysqlrouter/rest_client.h"
#include "process_launcher.h"
#include "rest_api_testutils.h"
#include "router_component_test.h"
#include "router_config.h"
#include "stdx_expected_no_error.h"

using namespace std::chrono_literals;

class RestRouterApiTest
    : public RestApiComponentTest,
      public ::testing::WithParamInterface<RestApiTestParams> {};

static const std::vector<SwaggerPath> kRouterSwaggerPaths{
    {"/router/status", "Get status of the application", "status of application",
     ""},
};

/**
 * @test check /router/status
 *
 * - start router with rest_router module loaded
 * - GET /router/status
 * - check response code is 200 and output matches openapi spec
 */
TEST_P(RestRouterApiTest, ensure_openapi) {
  const std::string http_hostname = "127.0.0.1";
  const std::string http_uri = GetParam().uri + GetParam().api_path;

  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config("rest_router", userfile, true);

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};
  ProcessWrapper &http_server{launch_router({"-c", conf_file})};

  ASSERT_NO_FATAL_FAILURE(
      fetch_and_validate_schema_and_resource(GetParam(), http_server));
}

class RestRouterURITest : public RestApiComponentTest {};

TEST_F(RestRouterURITest, router_status_invalid_uri) {
  RecordProperty("Description",
                 "Check that invalid URLs are handled properly by the "
                 "http_server plugin.");
  const std::string http_hostname = "127.0.0.1";

  const std::string userfile = create_password_file();

  router_spawner().spawn(
      {"-c",
       create_config_file(
           conf_dir_.name(),
           mysql_harness::join(
               get_restapi_config("rest_router", userfile, true), "\n"))});

  net::io_context io_ctx;

  net::ip::tcp::socket sock(io_ctx);

  auto connect_res = sock.connect(
      net::ip::tcp::endpoint(net::ip::address_v4::loopback(), http_port_));
  ASSERT_NO_ERROR(connect_res);

  std::string http_request("GET /path?someparam=${...://...} HTTP/1.0\r\n\r\n");
  auto write_res = net::write(sock, net::buffer(http_request));
  ASSERT_NO_ERROR(write_res);

  std::string read_buf;
  auto read_res = net::read(sock, net::dynamic_buffer(read_buf));
  if (!read_res) {
    ASSERT_EQ(read_res.error(), net::stream_errc::eof);
  }

  EXPECT_THAT(read_buf, ::testing::StartsWith("HTTP/1.1 400 Bad Request"));
}

// ****************************************************************************
// Request the resource(s) using supported methods with authentication enabled
// and valid credentials
// ****************************************************************************
static const RestApiTestParams rest_api_valid_methods[]{
    {"router_status_get",
     std::string(rest_api_basepath) + "/router/status",
     "/router/status",
     HttpMethod::Get,
     HttpStatusCode::Ok,
     kContentTypeJson,
     kRestApiUsername,
     kRestApiPassword,
     /*request_authentication =*/true,
     {
         {"/hostname",
          [](const JsonValue *value) -> void {
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->IsString());

            // hostname shouldn't be empty
            ASSERT_GT(value->GetStringLength(), 0);
          }},
         {"/processId",
          [](const JsonValue *value) -> void {
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->IsInt());

            ASSERT_GT(value->GetInt(), 0);
          }},
         {"/productEdition",
          [](const JsonValue *value) -> void {
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->IsString());

            ASSERT_STREQ(value->GetString(), MYSQL_ROUTER_VERSION_EDITION)
                << value->GetString();
          }},
         {"/timeStarted",
          [](const JsonValue *value) -> void {
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->IsString());

            ASSERT_TRUE(pattern_found(value->GetString(), kTimestampPattern))
                << value->GetString();
          }},
         {"/version",
          [](const JsonValue *value) -> void {
            ASSERT_NE(value, nullptr);
            ASSERT_TRUE(value->IsString());

            ASSERT_TRUE(pattern_found(value->GetString(), MYSQL_ROUTER_VERSION))
                << value->GetString();
          }},
     },
     kRouterSwaggerPaths},
    {"router_status_no_params",
     std::string(rest_api_basepath) + "/router/status?someparam",
     "/router/status",
     HttpMethod::Get,
     HttpStatusCode::BadRequest,
     kContentTypeJsonProblem,
     kRestApiUsername,
     kRestApiPassword,
     /*request_authentication =*/true,
     {},
     kRouterSwaggerPaths},
};

INSTANTIATE_TEST_SUITE_P(
    ValidMethods, RestRouterApiTest,
    ::testing::ValuesIn(rest_api_valid_methods),
    [](const ::testing::TestParamInfo<RestApiTestParams> &info) {
      return info.param.test_name;
    });

// ****************************************************************************
// Request the resource(s) using supported methods with authentication enabled
// and invalid credentials
// ****************************************************************************

static const RestApiTestParams rest_api_valid_methods_invalid_auth_params[]{
    {"router_status_invalid_auth",
     std::string(rest_api_basepath) + "/router/status",
     "/router/status",
     HttpMethod::Get,
     HttpStatusCode::Unauthorized,
     kContentTypeHtmlCharset,
     kRestApiUsername,
     "invalid password",
     /*request_authentication =*/true,
     {},
     kRouterSwaggerPaths},
};

INSTANTIATE_TEST_SUITE_P(
    ValidMethodsInvalidAuth, RestRouterApiTest,
    ::testing::ValuesIn(rest_api_valid_methods_invalid_auth_params),
    [](const ::testing::TestParamInfo<RestApiTestParams> &info) {
      return info.param.test_name;
    });

// ****************************************************************************
// Request the resource(s) using unsupported methods with authentication enabled
// and valid credentials
// ****************************************************************************
static const RestApiTestParams rest_api_invalid_methods_params[]{
    {"router_status_invalid_methods",
     std::string(rest_api_basepath) + "/router/status", "/router/status",
     HttpMethod::Post | HttpMethod::Delete | HttpMethod::Patch |
         HttpMethod::Head | HttpMethod::Trace | HttpMethod::Options,
     HttpStatusCode::MethodNotAllowed, kContentTypeJsonProblem,
     kRestApiUsername, kRestApiPassword,
     /*request_authentication =*/true,
     RestApiComponentTest::get_json_method_not_allowed_verifiers(),
     kRouterSwaggerPaths},
};

INSTANTIATE_TEST_SUITE_P(
    InvalidMethods, RestRouterApiTest,
    ::testing::ValuesIn(rest_api_invalid_methods_params),
    [](const ::testing::TestParamInfo<RestApiTestParams> &info) {
      return info.param.test_name;
    });

// ****************************************************************************
// Configuration errors scenarios
// ****************************************************************************

/**
 * @test Start router with the REST routing API plugin [rest_router] and
 * [http_server] enabled but not the [rest_api] plugin.
 *
 */
TEST_F(RestRouterApiTest, rest_api_section_missing_works) {
  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config("rest_router", userfile,
                                            /*request_authentication=*/true);

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};

  launch_router({"-c", conf_file}, EXIT_SUCCESS);
}

/**
 * @test Add [rest_router] twice to the configuration file. Start router. Expect
 * router to fail providing an error about the duplicate section.
 *
 */
TEST_F(RestRouterApiTest, rest_router_section_twice) {
  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config("rest_router", userfile,
                                            /*request_authentication=*/true);

  // force [rest_router] twice in the config
  config_sections.push_back(
      mysql_harness::ConfigBuilder::build_section("rest_router", {}));

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};
  auto &router = router_spawner()
                     .wait_for_sync_point(Spawner::SyncPoint::NONE)
                     .expected_exit_code(EXIT_FAILURE)
                     .spawn({"-c", conf_file});

  check_exit_code(router, EXIT_FAILURE, 10s);

  const std::string router_output = router.get_full_output();
  EXPECT_NE(router_output.find(
                "Configuration error: Section 'rest_router' already exists"),
            router_output.npos)
      << router_output;
}

/**
 * @test Enable [rest_router] using a section key such as [rest_router:A]. Start
 * router. Expect router to fail providing an error about the use of an
 * unsupported section key.
 *
 */
TEST_F(RestRouterApiTest, rest_router_section_has_key) {
  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config("rest_router:A", userfile,
                                            /*request_authentication=*/true);

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};
  auto &router = router_spawner()
                     .wait_for_sync_point(Spawner::SyncPoint::NONE)
                     .expected_exit_code(EXIT_FAILURE)
                     .spawn({"-c", conf_file});

  check_exit_code(router, EXIT_FAILURE, 10s);

  const std::string router_output = router.get_logfile_content();
  EXPECT_THAT(router_output,
              ::testing::HasSubstr("  init 'rest_router' failed: [rest_router] "
                                   "section does not expect a key, found 'A'"));
}

/**
 * @test Try to disable authentication although a REST API endpoint/plugin
 * defines authentication as a MUST.
 *
 */
TEST_F(RestRouterApiTest, router_api_no_auth) {
  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config("rest_router", userfile,
                                            /*request_authentication=*/false);

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};
  auto &router = router_spawner()
                     .wait_for_sync_point(Spawner::SyncPoint::NONE)
                     .expected_exit_code(EXIT_FAILURE)
                     .spawn({"-c", conf_file});

  check_exit_code(router, EXIT_FAILURE, 10s);

  const std::string router_output = router.get_logfile_content();
  EXPECT_THAT(router_output, ::testing::HasSubstr(
                                 "  init 'rest_router' failed: option "
                                 "require_realm in [rest_router] is required"));
}

/**
 * @test Enable authentication for the plugin in question. Reference a realm
 * that does not exist in the configuration file.
 */
TEST_F(RestRouterApiTest, invalid_realm) {
  const std::string userfile = create_password_file();
  auto config_sections = get_restapi_config(
      "rest_router", userfile, /*request_authentication=*/true, "invalidrealm");

  const std::string conf_file{create_config_file(
      conf_dir_.name(), mysql_harness::join(config_sections, "\n"))};
  auto &router = router_spawner()
                     .wait_for_sync_point(Spawner::SyncPoint::NONE)
                     .expected_exit_code(EXIT_FAILURE)
                     .spawn({"-c", conf_file});

  check_exit_code(router, EXIT_FAILURE, 10s);

  const std::string router_output = router.get_logfile_content();
  EXPECT_THAT(
      router_output,
      ::testing::HasSubstr(
          "Configuration error: The option 'require_realm=invalidrealm' "
          "in [rest_router] does not match any http_auth_realm."));
}

int main(int argc, char *argv[]) {
  init_windows_sockets();
  ProcessManager::set_origin(Path(argv[0]).dirname());
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
