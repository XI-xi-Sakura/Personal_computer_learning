/*
   Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MY_SYSTIME_INCLUDED
#define MY_SYSTIME_INCLUDED

/**
  @file include/my_systime.h
  Defines for getting and processing the current system type programmatically.
*/

#include <cassert>  // assert
#include <cstdint>  // std::int64_t
#include <ctime>    // time_t, struct timespec (C11/C++17)
#include <limits>   // std::numeric_limits
#include "my_config.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>  // clock_gettime()
#endif                 /* HAVE_SYS_TIME_H */

#ifdef _WIN32
#include <winsock2.h>  // struct timeval
#endif                 // _WIN32

#if !defined(HAVE_CLOCK_GETTIME) || !defined(HAVE_GETTIMEOFDAY)
#include <chrono>
using UTC_clock = std::chrono::system_clock;
#endif

#include "my_time_t.h"

struct timeval;

/* Bits for get_date timeflag */
constexpr const int GETDATE_DATE_TIME = 1;
constexpr const int GETDATE_SHORT_DATE = 2;
constexpr const int GETDATE_HHMMSSTIME = 4;
constexpr const int GETDATE_GMT = 8;
constexpr const int GETDATE_FIXEDLENGTH = 16;
constexpr const int GETDATE_T_DELIMITER = 32;
constexpr const int GETDATE_SHORT_DATE_FULL_YEAR = 64;

/**
   Wait a given number of microseconds.

   @param micro_seconds number of microseconds to wait.
*/
void my_sleep(int64_t micro_seconds);

#ifdef _WIN32

#include <windows.h>

/****************************************************************************
** Replacements for localtime_r and gmtime_r
****************************************************************************/

inline struct tm *localtime_r(const time_t *timep, struct tm *tmp) {
  localtime_s(tmp, timep);
  return tmp;
}

inline struct tm *gmtime_r(const time_t *clock, struct tm *res) {
  gmtime_s(res, clock);
  return res;
}

/**
   Sleep the given number of seconds. POSIX compatibility.

   @param seconds number of seconds to wait
*/
void sleep(unsigned long seconds);

#endif /* _WIN32 */

/**
  Get high-resolution time. Forwards to std::chrono on Windows.

  @return current high-resolution time in multiples of 100 nanoseconds.
*/
inline unsigned long long int my_getsystime() {
#ifdef HAVE_CLOCK_GETTIME
  // Performance regression testing showed this to be preferable
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  return (static_cast<unsigned long long int>(tp.tv_sec) * 10000000 +
          static_cast<unsigned long long int>(tp.tv_nsec) / 100);
#else
  return std::chrono::duration_cast<
             std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
             UTC_clock::now().time_since_epoch())
      .count();
#endif /* HAVE_CLOCK_GETTIME */
}

/**
   The maximum timespec value used to represent "inifinity" (as when
   requesting an "infinite" timeout.
 */
constexpr const timespec TIMESPEC_POSINF = {
    std::numeric_limits<decltype(timespec::tv_sec)>::max(), 999999999};

/** Type alias to reduce chance of conversion errors on timeout values. */
using Timeout_type = std::uint64_t;

/** Value representing "infinite" timeout. */
constexpr const Timeout_type TIMEOUT_INF =
    std::numeric_limits<Timeout_type>::max() - 1;

void set_timespec_nsec(struct timespec *abstime, Timeout_type nsec);
void set_timespec(struct timespec *abstime, Timeout_type sec);

/**
   Compare two timespec structs.

   @retval  1 If ts1 ends after ts2.
   @retval -1 If ts1 ends before ts2.
   @retval  0 If ts1 is equal to ts2.
*/
inline int cmp_timespec(struct timespec *ts1, struct timespec *ts2) {
  if (ts1->tv_sec > ts2->tv_sec ||
      (ts1->tv_sec == ts2->tv_sec && ts1->tv_nsec > ts2->tv_nsec))
    return 1;
  if (ts1->tv_sec < ts2->tv_sec ||
      (ts1->tv_sec == ts2->tv_sec && ts1->tv_nsec < ts2->tv_nsec))
    return -1;
  return 0;
}

/**
   Calculate the diff between two timespec values.

   @return difference in nanoseconds between ts1 and ts2
*/
inline unsigned long long int diff_timespec(struct timespec *ts1,
                                            struct timespec *ts2) {
  return (ts1->tv_sec - ts2->tv_sec) * 1000000000ULL + ts1->tv_nsec -
         ts2->tv_nsec;
}

/**
  Return current time. Takes an int argument
  for backward compatibility. This argument is ignored.

  @deprecated New code should use std::time() directly.

  @retval current time.
*/
[[deprecated]] inline time_t my_time(int) { return time(nullptr); }

/**
  Return time in microseconds. Uses std::chrono::high_resolution_clock

  @remark This function is to be used to measure performance in
          micro seconds.

  @retval Number of microseconds since the Epoch, 1970-01-01 00:00:00 +0000
  (UTC)
*/
inline unsigned long long int my_micro_time() {
#ifdef HAVE_GETTIMEOFDAY
  struct timeval t;
  /*
  The following loop is here because gettimeofday may fail on some systems
  */
  while (gettimeofday(&t, nullptr) != 0) {
  }
  return (static_cast<unsigned long long int>(t.tv_sec) * 1000000 + t.tv_usec);
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
             UTC_clock::now().time_since_epoch())
      .count();
#endif  // HAVE_GETTIMEOFDAY
}

/**
  Convert microseconds since epoch to my_timeval.
  @param      micro_time  Microseconds.
  @param[out] tm          A timeval variable to write to.
*/
inline void my_micro_time_to_timeval(std::uint64_t micro_time, my_timeval *tm) {
  tm->m_tv_sec = static_cast<int64_t>(micro_time / 1000000);
  tm->m_tv_usec = static_cast<int64_t>(micro_time % 1000000);
}

/**

 Convert microseconds since epoch to timeval. Prefer

   my_micro_time_to_timeval(std::uint64_t micro_time, my_timeval *tm)

 which is 64 bits safe on all platforms: Window's timeval's long
 members are only 32 bits. Unless you need to use the host system's
 struct timeval, of course.

 @param      micro_time  Microseconds.
 @param[out] tm          A timeval variable to write to.
 */
inline void my_micro_time_to_timeval(std::uint64_t micro_time, timeval *tm) {
  assert(static_cast<std::int64_t>(micro_time / 1000000) <=
         std::numeric_limits<long>::max());
  tm->tv_sec = static_cast<long>(micro_time / 1000000);
  tm->tv_usec = static_cast<long>(micro_time % 1000000);
}

void get_date(char *to, int flag, time_t date);

#endif  // MY_SYSTIME_INCLUDED
