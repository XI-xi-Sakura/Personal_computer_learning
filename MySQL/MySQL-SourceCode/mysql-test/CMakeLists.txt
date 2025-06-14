# Copyright (c) 2009, 2025, Oracle and/or its affiliates.
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

INCLUDE(ProcessorCount)

# Skip -- Up-to-date messages in case of multiple 'make install'.
# This works for CMAKE 3.1.3 and up
SET(CMAKE_INSTALL_MESSAGE LAZY)

IF(INSTALL_MYSQLTESTDIR)
  INSTALL(
    DIRECTORY .
    DESTINATION ${INSTALL_MYSQLTESTDIR}
    USE_SOURCE_PERMISSIONS
    COMPONENT Test
    PATTERN "var" EXCLUDE
    PATTERN "var/" EXCLUDE
    PATTERN "lib/My/SafeProcess" EXCLUDE
    PATTERN "CPack" EXCLUDE
    PATTERN "CMake*" EXCLUDE
    PATTERN "Makefile" EXCLUDE
    PATTERN "mtr.out*" EXCLUDE
    PATTERN "*.cmake" EXCLUDE
    PATTERN "*.in" EXCLUDE
    )
  IF(NOT INSTALL_LAYOUT MATCHES "RPM" AND
      NOT INSTALL_LAYOUT MATCHES "DEB")
    INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE
      DESTINATION ${INSTALL_DOCREADMEDIR}
      COMPONENT TestReadme
      RENAME LICENSE-test
      )
    INSTALL(FILES ${CMAKE_SOURCE_DIR}/README
      DESTINATION ${INSTALL_DOCREADMEDIR}
      COMPONENT TestReadme
      RENAME README-test
      )
  ENDIF()
ENDIF()


IF(NOT ${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  # Enable running mtr from build directory
  FIND_PROGRAM(PERL_EXECUTABLE perl
    /usr/bin /usr/local/bin
    NO_CMAKE_ENVIRONMENT_PATH
    NO_SYSTEM_ENVIRONMENT_PATH)
  # If not found in hard-coded list above, look in PATH as well.
  FIND_PROGRAM(PERL_EXECUTABLE perl)

  CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/mtr.out-of-source 
    ${CMAKE_CURRENT_BINARY_DIR}/mysql-test-run.pl
    @ONLY
  )
ENDIF()
IF(UNIX)
  EXECUTE_PROCESS(
    COMMAND chmod +x  mysql-test-run.pl
    COMMAND ${CMAKE_COMMAND} -E create_symlink 
     ./mysql-test-run.pl mtr
    COMMAND ${CMAKE_COMMAND} -E create_symlink 
     ./mysql-test-run.pl mysql-test-run
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  ) 
  IF(INSTALL_MYSQLTESTDIR)
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mtr 
      ${CMAKE_CURRENT_BINARY_DIR}/mysql-test-run 
      DESTINATION ${INSTALL_MYSQLTESTDIR}
      COMPONENT Test)
  ENDIF()
ENDIF()


SET(EXP --experimental=collections/default.experimental)

SET(DEBUG_SERVER "")
SET(VS_CONFIG "")

IF(DEBUG_EXTNAME)
  SET(DEBUG_SERVER "--debug-server")
ENDIF()

IF(CMAKE_BUILD_TYPE)
  IF(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    SET(DEBUG_SERVER "--debug-server")
  ELSE()
    SET(DEBUG_SERVER "")
    IF(NOT BUILD_IS_SINGLE_CONFIG)
      SET(VS_CONFIG "--vs-config=${CMAKE_CFG_INTDIR}")
    ENDIF()
  ENDIF()
ENDIF()

# On windows do: 'cmake --build . --config debug --target test-mtr'
# On unix do: 'make test-mtr'
ADD_CUSTOM_TARGET(test-mtr
  COMMAND perl ./mysql-test-run.pl --force --parallel=4 --suite=main
          ${VS_CONFIG} ${DEBUG_SERVER} ${EXP}
)

SET(MTR_TESTS_FOR_PROFILING
  auth_sec
  binlog
  connection_control
  encryption
  gcol
  gis
  innodb
  json
  main
  sysschema
  thread_pool
  x
  component_connection_control
  )
STRING(REPLACE ";" "," MTR_SUITE_STRING "${MTR_TESTS_FOR_PROFILING}")

PROCESSORCOUNT(NPROC)
IF(NPROC EQUAL 0)
  SET(NPROC 8)
ENDIF()

ADD_CUSTOM_TARGET(run-profile-suite
  COMMAND perl ./mysql-test-run.pl
  --accept-test-fail
  --clean-vardir
  --force
  --max-test-fail=10
  --mem
  --mysqld=--binlog-format=row
  --parallel=${NPROC}
  --retry=0
  --skip-rpl
  --suite=${MTR_SUITE_STRING}

  USES_TERMINAL # So that Ninja will show progress, rather than buffer output.
  VERBATIM
  )

## We could do something like this, but the 'test' target generated
## by cmake is really for unit testing: all output is muted by default,
## even in case of test failure.
## ADD_EXECUTABLE(perl IMPORTED)
## ADD_TEST(xxx perl ./mysql-test-run.pl --force
##          ${VS_CONFIG} ${DEBUG_SERVER} ${EXP})


# With different MAX_INDEXES values, server might behave differently in
# certain cases. 'max_indexes.inc' file should be updated accordingly to
# reflect the current MAX_INDEXES value. This file helps MTR to decide on
# which tests should be skipped.
# NOTE: While committing a patch please make sure that the file is unmodified
# and should show the default MAX_INDEXES (i.e. 64U).
IF (MAX_INDEXES)
  IF(NOT (${MAX_INDEXES} EQUAL 64U))
    FILE(WRITE include/max_indexes.inc
         "# Warning: This is an auto-generated file. Please do not modify it.
--let $max_indexes = ${MAX_INDEXES}\n")
    MESSAGE(STATUS "mysql-test/include/max_indexes.inc adjusted")
  ENDIF()
ENDIF()
