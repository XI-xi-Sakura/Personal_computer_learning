/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>

#include <logger/Logger.hpp>
#include "common.hpp"

#ifndef _WIN32
#include <pwd.h>
#endif

#include <BaseString.hpp>
#include <Properties.hpp>

int debug = 0;

Logger logger;

#ifndef _WIN32
int runas(const char *user) {
  if (user == 0 || strlen(user) == 0) {
    return 0;
  }
  struct passwd *pw = getpwnam(user);
  if (pw == 0) {
    logger.error("Can't find user to %s", user);
    return -1;
  }
  uid_t uid = pw->pw_uid;
  gid_t gid = pw->pw_gid;
  int res = setgid(gid);
  if (res != 0) {
    logger.error("Can't change group to %s(%d)", user, gid);
    return res;
  }

  res = setuid(uid);
  if (res != 0) {
    logger.error("Can't change user to %s(%d)", user, uid);
  }
  return res;
}
#endif

int insert(const char *pair, Properties &p) {
  BaseString tmp(pair);

  tmp.trim(" \t\n\r");

  Vector<BaseString> split;
  tmp.split(split, ":=", 2);

  if (split.size() != 2) return -1;

  p.put(split[0].trim().c_str(), split[1].trim().c_str());

  return 0;
}
