/*
   Copyright (c) 2007, 2025, Oracle and/or its affiliates.

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
#include <cstring>
#include "my_sys.h"
#include "my_thread.h"

#include <errno.h>

#include "AsyncFile.hpp"
#include "PosixAsyncFile.hpp"
#include "my_thread_local.h"

#include <NdbThread.h>
#include <kernel_types.h>
#include <ErrorHandlingMacros.hpp>
#include <ndbd_malloc.hpp>
#include <signaldata/FsOpenReq.hpp>
#include <signaldata/FsReadWriteReq.hpp>
#include <signaldata/FsRef.hpp>

#include <NdbTick.h>

#include <dirent.h>

#include <EventLogger.hpp>

#define JAM_FILE_ID 384

PosixAsyncFile::PosixAsyncFile(Ndbfs &fs) : AsyncFile(fs) {}

void PosixAsyncFile::removeReq(Request *request) {
#if TEST_UNRELIABLE_DISTRIBUTED_FILESYSTEM
  // Sometimes inject double file delete
  if (check_inject_and_log_extra_remove(theFileName.c_str()))
    ::remove(theFileName.c_str());
#endif
  if (-1 == ::remove(theFileName.c_str())) {
#if UNRELIABLE_DISTRIBUTED_FILESYSTEM
    if (check_and_log_if_remove_failure_ok(theFileName.c_str())) return;
#endif
    NDBFS_SET_REQUEST_ERROR(request, errno);
  }
}

void PosixAsyncFile::rmrfReq(Request *request, const char *src,
                             bool removePath) {
  if (!request->par.rmrf.directory) {
    // Remove file
    if (unlink(src) != 0 && errno != ENOENT)
      NDBFS_SET_REQUEST_ERROR(request, errno);
    return;
  }

  char path[PATH_MAX];
  strcpy(path, src);
  strcat(path, "/");

  DIR *dirp;
  struct dirent *dp;
loop:
  dirp = opendir(path);
  if (dirp == 0) {
    if (errno != ENOENT) NDBFS_SET_REQUEST_ERROR(request, errno);
    return;
  }

  while ((dp = readdir(dirp)) != NULL) {
    if ((strcmp(".", dp->d_name) != 0) && (strcmp("..", dp->d_name) != 0)) {
      int len = strlen(path);
      strcat(path, dp->d_name);
#if TEST_UNRELIABLE_DISTRIBUTED_FILESYSTEM
      // Sometimes inject double file delete
      if (check_inject_and_log_extra_remove(path)) remove(path);
#endif
      bool removed = (remove(path) == 0);
#if UNRELIABLE_DISTRIBUTED_FILESYSTEM
      if (!removed) {
        if (check_and_log_if_remove_failure_ok(path)) {
          removed = true;
        }
      }
#endif
      if (removed) {
        path[len] = 0;
        continue;
      }

      closedir(dirp);
      strcat(path, "/");
      goto loop;
    }
  }
  closedir(dirp);
  path[strlen(path) - 1] = 0;  // remove /
  if (strcmp(src, path) != 0) {
    char *t = strrchr(path, '/');
    t[1] = 0;
    goto loop;
  }

#if TEST_UNRELIABLE_DISTRIBUTED_FILESYSTEM
  // Sometimes inject double file delete
  if (removePath && check_inject_and_log_extra_remove(src)) rmdir(src);
#endif
  if (removePath && rmdir(src) != 0) {
#if UNRELIABLE_DISTRIBUTED_FILESYSTEM
    if (check_and_log_if_remove_failure_ok(src)) return;
#endif
    NDBFS_SET_REQUEST_ERROR(request, errno);
  }
}

void PosixAsyncFile::createDirectories() {
  char *tmp;
  const char *name = theFileName.c_str();
  const char *base = theFileName.get_base_name();
  while ((tmp = (char *)strstr(base, DIR_SEPARATOR))) {
    char t = tmp[0];
    tmp[0] = 0;
    mkdir(name, S_IRUSR | S_IWUSR | S_IXUSR | S_IXGRP | S_IRGRP);
    tmp[0] = t;
    base = tmp + sizeof(DIR_SEPARATOR);
  }
}
