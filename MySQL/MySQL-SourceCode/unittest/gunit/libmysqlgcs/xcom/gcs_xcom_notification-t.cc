/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#include <ctime>

#include "gcs_base_test.h"

#include "gcs_xcom_notification.h"

namespace gcs_xcom_notification_unittest {
class XcomNotificationTest : public GcsBaseTest {};

void function(int &val) { val += 1; }

class Dummy_notification : public Parameterized_notification<false> {
 public:
  Dummy_notification(void (*functor)(int &), int &val)
      : m_functor(functor), m_val(val) {}

  ~Dummy_notification() override = default;

  void (*m_functor)(int &);
  int &m_val;

 private:
  void do_execute() override { (*m_functor)(m_val); }
};

static int var = 0;
static void cleanup() { var += 1; }

TEST_F(XcomNotificationTest, ProcessDummyNotification) {
  int val = 0;
  auto *engine = new Gcs_xcom_engine();

  ASSERT_EQ(val, 0);

  engine->initialize(nullptr);
  engine->push(new Dummy_notification(&function, val));
  engine->finalize(nullptr);
  delete engine;

  ASSERT_EQ(val, 1);
}

TEST_F(XcomNotificationTest, ProcessFinalizeNotification) {
  auto *engine = new Gcs_xcom_engine();

  ASSERT_EQ(var, 0);

  engine->initialize(nullptr);
  engine->finalize(cleanup);
  delete engine;

  ASSERT_EQ(var, 1);
}
}  // namespace gcs_xcom_notification_unittest
