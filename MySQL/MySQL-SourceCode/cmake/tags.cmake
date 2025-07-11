# Copyright (c) 2010, 2025, Oracle and/or its affiliates.
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

# Generate tag files
IF(UNIX)
  FIND_PROGRAM(CTAGS_EXECUTABLE ctags)
  IF(NOT CTAGS_EXECUTABLE)
    RETURN()
  ENDIF()
  IF(APPLE AND CTAGS_EXECUTABLE STREQUAL "/usr/bin/ctags")
    MESSAGE(STATUS "Install Homebrew ctags to generate tags targets")
  ELSE()
    EXECUTE_PROCESS(
      COMMAND ${CTAGS_EXECUTABLE} --version OUTPUT_VARIABLE CTAGS_VERSION)
  ENDIF()

  IF(CTAGS_VERSION MATCHES "Exuberant")
    ADD_CUSTOM_TARGET(tags
      COMMAND support-files/build-tags
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )
    ADD_CUSTOM_TARGET(ctags
      COMMAND support-files/build-tags ctags
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )
  ELSE()
    ADD_CUSTOM_TARGET (tags
      COMMAND exit 1
      COMMENT "Please install Exuberant Ctags"
      )
    ADD_CUSTOM_TARGET (ctags
      COMMAND exit 1
      COMMENT "Please install Exuberant Ctags"
      )
  ENDIF()
ENDIF()
