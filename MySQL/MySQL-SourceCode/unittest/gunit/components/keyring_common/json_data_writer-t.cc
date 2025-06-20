/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#include <string>

#include <components/keyrings/common/json_data/json_reader.h>
#include <components/keyrings/common/json_data/json_writer.h>

#include <gtest/gtest.h>

namespace json_writer_unittest {

class KeyringCommonJsonWriter_test : public ::testing::Test {};

using keyring_common::data::Data;
using keyring_common::json_data::Json_data_extension;
using keyring_common::json_data::Json_reader;
using keyring_common::json_data::Json_writer;
using keyring_common::meta::Metadata;

TEST_F(KeyringCommonJsonWriter_test, JsonWriterTest) {
  Json_writer json_writer;
  Json_data_extension json_data_extension;
  {
    Metadata const metadata("key1", "foo@bar");
    Data const data("abcdefghijklmnop", "AES");
    ASSERT_FALSE(json_writer.add_element(metadata, data, json_data_extension));
  }
  {
    Metadata const metadata("key1", "bar@foo");
    Data const data("qrstuvwx", "RSA");
    ASSERT_FALSE(json_writer.add_element(metadata, data, json_data_extension));
  }
  {
    Metadata const metadata("master_key", "");
    Data const data("yzabcdefghijklmn", "AES");
    ASSERT_FALSE(json_writer.add_element(metadata, data, json_data_extension));
  }

  Json_reader const json_reader(json_writer.to_string());
  ASSERT_TRUE(json_reader.valid());

  Json_writer json_writer_2(json_writer.to_string());
  ASSERT_TRUE(json_writer_2.valid());
  ASSERT_TRUE(json_writer_2.num_elements() == 3);
  {
    Metadata const metadata("key1", "bar@foo");
    ASSERT_FALSE(json_writer_2.remove_element(metadata, json_data_extension));
    ASSERT_TRUE(json_writer_2.remove_element(metadata, json_data_extension));
  }
  ASSERT_TRUE(json_writer_2.num_elements() == 2);
  {
    Metadata const metadata("key1", "bar@foo");
    Data const data("qrstuvwx", "RSA");
    ASSERT_FALSE(
        json_writer_2.add_element(metadata, data, json_data_extension));
    ASSERT_FALSE(
        json_writer_2.add_element(metadata, data, json_data_extension));
    ASSERT_TRUE(json_writer_2.num_elements() == 4);
    ASSERT_FALSE(json_writer_2.remove_element(metadata, json_data_extension));
    ASSERT_TRUE(json_writer_2.num_elements() == 2);
  }

  Json_reader const json_reader_2(json_writer_2.to_string());
  ASSERT_TRUE(json_reader_2.valid());
}

}  // namespace json_writer_unittest
