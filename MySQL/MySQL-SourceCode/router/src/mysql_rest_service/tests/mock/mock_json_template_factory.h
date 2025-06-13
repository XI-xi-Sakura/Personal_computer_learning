/*
  Copyright (c) 2022, 2024, Oracle and/or its affiliates.

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

#ifndef ROUTER_SRC_REST_MRS_TESTS_MOCK_MOCK_JSON_TEMPLATE_FACTORY_H_
#define ROUTER_SRC_REST_MRS_TESTS_MOCK_MOCK_JSON_TEMPLATE_FACTORY_H_

#include "mrs/database/json_template.h"

class MockJsonTemplate : public mrs::database::JsonTemplate {
 public:
  MOCK_METHOD(void, begin_resultset,
              (const std::string &url, const std::string &items_name,
               const std::vector<helper::Column> &columns),
              (override));
  MOCK_METHOD(void, begin_resultset_with_limits,
              (uint64_t offset, uint64_t limit, bool is_default_limit,
               const std::string &url,
               const std::vector<helper::Column> &columns),
              (override));
  MOCK_METHOD(bool, push_json_document, (const char *document), (override));
  MOCK_METHOD(bool, push_row,
              (const ResultRow &values, const char *ignore_column), (override));
  MOCK_METHOD(void, end_resultset, (const std::optional<bool> &), (override));
  MOCK_METHOD(void, begin, (), (override));
  MOCK_METHOD(void, finish, (const CustomMetadata &), (override));
  MOCK_METHOD(void, flush, (), (override));
  MOCK_METHOD(std::string, get_result, (), (override));
};

class InjectMockJsonTemplateFactory
    : public mrs::database::JsonTemplateFactory {
 public:
  using JsonTemplate = mrs::database::JsonTemplate;
  using JsonTemplateType = mrs::database::JsonTemplateType;

 public:
  std::shared_ptr<JsonTemplate> create_template(
      const JsonTemplateType type = JsonTemplateType::kStandard,
      [[maybe_unused]] const bool encode_bigints_as_strings = false,
      [[maybe_unused]] const bool include_links = true) const override {
    JsonTemplate *ptr;

    switch (type) {
      case JsonTemplateType::kStandard:
        ptr = &mock_;
        break;
      case JsonTemplateType::kObjectNested:
        ptr = &mock_nested_;
        break;
      case JsonTemplateType::kObjectNestedOutParameters:
        ptr = &mock_nested_out_params_;
        break;
      default:
      case JsonTemplateType::kObjectUnnested:
        ptr = &mock_unnested_;
        break;
    }
    return std::shared_ptr<JsonTemplate>(ptr, [](JsonTemplate *) {});
  }

  mutable testing::StrictMock<MockJsonTemplate> mock_;
  mutable testing::StrictMock<MockJsonTemplate> mock_unnested_;
  mutable testing::StrictMock<MockJsonTemplate> mock_nested_;
  mutable testing::StrictMock<MockJsonTemplate> mock_nested_out_params_;
};

#endif  // ROUTER_SRC_REST_MRS_TESTS_MOCK_MOCK_JSON_TEMPLATE_FACTORY_H_
