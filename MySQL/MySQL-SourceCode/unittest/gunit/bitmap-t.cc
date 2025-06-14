/* Copyright (c) 2013, 2025, Oracle and/or its affiliates.

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
#include <algorithm>
#include <cstddef>

#include "my_bitmap.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "sql/sql_bitmap.h"

namespace bitmap_unittest {

const int BITMAP_SIZE = 128;

class BitmapTest : public ::testing::Test {
 protected:
  BitmapTest() = default;

  void SetUp() override { bitmap.init(); }

  Bitmap<BITMAP_SIZE> bitmap;
};

TEST_F(BitmapTest, IntersectTest) {
  bitmap.set_prefix(4);
  bitmap.intersect(0xBBBBULL);
  EXPECT_TRUE(bitmap.is_set(0));
  EXPECT_TRUE(bitmap.is_set(1));
  EXPECT_FALSE(bitmap.is_set(2));
  EXPECT_TRUE(bitmap.is_set(3));
  bitmap.clear_bit(0);
  bitmap.clear_bit(1);
  bitmap.clear_bit(3);
  EXPECT_TRUE(bitmap.is_clear_all());
}

TEST_F(BitmapTest, ULLTest) {
  bitmap.set_all();
  bitmap.intersect(0x0123456789ABCDEFULL);
  ulonglong const ull = bitmap.to_ulonglong();
  EXPECT_TRUE(ull == 0x0123456789ABCDEFULL);

  Bitmap<24> bitmap24;
  bitmap24.init();
  bitmap24.set_all();
  bitmap24.intersect(0x47BULL);
  ulonglong const ull24 = bitmap24.to_ulonglong();
  EXPECT_TRUE(ull24 == 0x47BULL);
}

TEST_F(BitmapTest, GetFirstSet) {
  const Bitmap<64> const64bitmap(0);
  uint key = const64bitmap.get_first_set();
  EXPECT_EQ(MY_BIT_NONE, key);
  const Bitmap<BITMAP_SIZE> const128bitmap(0);
  key = const128bitmap.get_first_set();
  EXPECT_EQ(MY_BIT_NONE, key);
}

}  // namespace bitmap_unittest
