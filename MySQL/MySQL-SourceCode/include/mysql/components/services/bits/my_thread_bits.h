/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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

#ifndef COMPONENTS_SERVICES_BITS_MY_THREAD_BITS_H
#define COMPONENTS_SERVICES_BITS_MY_THREAD_BITS_H

/**
  @file mysql/components/services/bits/my_thread_bits.h
  Types to make different thread packages compatible.
*/

#ifndef MYSQL_ABI_CHECK
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>  // IWYU pragma: export
#include <sched.h>    // IWYU pragma: export
#endif
#endif /* MYSQL_ABI_CHECK */

#if defined(_WIN32) && !defined(MYSQL_ABI_CHECK)
typedef DWORD my_thread_t;
typedef struct thread_attr {
  DWORD dwStackSize;
  int detachstate;
} my_thread_attr_t;
#else
typedef pthread_t my_thread_t;
typedef pthread_attr_t my_thread_attr_t;
#endif

// Linux: unsigned long int
// Solaris: unsigned int
// Windows: DWORD
// Mac:__darwin_pthread_t ==  _opaque_pthread_t*
static constexpr my_thread_t null_thread_initializer = my_thread_t{};

struct my_thread_handle {
  my_thread_t thread{null_thread_initializer};
#if defined(_WIN32) && !defined(MYSQL_ABI_CHECK)
  HANDLE handle{INVALID_HANDLE_VALUE};
#endif
};

#endif /* COMPONENTS_SERVICES_BITS_MY_THREAD_BITS_H */
