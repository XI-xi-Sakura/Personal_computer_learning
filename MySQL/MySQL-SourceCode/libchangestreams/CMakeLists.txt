# Copyright (c) 2021, 2025, Oracle and/or its affiliates.
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

OPTION(WITH_CS_PROTOBUF
  "Compile change streams library with protobuf based functionality" OFF)

ADD_WSHADOW_WARNING()
DISABLE_MISSING_PROFILE_WARNING()

SET(CS_BASE_SRC_DIR ${CMAKE_SOURCE_DIR}/libchangestreams/src)
SET(CS_LIB_PB_GENERATE_DIR ${CMAKE_BINARY_DIR}/libchangestreams/generated/lite/mysql/cs/codec/pb)
SET(CS_LIB_PB_SPEC_FILES ${CS_BASE_SRC_DIR}/lib/protobuf/reader_state.proto)
SET(CS_LIBS_ARCHIVE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/libchangestreams/lib/)

SET(CS_LIB_SOURCES
  ${CS_BASE_SRC_DIR}/lib/mysql/cs/reader/binary/mysqlproto.cpp
  ${CS_BASE_SRC_DIR}/lib/mysql/cs/reader/binary/tracker.cpp
  ${CS_BASE_SRC_DIR}/lib/mysql/cs/reader/state.cpp
  )

IF (WITH_CS_PROTOBUF)
  FILE(MAKE_DIRECTORY ${CS_LIB_PB_GENERATE_DIR})
  INCLUDE_DIRECTORIES(${CS_LIB_PB_GENERATE_DIR})

  MYSQL_PROTOBUF_GENERATE_CPP_LIBRARY(
    changestreams_pb_static
    ${CS_LIB_PB_SPEC_FILES}
    OUTPUT_DIRECTORY ${CS_LIB_PB_GENERATE_DIR}
    ADDITIONAL_COMMENT "- lite version")
  TARGET_LINK_LIBRARIES(changestreams_pb_static PUBLIC ext::libprotobuf-lite)

  SET_TARGET_PROPERTIES(changestreams_pb_static
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CS_LIBS_ARCHIVE_OUTPUT_DIR})

  LIST(APPEND CS_LIB_SOURCES
    ${CS_BASE_SRC_DIR}/lib/mysql/cs/codec/pb/reader_state_codec_pb.cpp)
ENDIF()


ADD_SUBDIRECTORY(src/lib)
ADD_SUBDIRECTORY(src/libstandalone)
ADD_SUBDIRECTORY(src/tests)

