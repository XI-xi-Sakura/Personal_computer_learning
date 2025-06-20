/* Copyright (c) 2012, 2025, Oracle and/or its affiliates.

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
#include <cerrno>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "my_inttypes.h"
#include "my_sys.h"
#include "my_thread_local.h"
#include "mysql/psi/mysql_file.h"

#if !defined(_WIN32)
TEST(FileUtilsTest, TellPipe) {
  int pipefd[2];
  EXPECT_EQ(0, pipe(pipefd));
  my_off_t const pos = mysql_file_tell(pipefd[1], MYF(0));
  EXPECT_EQ(MY_FILEPOS_ERROR, pos);
  EXPECT_EQ(ESPIPE, my_errno());
  EXPECT_EQ(0, close(pipefd[0]));
  EXPECT_EQ(0, close(pipefd[1]));
}
#endif
