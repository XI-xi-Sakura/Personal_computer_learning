# Copyright (c) 2020, 2025, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA


GET_DIRECTORY_PROPERTY(MYSQLX_CLIENT_FULL_LIB
  DIRECTORY ${MYSQLX_PROJECT_DIR}
  DEFINITION MYSQLX_CLIENT_FULL_LIB
)

GET_DIRECTORY_PROPERTY(MYSQLX_PROTOCOL_FULL_LIB
  DIRECTORY ${MYSQLX_PROJECT_DIR}
  DEFINITION MYSQLX_PROTOCOL_FULL_LIB
)

SET(XCL_UNIT_TESTS xclient_unit_tests)

INCLUDE(source_files.cmake)

# Fixes "C1128: number of sections exceeded object file format limit" in MSVC
IF(MSVC)
  ADD_COMPILE_FLAGS(
    mock/mock.cc
    COMPILE_FLAGS "/bigobj"
    )
ENDIF()

MYSQL_ADD_EXECUTABLE(${XCL_UNIT_TESTS}
  ../test_main.cc
  ${XCL_TEST_SRC}
  ADD_TEST xclient
)
SET_TESTS_PROPERTIES(xclient PROPERTIES TIMEOUT 180)

ADD_DEPENDENCIES(${XCL_UNIT_TESTS}
  ${MYSQLX_CLIENT_FULL_LIB}
  ${MYSQLX_PROTOCOL_FULL_LIB}
  xprotocol_tags
)

TARGET_LINK_LIBRARIES(${XCL_UNIT_TESTS}
  ${GCOV_LDFLAGS}
  ${MYSQLX_CLIENT_FULL_LIB}
  ${MYSQLX_PROTOCOL_FULL_LIB}
  harness_net_ts
  ext::libprotobuf
  ext::lz4
  mysqlclient
  gtest
  gmock
)
