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

# Produce meaningful package name for the binary package
# The logic is rather involved with special cases for  different OSes

MACRO(GET_PACKAGE_FILE_NAME Var)
  IF(NOT VERSION)
    MESSAGE(FATAL_ERROR
     "Variable VERSION needs to be set prior to calling GET_PACKAGE_FILE_NAME")
  ENDIF()
  IF(NOT SYSTEM_NAME_AND_PROCESSOR)
    SET(NEED_DASH_BETWEEN_PLATFORM_AND_MACHINE 1)
    SET(DEFAULT_PLATFORM ${CMAKE_SYSTEM_NAME})
    SET(DEFAULT_MACHINE  ${CMAKE_SYSTEM_PROCESSOR})
    SET(64BIT 1)

    IF(WIN32)
      SET(NEED_DASH_BETWEEN_PLATFORM_AND_MACHINE 0)
      SET(DEFAULT_PLATFORM "win")
      SET(DEFAULT_MACHINE "x64")
    ELSEIF(SOLARIS)
      # SunOS 5.10=> solaris10
      STRING(REPLACE "5." "" VER "${CMAKE_SYSTEM_VERSION}")
      SET(DEFAULT_PLATFORM "solaris${VER}")
      SET(DEFAULT_MACHINE "${CMAKE_SYSTEM_PROCESSOR}-64bit")
    ELSEIF(FREEBSD)
      STRING(REGEX MATCH "[0-9]+\\.[0-9]+"  VER "${CMAKE_SYSTEM_VERSION}")
      SET(DEFAULT_PLATFORM "${CMAKE_SYSTEM_NAME}${VER}")
      IF(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64")
        SET(DEFAULT_MACHINE "x86_64")
      ENDIF()
    ELSEIF(APPLE)
      # CMAKE_SYSTEM_PROCESSOR seems to based on 'uname -r'
      # CMAKE_SYSTEM_VERSION cannot be trusted for version information:

      # CMAKE_SYSTEM_VERSION 19.2.0
      # sw_vers
      # ProductName:    Mac OS X
      # ProductVersion: 10.15.2
      # BuildVersion:   19C57

      # CMAKE_SYSTEM_VERSION  20.2.0
      # sw_vers
      # ProductName:    macOS
      # ProductVersion: 11.1
      # BuildVersion:   20C69

      EXECUTE_PROCESS(COMMAND sw_vers
        OUTPUT_VARIABLE SW_VERS_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        )
      STRING(REPLACE "\n" ";" SW_VERS_OUTPUT_LIST "${SW_VERS_OUTPUT}")
      LIST(GET SW_VERS_OUTPUT_LIST 0 SW_VERS_PRODUCTNAME)
      LIST(GET SW_VERS_OUTPUT_LIST 1 SW_VERS_PRODUCTVERSION)

      STRING(REGEX MATCH
        "ProductVersion:[\n\t ]*([0-9]+)\\.([0-9]+)" UNUSED ${SW_VERS_PRODUCTVERSION})
      IF(NOT DEFINED CMAKE_MATCH_1 OR NOT DEFINED CMAKE_MATCH_2)
        MESSAGE(FATAL_ERROR "Could not run sw_vers")
      ENDIF()

      SET(DEFAULT_PLATFORM "macos${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")

      MESSAGE(STATUS "DEFAULT_PLATFORM ${DEFAULT_PLATFORM}")

      IF(APPLE_ARM)
        SET(DEFAULT_MACHINE "arm64")
      ELSE()
        SET(DEFAULT_MACHINE "x86_64")
      ENDIF()
    ENDIF()

    IF(NOT PLATFORM)
      SET(PLATFORM ${DEFAULT_PLATFORM})
    ENDIF()
    IF(NOT MACHINE)
      SET(MACHINE ${DEFAULT_MACHINE})
    ENDIF()

    IF(NEED_DASH_BETWEEN_PLATFORM_AND_MACHINE)
      SET(SYSTEM_NAME_AND_PROCESSOR "${PLATFORM}-${MACHINE}")
    ELSE()
      SET(SYSTEM_NAME_AND_PROCESSOR "${PLATFORM}${MACHINE}")
    ENDIF()
  ENDIF()

  IF(SHORT_PRODUCT_TAG)
    SET(PRODUCT_TAG "-${SHORT_PRODUCT_TAG}")
  ELSEIF(MYSQL_SERVER_SUFFIX)
    SET(PRODUCT_TAG "${MYSQL_SERVER_SUFFIX}")  # Already has a leading dash
  ELSE()
    SET(PRODUCT_TAG)
  ENDIF()

  IF(WITH_NDB)
    SET(package_name "mysql${PRODUCT_TAG}-${MYSQL_CLUSTER_VERSION}-${SYSTEM_NAME_AND_PROCESSOR}")
  ELSE()
    SET(package_name "mysql${PRODUCT_TAG}-${VERSION}-${SYSTEM_NAME_AND_PROCESSOR}")
  ENDIF()

  MESSAGE(STATUS "Packaging as: ${package_name}")

  # Sometimes package suffix is added (something like "-icc-glibc23")
  IF(PACKAGE_SUFFIX)
    SET(package_name "${package_name}${PACKAGE_SUFFIX}")
  ENDIF()
  STRING(TOLOWER ${package_name} package_name)
  SET(${Var} ${package_name})
ENDMACRO()


IF(NOT CPACK_PACKAGE_FILE_NAME)
  GET_PACKAGE_FILE_NAME(CPACK_PACKAGE_FILE_NAME)
ENDIF()

IF(NOT CPACK_SOURCE_PACKAGE_FILE_NAME)
  SET(CPACK_SOURCE_PACKAGE_FILE_NAME "mysql-${VERSION}")
  IF(WITH_NDB)
    SET(CPACK_SOURCE_PACKAGE_FILE_NAME "mysql-cluster-gpl-${MYSQL_CLUSTER_VERSION}")
    MESSAGE(STATUS "MySQL Cluster package name: ${CPACK_SOURCE_PACKAGE_FILE_NAME}")
  ELSE()
  ENDIF()
ENDIF()
SET(CPACK_PACKAGE_CONTACT "MySQL Release Engineering <mysql-build@oss.oracle.com>")
SET(CPACK_PACKAGE_VENDOR "Oracle Corporation")
SET(CPACK_SOURCE_GENERATOR "TGZ")
INCLUDE(cpack_source_ignore_files)

# Defintions for windows version resources
SET(PRODUCTNAME "MySQL Server")
SET(COMPANYNAME ${CPACK_PACKAGE_VENDOR})

STRING(TIMESTAMP MYSQL_COPYRIGHT_YEAR "%Y")

# Add version information to the exe and dll files
# Refer to http://msdn.microsoft.com/en-us/library/aa381058(VS.85).aspx
# for more info.
IF(MSVC)
  GET_FILENAME_COMPONENT(MYSQL_CMAKE_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)

  IF(WITH_NDB)
    SET(VINFO_PRODUCT_NAME "MySQL NDB Cluster")
  ELSE()
    SET(VINFO_PRODUCT_NAME "MySQL Server")
  ENDIF()

  # Find the copyright line from the top README file
  FILE(STRINGS ${CMAKE_SOURCE_DIR}/README VINFO_COPYRIGHT_LINE
       ENCODING UTF-8 LIMIT_COUNT 1 REGEX "^Copyright")
  IF(NOT VINFO_COPYRIGHT_LINE)
    MESSAGE(FATAL_ERROR "Can't read copyright line from top README")
  ENDIF()
  MESSAGE(STATUS "Windows EXE/DLL file info copyright line: ${VINFO_COPYRIGHT_LINE}")

  SET(FILETYPE VFT_APP)
  CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in
    ${CMAKE_BINARY_DIR}/versioninfo_exe.rc)

  SET(FILETYPE VFT_DLL)
  CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in
    ${CMAKE_BINARY_DIR}/versioninfo_dll.rc)

  SET(VINFO_PRODUCT_NAME "MySQL Router")

  SET(FILETYPE VFT_APP)
  CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in
    ${CMAKE_BINARY_DIR}/router_versioninfo_exe.rc)

  SET(FILETYPE VFT_DLL)
  CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in
    ${CMAKE_BINARY_DIR}/router_versioninfo_dll.rc)

  SET(VERSION_INFO_RC_EXE_Router ${CMAKE_BINARY_DIR}/router_versioninfo_exe.rc)
  SET(VERSION_INFO_RC_DLL_Router ${CMAKE_BINARY_DIR}/router_versioninfo_dll.rc)

  # ADD_VERSION_INFO:
  #     add version info the executables/shared libraries on windows
  #
  # @param target_type  type of the target: SHARED|MODULE|EXE
  # @param sources_var  caller's variable name to append the rc-files to
  # @param component    component name
  #
  FUNCTION(ADD_VERSION_INFO target_type sources_var component)
    SET(exe_rc_file ${CMAKE_BINARY_DIR}/versioninfo_exe.rc)
    SET(dll_rc_file ${CMAKE_BINARY_DIR}/versioninfo_dll.rc)

    # Override default when VERSION_INFO_RC_[EXE|DLL]_{$component} defined
    IF(component)
      IF(VERSION_INFO_RC_EXE_${component})
        SET(exe_rc_file ${VERSION_INFO_RC_EXE_${component}})
      ENDIF()
      IF(VERSION_INFO_RC_DLL_${component})
        SET(dll_rc_file ${VERSION_INFO_RC_DLL_${component}})
      ENDIF()
    ENDIF()

    IF("${target_type}" MATCHES "SHARED" OR "${target_type}" MATCHES "MODULE")
      SET(rcfile ${dll_rc_file})
    ELSEIF("${target_type}" MATCHES "EXE")
      SET(rcfile ${exe_rc_file})
    ENDIF()
    SET(${sources_var} ${${sources_var}} ${rcfile} PARENT_SCOPE)
  ENDFUNCTION()
ELSE()
  FUNCTION(ADD_VERSION_INFO)
  ENDFUNCTION()
ENDIF()
