/* Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#include "my_config.h"

#include <gtest/gtest.h>
#include <ctime>

#include "my_inttypes.h"
#include "my_systime.h"
#include "sql/sql_const.h"

namespace timespec_unittest {

class TimespecTest : public ::testing::Test {
 protected:
  /*
     Helper function which checks that none of the fields have overflowed.
   */
  void verify_timespec() {
    EXPECT_GT(ts.tv_sec, 0);
    EXPECT_GE(ts.tv_nsec, 0);
  }

  struct timespec ts;
};

/* Tests for set_timespec_nsec */

TEST_F(TimespecTest, TestNsecZero) {
  ulonglong const nsec = 0;
  set_timespec_nsec(&ts, nsec);
  verify_timespec();
}

TEST_F(TimespecTest, TestNsecMax) {
  set_timespec_nsec(&ts, TIMEOUT_INF);
  verify_timespec();
}

/* Tests for set_timespec (taking a seconds argument) */

TEST_F(TimespecTest, TestSecZero) {
  ulonglong const sec = 0;
  set_timespec(&ts, sec);
  verify_timespec();
}

TEST_F(TimespecTest, TestSec_LONG_TIMEOUT) {
  ulonglong const sec = LONG_TIMEOUT;
  set_timespec(&ts, sec);
  verify_timespec();
}

TEST_F(TimespecTest, TestSec_INT_MAX32) {
  ulonglong const sec = INT_MAX32;
  set_timespec(&ts, sec);
  verify_timespec();
}

TEST_F(TimespecTest, TestSec_UINT_MAX32) {
  ulonglong const sec = UINT_MAX32;
  set_timespec(&ts, sec);
  verify_timespec();
}
}  // namespace timespec_unittest
