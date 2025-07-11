# Copyright (c) 2008, 2025, Oracle and/or its affiliates.
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

# Disable specific types of warnings for current directory
# if the compiler supports the flag

INCLUDE(ndb_downgrade_warnings)

IF(MY_COMPILER_IS_GNU_OR_CLANG)
  INCLUDE(CheckCXXCompilerFlag)
  FOREACH(warning
      "cast-qual"
      "maybe-uninitialized"
      "restrict"
      "unused-but-set-variable"
      "unused-parameter"
      )
    MY_CHECK_CXX_COMPILER_WARNING("${warning}" HAS_WARN_FLAG)
    IF(HAS_WARN_FLAG)
      SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-${warning}")
      SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-${warning}")
    ENDIF()
  ENDFOREACH()
ENDIF()

ADD_SUBDIRECTORY(src)
IF(WITH_NDB_TEST)
  ADD_SUBDIRECTORY(tools)
  ADD_SUBDIRECTORY(ndbapi)
  ADD_SUBDIRECTORY(run-test)
  ADD_SUBDIRECTORY(crund)
ENDIF(WITH_NDB_TEST)
