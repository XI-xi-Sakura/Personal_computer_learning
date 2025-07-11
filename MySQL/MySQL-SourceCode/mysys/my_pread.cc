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

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file mysys/my_pread.cc
*/

#include "my_config.h"

#include <fcntl.h>
#include <sys/types.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <limits>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "my_base.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_sys.h"
#include "my_thread_local.h"
#include "mysys_err.h"
#if defined(_WIN32)
#include "mysys/mysys_priv.h"
#endif

#ifndef _WIN32
// Mock away pwrite() for unit testing.
ssize_t (*mock_pwrite)(int fd, const void *buf, size_t count,
                       off_t offset) = nullptr;
#endif

/**
  Read a chunk of bytes from a file from a given position.
  @note This differs from the normal pread() call in that we don't care
    to set the position in the file back to the original position
    if the system doesn't support pread().

    @param Filedes	File descriptor
    @param Buffer	Buffer to read data into
    @param Count	Number of bytes to read
    @param offset	Position to read from
    @param MyFlags	Flags

    @retval MY_FILE_ERROR in case of error
    @retval Number of bytes read, otherwise
*/

size_t my_pread(File Filedes, uchar *Buffer, size_t Count, my_off_t offset,
                myf MyFlags) {
  DBUG_TRACE;
  for (;;) {
    errno = 0; /* Linux, Windows don't reset this on EOF/success */

    const int64_t readbytes =
#if defined(_WIN32)
        my_win_pread(Filedes, Buffer, Count, offset);
#else
        pread(Filedes, Buffer, Count, offset);
#endif
    const bool error = (readbytes != static_cast<int64_t>(Count));
    if (error) {
      set_my_errno(errno ? errno : -1);
      if (errno == 0 || (readbytes != -1 && (MyFlags & (MY_NABP | MY_FNABP))))
        set_my_errno(HA_ERR_FILE_TOO_SHORT);

      if ((readbytes == 0 || readbytes == -1) && errno == EINTR) {
        continue; /* Interrupted */
      }

      if (MyFlags & (MY_WME | MY_FAE | MY_FNABP)) {
        if (readbytes == -1)
          MyOsError(my_errno(), EE_READ, MYF(0), my_filename(Filedes));
        else if (MyFlags & (MY_NABP | MY_FNABP))
          MyOsError(my_errno(), EE_EOFERR, MYF(0), my_filename(Filedes));
      }
      if (readbytes == -1 || (MyFlags & (MY_FNABP | MY_NABP)))
        return MY_FILE_ERROR;
    }                                             // if (error)
    if (MyFlags & (MY_NABP | MY_FNABP)) return 0; /* Read went ok; Return 0 */
    assert(readbytes >= 0);
    return readbytes; /* purecov: inspected */
  }                   // for (;;)
}

/**
  Write a chunk of bytes to a file at a given position
  @note
    This differs from the normal pwrite() call in that we don't care
    to set the position in the file back to the original position
    if the system doesn't support pwrite()

    @param Filedes	File descriptor
    @param Buffer	Buffer to write data from
    @param Count	Number of bytes to write
    @param offset	Position to write to
    @param MyFlags	Flags

  @return
    If Count == 0, returns 0. If failure, (size_t)-1 == MY_FILE_ERROR.
    Behavior otherwise depends on MyFlags:


    if (MyFlags & (MY_NABP | MY_FNABP))
      On success, 0
    otherwise
      On success or partial success, the number of bytes actually written
      (which can be less than Count).
*/

size_t my_pwrite(File Filedes, const uchar *Buffer, size_t Count,
                 my_off_t offset, myf MyFlags) {
  int64_t sum_written = 0;
  uint errors = 0;
  const size_t initial_count = Count;

  DBUG_TRACE;

  for (;;) {
    errno = 0;
    int64_t writtenbytes;
#if defined(_WIN32)
    writtenbytes = my_win_pwrite(Filedes, Buffer, Count, offset);
#else
    if (mock_pwrite)
      writtenbytes = mock_pwrite(Filedes, Buffer, Count, offset);
    else
      writtenbytes = pwrite(Filedes, Buffer, Count, offset);
#endif
    if (writtenbytes == static_cast<int64_t>(Count)) {
      sum_written += writtenbytes;
      break;
    }
    set_my_errno(errno);
    if (writtenbytes != -1) {
      sum_written += writtenbytes;
      Buffer += writtenbytes;
      Count -= writtenbytes;
      offset += writtenbytes;
    }

    if (is_killed_hook(nullptr))
      MyFlags &= ~MY_WAIT_IF_FULL; /* End if aborted by user */

    if ((my_errno() == ENOSPC || my_errno() == EDQUOT) &&
        (MyFlags & MY_WAIT_IF_FULL)) {
      wait_for_free_space(my_filename(Filedes), errors);
      errors++;
      continue;
    }
    if (writtenbytes != 0 && writtenbytes != -1) continue;
    if (my_errno() == EINTR) {
      continue; /* Retry */
    }
    if (writtenbytes == 0 && !errors++) /* Retry once */
    {
      /* We may come here if the file quota is exeeded */
      continue;
    }
    break; /* Return bytes written */
  }
  if (MyFlags & (MY_NABP | MY_FNABP)) {
    if (sum_written == static_cast<int64_t>(initial_count))
      return 0; /* Want only errors, not bytes written */
    if (MyFlags & (MY_WME | MY_FAE | MY_FNABP)) {
      MyOsError(my_errno(), EE_WRITE, MYF(0), my_filename(Filedes));
    }
    return MY_FILE_ERROR;
  }
  DBUG_EXECUTE_IF("check", my_seek(Filedes, -1, SEEK_SET, MYF(0)););

  if (sum_written == 0) return MY_FILE_ERROR;

  return sum_written;
}
