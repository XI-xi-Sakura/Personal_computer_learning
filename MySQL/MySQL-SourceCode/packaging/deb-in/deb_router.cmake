# Copyright (c) 2017, 2025, Oracle and/or its affiliates.
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

# Enable various apparmor systems for Ubuntu
IF(DEB_ID STREQUAL "ubuntu")
  SET (DEB_RULES_INSTALL_APPARMOR_ROUTER "install -g root -o root -m 0644 -D debian/extra/apparmor-profile-router debian/tmp/etc/apparmor.d/usr.bin.mysqlrouter")
  SET (DEB_RULES_APPARMOR_LOAD_ROUTER "dh_apparmor -pmysql-router-${DEB_PRODUCTNAME} --profile-name=usr.bin.mysqlrouter")
  SET (DEB_INSTALL_ROUTER_APPARMOR "etc/apparmor.d/usr.bin.mysqlrouter")
ENDIF()
SET (DEB_RULES_INSTALLINIT_ROUTER "dh_installinit --name=mysqlrouter -- defaults 19 21")
SET (DEB_RULES_INSTALL_ROUTER_SYSTEMD
  "install -m 0755 debian/extra/mysqlrouter-systemd-start debian/tmp/usr/share/mysqlrouter/"
  )


if(DEFINED MRS_JIT_EXECUTOR_LIB)
  SET (DEB_CMAKE_EXTRAS "${DEB_CMAKE_EXTRAS} -DMRS_JIT_EXECUTOR_LIB=${MRS_JIT_EXECUTOR_LIB}")
  SET (DEB_INSTALL_MRS_JIT_EXECUTOR_PLUGIN "usr/lib/mysqlrouter/plugin/jit_executor.so")
  SET (DEB_INSTALL_MRS_JIT_EXECUTOR_LIB "usr/lib/mysqlrouter/private/libjitexecutor.so")
endif()

SET(DEB_CONTROL_ROUTER
"
Package: mysql-router-${DEB_PRODUCTNAME}
Architecture: any
Depends: \${shlibs:Depends}, \${misc:Depends}
Breaks: mysql-router-${DEB_PRODUCTNAME}
Replaces: mysql-router-${DEB_PRODUCTNAME}
Conflicts: mysql-router-${DEB_NOTPRODUCTNAME}, mysql-router (<< 8.0.3)
Description: MySQL Router
 The MySQL(TM) Router software delivers a fast, multi-threaded way of
 routing connections from MySQL Clients to MySQL Servers. MySQL is a
 trademark of Oracle.


Package: mysql-router
Architecture: any
Depends: mysql-router-${DEB_PRODUCTNAME}
Description: MySQL Router Metapackage
 The MySQL(TM) Router software delivers a fast, multi-threaded way of
 routing connections from MySQL Clients to MySQL Servers. MySQL is a
 trademark of Oracle. This is the shared router metapackage, used for
 dependency handling.

")
