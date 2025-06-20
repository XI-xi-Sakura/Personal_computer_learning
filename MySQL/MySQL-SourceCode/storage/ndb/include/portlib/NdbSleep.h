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

#ifndef NDBSLEEP_H
#define NDBSLEEP_H

#include <ndb_global.h>
#include "my_config.h"
#include "ndb_config.h"

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if defined(HAVE_NANOSLEEP)
#include <time.h>
#endif

static inline void NdbSleep_MilliSleep(int milliseconds);

static inline void NdbSleep_MicroSleep(int microseconds) {
  assert(0 < microseconds);
#ifdef _WIN32
  // Waitable timer use 100ns time unit, negative for relative time periods
  LARGE_INTEGER liDueTime;
  liDueTime.QuadPart = -10LL * microseconds;

  HANDLE hTimer = CreateWaitableTimer(nullptr, true, nullptr);
  if (nullptr == hTimer ||
      !SetWaitableTimer(hTimer, &liDueTime, 0, nullptr, nullptr, 0) ||
      WaitForSingleObject(hTimer, INFINITE) != WAIT_OBJECT_0) {
#ifndef NDEBUG
    // Error code for crash analysis
    DWORD winerr [[maybe_unused]] = GetLastError();
#endif
    assert(false);
    // Fallback to millisleep in release
    NdbSleep_MilliSleep(1 + (microseconds - 1) / 1000);
  }
  if (nullptr != hTimer) {
    CloseHandle(hTimer);
  }
#elif defined(HAVE_NANOSLEEP)
  struct timespec t;
  t.tv_sec = microseconds / 1000000;
  t.tv_nsec = 1000 * (microseconds % 1000000);
  while (nanosleep(&t, &t) == -1) {
    if (errno != EINTR) {
      assert(false);
      // Fallback to millisleep in release
      NdbSleep_MilliSleep(1 + (microseconds - 1) / 1000);
      return;
    }
  }
#else
  // Fallback to millisleep
  NdbSleep_MilliSleep(1 + (microseconds - 1) / 1000);
#endif
}

static inline void NdbSleep_MilliSleep(int milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timeval t;
  t.tv_sec = milliseconds / 1000L;
  t.tv_usec = (milliseconds % 1000L) * 1000L;
  select(0, nullptr, nullptr, nullptr, &t);
#endif
}

static inline void NdbSleep_SecSleep(int seconds) {
  NdbSleep_MilliSleep(seconds * 1000);
}

#endif
