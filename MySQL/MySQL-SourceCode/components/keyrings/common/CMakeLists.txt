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

ADD_WSHADOW_WARNING()

# Use default value for -ftls-model which is global-dynamic with -fPIC
REMOVE_CMAKE_COMPILER_FLAGS(CMAKE_C_FLAGS "-ftls-model=initial-exec")
REMOVE_CMAKE_COMPILER_FLAGS(CMAKE_CXX_FLAGS "-ftls-model=initial-exec")

SET(KEYRING_COMMON_SOURCES
  # Component implementation helpers
  component_helpers/src/keyring_log_builtins_definition.cc
  # Config file reader
  config/config_reader.cc
  # Data representation
  data/data.cc
  data/meta.cc
  # File reader/writer
  data_file/reader.cc
  data_file/writer.cc
  # Encryption
  encryption/aes.cc
  # JSON reader/writer
  json_data/json_reader.cc
  json_data/json_writer.cc
  # Utilities
  utils/utils.cc
  )

SET (KEYRING_COMMON_DEPENDENCIES
  GenError
  )

IF(COMPONENT_COMPILE_VISIBILITY)
  SET(COMPILE_OPTIONS_ARG COMPILE_OPTIONS "${COMPONENT_COMPILE_VISIBILITY}")
ENDIF()

ADD_CONVENIENCE_LIBRARY(
  keyring_common
  ${KEYRING_COMMON_SOURCES}
  ${COMPILE_OPTIONS_ARG}
  DEPENDENCIES ${KEYRING_COMMON_DEPENDENCIES}
  SYSTEM_INCLUDE_DIRECTORIES
    $<TARGET_PROPERTY:extra::rapidjson,INTERFACE_INCLUDE_DIRECTORIES>
  COMPILE_DEFINITIONS
    $<TARGET_PROPERTY:extra::rapidjson,INTERFACE_COMPILE_DEFINITIONS>
  LINK_LIBRARIES library_mysys extra::rapidjson
)

IF(MY_COMPILER_IS_GNU AND FPROFILE_USE)
  ADD_COMPILE_FLAGS(
    component_helpers/src/keyring_log_builtins_definition.cc
    json_data/json_reader.cc
    COMPILE_FLAGS
    "-Wno-array-bounds"
    "-Wno-restrict"
    "-Wno-stringop-overflow"
    )
ENDIF()
