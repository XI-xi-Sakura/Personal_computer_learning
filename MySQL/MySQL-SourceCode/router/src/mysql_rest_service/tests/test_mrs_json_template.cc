/*
  Copyright (c) 2022, 2025, Oracle and/or its affiliates.

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
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "helper/make_shared_ptr.h"
#include "mrs/json/json_template_nest.h"
#include "mrs/json/json_template_nest_without_outparams.h"
#include "mrs/json/json_template_unnest.h"

using ResultRow = mysqlrouter::MySQLSession::ResultRow;
template <typename T>
using MakeSharedPtr = helper::MakeSharedPtr<T>;
using testing::Test;
using namespace mrs::json;

template <typename SutType>
class JsonTemplateParamTests : public Test {
 public:
  using ParamType = SutType;
  MakeSharedPtr<ParamType> sut_;
};

using AcceptSinglResultsetTypes =
    ::testing::Types<JsonTemplateNest, JsonTemplateUnnest,
                     JsonTemplateNestWithoutOutParameters>;

using AcceptMultipleResultsetsTypes =
    ::testing::Types<JsonTemplateNest, JsonTemplateNestWithoutOutParameters>;

template <typename T>
class MultipleResultsetsJsonTemplateParamTests
    : public JsonTemplateParamTests<T> {};

TYPED_TEST_SUITE(JsonTemplateParamTests, AcceptSinglResultsetTypes);
TYPED_TEST_SUITE(MultipleResultsetsJsonTemplateParamTests,
                 AcceptMultipleResultsetsTypes);

struct ConstantsNestWithout {
  const std::string k_empty_resultset{"{\"resultSets\":[]}"};
  const std::string k_resultset_without_data{
      "{\"resultSets\":[{\"type\":\"myitems\",\"items\":[],\"_metadata\":{"
      "\"columns\":[]}}]}"};
  const std::string k_resultset_only_metadata{
      "{\"resultSets\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":[],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INTEGER\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"}"
      "]}}]}"};
  const std::string k_resultset_with_data{
      "{\"resultSets\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":0},"
      "{\"c1\":100,\"c2\":null,\"c3\":1000000}"
      "],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}"
      "]}}]}"};
  const std::string k_resultset_with_data_bigints_encode{
      "{\"resultSets\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":\"0\"},"
      "{\"c1\":100,\"c2\":null,\"c3\":\"1000000\"}"
      "],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}"
      "]}}]}"};

  const std::string k_resultset_and_out_parameters{
      "{\"resultSets\":["
      "{\"type\":\"myitems\",\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":\"0\"},"
      "{\"c1\":100,\"c2\":null,\"c3\":\"1000000\"}],"
      "\"_metadata\":{\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}]}}],"
      "\"outParameters\":{\"param1\":10101,\"param2\":\"Parameter text\"}}"};
};

struct ConstantsNest {
  const std::string k_empty_resultset{"{\"items\":[]}"};
  const std::string k_resultset_without_data{
      "{\"items\":[{\"type\":\"myitems\",\"items\":[],\"_metadata\":{"
      "\"columns\":[]}}]}"};
  const std::string k_resultset_only_metadata{
      "{\"items\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":[],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INTEGER\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"}"
      "]}}]}"};
  const std::string k_resultset_with_data{
      "{\"items\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":0},"
      "{\"c1\":100,\"c2\":null,\"c3\":1000000}"
      "],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}"
      "]}}]}"};
  const std::string k_resultset_with_data_bigints_encode{
      "{\"items\":["
      "{"
      "\"type\":\"myitems\","
      "\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":\"0\"},"
      "{\"c1\":100,\"c2\":null,\"c3\":\"1000000\"}"
      "],"
      "\"_metadata\":{"
      "\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}"
      "]}}]}"};
  const std::string k_resultset_and_out_parameters{
      "{\"items\":["
      "{\"type\":\"myitems\",\"items\":"
      "[{\"c1\":0,\"c2\":\"Some text value\",\"c3\":\"0\"},"
      "{\"c1\":100,\"c2\":null,\"c3\":\"1000000\"}],"
      "\"_metadata\":{\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}]}},"
      "{\"type\":\"out-items\",\"items\":"
      "[{\"param1\":10101,\"param2\":\"Parameter text\"}],"
      "\"_metadata\":{\"columns\":[{\"name\":\"param1\",\"type\":\"INT\"},"
      "{\"name\":\"param2\",\"type\":\"TEXT\"}]}}]}"};
};

struct ConstantsUnest {
  const std::string k_empty_resultset{
      "{\"items\":[],\"_metadata\":{\"columns\":[]}}"};
  const std::string k_resultset_without_data{
      "{\"items\":[],\"_metadata\":{\"columns\":[]}}"};
  const std::string k_resultset_only_metadata{
      "{\"items\":[],\"_metadata\":{\"columns\":[{\"name\":\"c1\",\"type\":"
      "\"INTEGER\"},{\"name\":\"c2\",\"type\":\"TEXT\"}]}}"};
  const std::string k_resultset_with_data{
      "{\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":0},"
      "{\"c1\":100,\"c2\":null,\"c3\":1000000}"
      "],"
      "\"_metadata\":{\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}]}}"};
  const std::string k_resultset_with_data_bigints_encode{
      "{\"items\":["
      "{\"c1\":0,\"c2\":\"Some text value\",\"c3\":\"0\"},"
      "{\"c1\":100,\"c2\":null,\"c3\":\"1000000\"}"
      "],"
      "\"_metadata\":{\"columns\":["
      "{\"name\":\"c1\",\"type\":\"INT\"},"
      "{\"name\":\"c2\",\"type\":\"TEXT\"},"
      "{\"name\":\"c3\",\"type\":\"BIGINT\"}]}}"};
  const std::string k_resultset_and_out_parameters{};
};

template <typename T>
class UseConstants {};

template <>
class UseConstants<JsonTemplateNest> {
 public:
  using Type = ConstantsNest;
};

template <>
class UseConstants<JsonTemplateUnnest> {
 public:
  using Type = ConstantsUnest;
};

template <>
class UseConstants<JsonTemplateNestWithoutOutParameters> {
 public:
  using Type = ConstantsNestWithout;
};

TYPED_TEST(JsonTemplateParamTests, no_iteration_doesnt_generates) {
  typename UseConstants<TypeParam>::Type constant;
  auto sut = this->sut_.get();
  ASSERT_EQ("", sut->get_result());
}

TYPED_TEST(JsonTemplateParamTests, begin_end_generated_empty_resultsets_list) {
  typename UseConstants<TypeParam>::Type constant;
  auto sut = this->sut_.get();
  sut->begin();
  sut->finish();
  ASSERT_EQ(constant.k_empty_resultset, sut->get_result());
}

TYPED_TEST(
    JsonTemplateParamTests,
    begin_bresultset_eresultset_end_generates_single_resultset_withoutdata) {
  typename UseConstants<TypeParam>::Type constant;
  auto sut = this->sut_.get();

  sut->begin();
  sut->begin_resultset("local", "myitems", {});
  sut->end_resultset();
  sut->finish();
  ASSERT_EQ(constant.k_resultset_without_data, sut->get_result());
}

TYPED_TEST(JsonTemplateParamTests,
           generate_single_resultset_with_only_metadata) {
  typename UseConstants<TypeParam>::Type constant;
  auto sut = this->sut_.get();

  sut->begin();
  sut->begin_resultset("local", "myitems", {{"c1", "INTEGER"}, {"c2", "TEXT"}});
  sut->end_resultset();
  sut->finish();
  ASSERT_EQ(constant.k_resultset_only_metadata, sut->get_result());
}

TYPED_TEST(JsonTemplateParamTests, generated_with_one_resultset) {
  typename UseConstants<TypeParam>::Type constant;
  auto sut = this->sut_.get();
  ResultRow r1{{"0", "Some text value", "0"}};
  ResultRow r2{{"100", nullptr, "1000000"}};

  sut->begin();
  sut->begin_resultset("local", "myitems",
                       {{"c1", "INT"}, {"c2", "TEXT"}, {"c3", "BIGINT"}});
  sut->push_row(r1);
  sut->push_row(r2);
  sut->end_resultset();
  sut->finish();
  ASSERT_EQ(constant.k_resultset_with_data, sut->get_result());
}

TYPED_TEST(JsonTemplateParamTests,
           generated_with_one_resultset_bigint_as_string) {
  typename UseConstants<TypeParam>::Type constant;
  this->sut_.reset(new TypeParam(true));
  auto sut = this->sut_.get();
  ResultRow r1{{"0", "Some text value", "0"}};
  ResultRow r2{{"100", nullptr, "1000000"}};

  sut->begin();
  sut->begin_resultset("local", "myitems",
                       {{"c1", "INT"}, {"c2", "TEXT"}, {"c3", "BIGINT"}});
  sut->push_row(r1);
  sut->push_row(r2);
  sut->end_resultset();
  sut->finish();
  ASSERT_EQ(constant.k_resultset_with_data_bigints_encode, sut->get_result());
}

TYPED_TEST(MultipleResultsetsJsonTemplateParamTests,
           generated_with_one_resultset_and_out_params) {
  typename UseConstants<TypeParam>::Type constant;

  this->sut_.reset(new TypeParam(true));
  auto sut = this->sut_.get();
  ResultRow r1{{"0", "Some text value", "0"}};
  ResultRow r2{{"100", nullptr, "1000000"}};
  ResultRow p1{{"10101", "Parameter text"}};

  sut->begin();
  sut->begin_resultset("local", "myitems",
                       {{"c1", "INT"}, {"c2", "TEXT"}, {"c3", "BIGINT"}});
  sut->push_row(r1);
  sut->push_row(r2);
  sut->end_resultset();
  sut->begin_resultset("local", "out-items",
                       {{"param1", "INT", false, false, true},
                        {"param2", "TEXT", false, false, true}});
  sut->push_row(p1);
  sut->end_resultset();
  sut->finish();
  ASSERT_EQ(constant.k_resultset_and_out_parameters, sut->get_result());
}
