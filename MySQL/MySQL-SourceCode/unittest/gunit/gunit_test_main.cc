/* Copyright (c) 2009, 2025, Oracle and/or its affiliates.

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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/time.h>
#endif
#include <sys/types.h>

#include "gunit_test_main.h"

#include "my_getopt.h"
#include "my_inttypes.h"
#include "my_stacktrace.h"
#include "my_sys.h"
#include "my_thread_local.h"
#include "mysql/psi/mysql_mutex.h"

class Cost_constant_cache;
class THD;

struct CHARSET_INFO;

CHARSET_INFO *system_charset_info = nullptr;

#ifdef _WIN32
#define SIGNAL_FMT "exception 0x%x"
#else
#define SIGNAL_FMT "signal %d"
#endif

static void signal_handler(int sig) {
  my_safe_printf_stderr("unit test got " SIGNAL_FMT "\n", sig);

// Alpine Linux does not have backtrace.
#ifdef HAVE_STACKTRACE
  my_print_stacktrace(nullptr, 0);
#endif

  exit(EXIT_FAILURE);
}

#ifdef _WIN32

LONG WINAPI exception_filter(EXCEPTION_POINTERS *exp) {
  __try {
    my_set_exception_pointers(exp);
    signal_handler(exp->ExceptionRecord->ExceptionCode);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    fputs("Got exception in exception handler!\n", stderr);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

static void init_signal_handling() {
  /* Set output destination of messages to the standard error stream. */
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

  /* Do not not display the a error message box. */
  UINT mode = SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
  SetErrorMode(mode);

  SetUnhandledExceptionFilter(exception_filter);
}

#else

static void init_signal_handling() {
// Alpine Linux does not have backtrace.
#ifdef HAVE_STACKTRACE
  my_init_stacktrace();
#endif

  struct sigaction sa;
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigprocmask(SIG_SETMASK, &sa.sa_mask, nullptr);

  sa.sa_handler = signal_handler;
  // Treat these as fatal and handle them.
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  // Handle these as well, except for ASAN/UBSAN builds:
  // we let sanitizer runtime handle them instead.
#if defined(HANDLE_FATAL_SIGNALS)
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
#endif
}

#endif

// Some globals needed for "small" tests.
mysql_mutex_t LOCK_open;
uint opt_debug_sync_timeout = 0;
thread_local MEM_ROOT **THR_MALLOC = nullptr;
thread_local THD *current_thd = nullptr;
size_t malloc_chunk_size = 1024;

extern "C" void sql_alloc_error_handler(void) { ADD_FAILURE(); }

int compare_malloc_chunks(void *a, void *b, size_t sz) {
  return memcmp(a, b, sz);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::InitGoogleMock(&argc, argv);
  MY_INIT(argv[0]);

  mysql_mutex_init(PSI_NOT_INSTRUMENTED, &LOCK_open, MY_MUTEX_INIT_FAST);

#ifndef _WIN32
  rlimit core_limit;
  core_limit.rlim_cur = 0;
  core_limit.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &core_limit);
#endif

  init_signal_handling();

  const int retval = RUN_ALL_TESTS();
  mysql_mutex_destroy(&LOCK_open);
  my_end(0);
  return retval;
}
