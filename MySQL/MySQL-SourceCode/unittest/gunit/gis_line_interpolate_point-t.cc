/* Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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

#include <gtest/gtest.h>
#include <memory>  // unique_ptr

#include "sql/gis/length.h"
#include "sql/gis/line_interpolate.h"

#include "unittest/gunit/gis_test.h"
#include "unittest/gunit/gis_typeset.h"

namespace {

template <typename T_typeset>
struct LineInterpolatePointTest : Gis_test<T_typeset> {};

TYPED_TEST_SUITE(LineInterpolatePointTest, gis_typeset::Test_both);

TYPED_TEST(LineInterpolatePointTest, empty_linestring) {
  typename TypeParam::Linestring g{};
  double const distance = 1;
  std::unique_ptr<gis::Geometry> result_g;
  bool result_null;
  bool const res =
      gis::line_interpolate_point(this->m_srs.get(), &g, distance, false,
                                  "testcase", &result_g, &result_null);
  EXPECT_FALSE(res);
  EXPECT_TRUE(result_null);
}

TYPED_TEST(LineInterpolatePointTest, valid_input_point) {
  typename TypeParam::Linestring g{};
  g.push_back(typename TypeParam::Point(0, 0));
  g.push_back(typename TypeParam::Point(0, 1));
  double const distance = 0.5;
  std::unique_ptr<gis::Geometry> result_g;
  bool result_null = false;
  bool const res =
      gis::line_interpolate_point(this->m_srs.get(), &g, distance, false,
                                  "testcase", &result_g, &result_null);
  EXPECT_FALSE(res);
  EXPECT_EQ(gis::Geometry_type::kPoint, result_g.get()->type());
}

TYPED_TEST(LineInterpolatePointTest, valid_input_multipoint) {
  typename TypeParam::Linestring g{};
  g.push_back(typename TypeParam::Point(0, 0));
  g.push_back(typename TypeParam::Point(0, 1));
  double length;
  bool result_null = false;
  bool const res_length =
      gis::length(this->m_srs.get(), &g, &length, &result_null);
  EXPECT_FALSE(res_length);
  double const distance = 0.2 * length;
  std::unique_ptr<gis::Geometry> result_g;
  bool const res =
      gis::line_interpolate_point(this->m_srs.get(), &g, distance, true,
                                  "testcase", &result_g, &result_null);
  EXPECT_FALSE(res);
  EXPECT_EQ(gis::Geometry_type::kMultipoint, result_g.get()->type());
}

}  // namespace
