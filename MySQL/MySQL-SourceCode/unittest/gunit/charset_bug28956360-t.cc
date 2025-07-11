/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include "my_sys.h"
#include "mysql/strings/m_ctype.h"

namespace charset_bug28956360_unittest {

TEST(CharsetBug28956360Unittest, UninitOnce) {
  charset_uninit();
  CHARSET_INFO *ci = get_charset_by_csname("utf8mb4", MY_CS_PRIMARY, MY_WME);
  EXPECT_NE(nullptr, ci);
}

TEST(CharsetBug28956360Unittest, UninitManyTimes) {
  for (int i = 0; i < 100; ++i) {
    charset_uninit();
    CHARSET_INFO *ci = get_charset_by_csname("utf8mb4", MY_CS_PRIMARY, MY_WME);
    EXPECT_NE(nullptr, ci);
  }
}

}  // namespace charset_bug28956360_unittest
