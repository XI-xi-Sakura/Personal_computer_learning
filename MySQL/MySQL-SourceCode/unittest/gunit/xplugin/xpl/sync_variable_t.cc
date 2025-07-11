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

#include <gtest/gtest.h>

#include <cstddef>
#include <thread>  // NOLINT(build/c++11)

#include "my_sys.h"      // NOLINT(build/include_subdir)
#include "my_systime.h"  // my_sleep() NOLINT(build/include_subdir)

#include "plugin/x/src/helper/multithread/sync_variable.h"
#include "plugin/x/src/ngs/thread.h"

namespace xpl::test {

const int EXPECTED_VALUE_FIRST = 10;
const int EXPECTED_VALUE_SECOND = 20;
const int EXPECTED_VALUE_THRID = 30;
const int EXPECTED_VALUE_SET = 40;
const int EXPECTED_VALUE_SET_EXPECT = 50;

class Xpl_sync_variable : public ::testing::Test {
 public:
  Xpl_sync_variable()
      : m_sut(EXPECTED_VALUE_FIRST, PSI_NOT_INSTRUMENTED, PSI_NOT_INSTRUMENTED),
        m_thread_ended(false) {}

  static void *start_routine_set(void *data) {
    auto *self = static_cast<Xpl_sync_variable *>(data);
    self->set_value();

    return nullptr;
  }

  static void *start_routine_set_and_expect(void *data) {
    auto *self = static_cast<Xpl_sync_variable *>(data);
    self->set_value();
    self->m_sut.wait_for(EXPECTED_VALUE_SET_EXPECT);

    return nullptr;
  }

  void set_value() {
    my_sleep(1);
    m_thread_ended = true;
    m_sut.set(EXPECTED_VALUE_SET);
  }

  Sync_variable<int> m_sut;

  volatile bool m_thread_ended;
};

TEST_F(Xpl_sync_variable, is_returnConstructorInitializedValue) {
  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_FIRST));
}

TEST_F(Xpl_sync_variable, is_returnChangedValue) {
  m_sut.set(EXPECTED_VALUE_SECOND);

  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_SECOND));
}

TEST_F(Xpl_sync_variable, is_returnChangedValue_afterSetWasCalled) {
  m_sut.set(EXPECTED_VALUE_SECOND);

  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_SECOND));
}

TEST_F(Xpl_sync_variable, is_exchangeSuccesses_whenCurrentValueMatches) {
  ASSERT_TRUE(m_sut.exchange(EXPECTED_VALUE_FIRST, EXPECTED_VALUE_SECOND));
  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_SECOND));
}

TEST_F(Xpl_sync_variable, is_exchangeFails_whenCurrentValueDoesntMatches) {
  ASSERT_FALSE(m_sut.exchange(EXPECTED_VALUE_THRID, EXPECTED_VALUE_SECOND));
  ASSERT_FALSE(m_sut.is(EXPECTED_VALUE_SECOND));
  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_FIRST));
}

TEST_F(Xpl_sync_variable, wait_returnsRightAway_whenCurrentValueMatches) {
  m_sut.wait_for(EXPECTED_VALUE_FIRST);
}

TEST_F(Xpl_sync_variable,
       wait_returnsRightAway_whenCurrentValueInArrayMatches) {
  int const VALUES[] = {EXPECTED_VALUE_SECOND, EXPECTED_VALUE_FIRST};
  m_sut.wait_for(VALUES);
}

TEST_F(Xpl_sync_variable, wait_returnsRightAway_whenNewValueMatches) {
  m_sut.set(EXPECTED_VALUE_SECOND);
  m_sut.wait_for(EXPECTED_VALUE_SECOND);
}

TEST_F(Xpl_sync_variable, set_returnsOldValue) {
  ASSERT_EQ(EXPECTED_VALUE_FIRST,
            m_sut.set_and_return_old(EXPECTED_VALUE_SET_EXPECT));
  ASSERT_EQ(EXPECTED_VALUE_SET_EXPECT,
            m_sut.set_and_return_old(EXPECTED_VALUE_SECOND));
  ASSERT_EQ(EXPECTED_VALUE_SECOND,
            m_sut.set_and_return_old(EXPECTED_VALUE_FIRST));
}

TEST_F(Xpl_sync_variable,
       wait_returnsRightAway_whenNewCurrentValueInArrayMatches) {
  int const VALUES[] = {EXPECTED_VALUE_SECOND, EXPECTED_VALUE_FIRST};
  m_sut.set(EXPECTED_VALUE_SECOND);
  m_sut.wait_for(VALUES);
}

TEST_F(Xpl_sync_variable,
       wait_returnsDelayed_whenThreadChangesValueAndItsExpected) {
  std::thread t(&Xpl_sync_variable::start_routine_set, this);
  m_sut.wait_for(EXPECTED_VALUE_SET);
  t.join();

  ASSERT_TRUE(m_thread_ended);  // Verify that the exit was triggerd by thread
}

TEST_F(
    Xpl_sync_variable,
    wait_returnsDelayed_whenThreadChangesValueAndItsInArrayOfExpectedValues) {
  std::thread t(&Xpl_sync_variable::start_routine_set_and_expect, this);
  int const VALUES[] = {EXPECTED_VALUE_SET};
  m_sut.wait_for_and_set(VALUES, EXPECTED_VALUE_SET_EXPECT);
  t.join();

  ASSERT_TRUE(m_thread_ended);  // Verify that the exit was triggerd by thread
  ASSERT_TRUE(m_sut.is(EXPECTED_VALUE_SET_EXPECT));
}

}  // namespace xpl::test
