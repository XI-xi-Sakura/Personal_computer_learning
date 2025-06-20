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

#include <cstddef>

#ifdef _WIN32
#include "mysql/strings/m_ctype.h"
#endif
#include "my_dbug.h"
#include "my_io.h"
#include "my_sys.h"  // IWYU pragma: keep
#include "strmake.h"

/**
  @file mysys/mf_dirname.cc
*/

/**
  Get the string length of the directory part of name, including the
  last FN_LIBCHAR. If name is not a path, return 0.

  On Windows, we special-case Shift-JIS (or more specifically,
  Windows code-page 932).

  Everything else (i.e. anything that's not CP932 and on Windows)
  is considered binary. Specifically, we assume that drive and directory
  separators are single-byte (and as defined by FN_LIBCHAR / FN_LIBCHAR2),
  and that the string is '\0' terminated. Therefore, latin-*, UTF-8, and
  other arguments that behave sufficiently like ASCII will work, whereas
  e.g. UTF-16 would break as the separators would have '\0' as part of the
  UTF-16 character. Fortunately, CP932 does not use '\0' as part of a
  multi-byte character.

  @param name  path to calculate directory length for.
  @return length of directory part (including the last separator), or 0
 */
size_t dirname_length(const char *name) {
  const char *pos = name;     // position in input as we iterate over it
  const char *sep = nullptr;  // position of most recent separator character

#ifdef _WIN32
  // If we're on Windows ...

  /*
    At present, the character set for file-paths is either CP932
    (a 1/2 byte variable length charset) or considered binary.
    (See the function comment on more information about what "binary"
    implies in this context.)

    fs_character_set() gets this info from GetLocaleInfo(), so it's not
    something we configure within the server, we just roll with what we
    find.

    There is some non-standard oddness in Win's take on Shift-JIS, but
    the important part is that characters we'd be interested in (like
    '\0' '/' '\' ':') are single-width (1 byte).
  */
  CHARSET_INFO *fs = fs_character_set();

  /*
    If a drive letter separator is defined (e.g. ':'), see whether it
    is present in the path. If so, skip the drive letter.
    Luckily, ':' does not appear as part of a CP932 MB character, any
    matches are genuine.
  */
#ifdef FN_DEVCHAR
  const char *devchar_pos = strrchr(name, FN_DEVCHAR);  // find ':' if present

  if (devchar_pos != nullptr) {
    sep = devchar_pos;      // ':' counts as last character of path for now
    pos = devchar_pos + 1;  // position behind ':'
  }
#endif

  for (; *pos; pos++)  // Find last FN_LIBCHAR.
  {
    /*
      Check whether it's a multi-byte character.
      If it is a MB character, then by definition it can't be a directory
      separator, so we'll skip over it.

      By definition (or rather, due to the way fs_character_set() works),
      use_mb() is true for cp932, and false otherwise ("considered binary").

      For well-formed cp932, if the character pointed at is a valid
      multi-byte char header, the next character is guaranteed to be
      not '\0', but we guard against malformedness.
    */
    uint l;
    if (use_mb(fs) && pos[1] &&
        (l = my_ismbchar(fs, pos, pos + fs->mbmaxlen))) {
      pos += l - 1;  // skip character. length-1 to account for loop's pos++.
      continue;
    }

    if (is_directory_separator(*pos)) {  // If it's a directory separator,
      sep = pos;                         // save its position.
    }
  }

#else
  // not Windows ...
  // FN_DEVCHAR should be undefined here, so we're not checking for it.

  for (; *pos; pos++)  // Find last FN_LIBCHAR
  {
    if (is_directory_separator(*pos)) {  // If it's a directory separator,
      sep = pos;                         // save its position.
    }
  }
#endif

  if (sep == nullptr)  // No directory separator found:
    return 0;          // return 0

  return (sep - name) + 1;  // Otherwise, return length including separator
}

/**
  Gives directory part of filename. Directory ends with '/'.

  Pre-condition: At least FN_REFLEN bytes can be stored in buffer
  pointed to by 'to'. 'from' is a '\0'-terminated byte buffer.

  Post-condition: At most FN_REFLEN bytes will have been written to
  'to'. If the combined length of 'from' and any expanded elements
  exceeds FN_REFLEN-1, the result is truncated and likely not what the
  caller expects. If the result is truncated, the return value will be
  larger than the length stored in 'to_length'.

  @param to   destination buffer.
  @param name path to get the directory part of.
  @param to_res_length store the the number of bytes written into 'to'.

  @return Actual length of directory part in 'name' (the number of
  bytes which would have been written if 'to' had been large enough).
 */

size_t dirname_part(char *to, const char *name, size_t *to_res_length) {
  size_t length;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("'%s'", name));

  length = dirname_length(name);
  *to_res_length = (size_t)(convert_dirname(to, name, name + length) - to);
  return length;
} /* dirname */

#ifndef FN_DEVCHAR
#define FN_DEVCHAR '\0' /* For easier code */
#endif

/**
  Convert directory name to use under this system.

  Pre-condition: At least FN_REFLEN bytes can be stored in buffer
  pointed to by 'to'. 'from' is a '\0'-terminated byte buffer.

  Post-condition: At most FN_REFLEN bytes will have been written to
  'to'. If the combined length of 'from' and any expanded elements
  exceeds FN_REFLEN-1, the result is truncated and likely not what the
  caller expects.

  IMPLEMENTATION:
  If Windows converts '/' to '\'
  Adds a FN_LIBCHAR to end if the result string if there isn't one
  and the last isn't dev_char.
  Copies data from 'from' until ASCII(0) for until from == from_end
  If you want to use the whole 'from' string, just send nullptr as the
  last argument.

  If the result string is larger than FN_REFLEN -1, then it's cut.

  @param to destination buffer. Store result here. Must be at least of
  size min(FN_REFLEN, strlen(from) + 1) to make room for adding
  FN_LIBCHAR at the end.
  @param from Original filename. May be == to
  @param from_end Pointer at end of filename (normally end \0)
  @return Returns pointer to end \0 in to
*/

char *convert_dirname(char *to, const char *from, const char *from_end) {
  char *to_org = to;
#ifdef _WIN32
  CHARSET_INFO *fs = fs_character_set();
#endif
  DBUG_TRACE;

  /* We use -2 here, because we need place for the last FN_LIBCHAR */
  if (!from_end || (from_end - from) > FN_REFLEN - 2)
    from_end = from + FN_REFLEN - 2;

#if FN_LIBCHAR != '/'
  {
    for (; from < from_end && *from; from++) {
      if (*from == '/')
        *to++ = FN_LIBCHAR;
      else {
#ifdef _WIN32
        uint l;
        if (use_mb(fs) && (l = my_ismbchar(fs, from, from + 3))) {
          memmove(to, from, l);
          to += l;
          from += l - 1;
          to_org = to; /* Don't look inside mbchar */
        } else
#endif
        {
          *to++ = *from;
        }
      }
    }
    *to = 0;
  }
#else
  /* This is ok even if to == from, because we need to cut the string */
  to = strmake(to, from, (size_t)(from_end - from));
#endif

  /* Add FN_LIBCHAR to the end of directory path */
  if (to != to_org && (to[-1] != FN_LIBCHAR && to[-1] != FN_DEVCHAR)) {
    *to++ = FN_LIBCHAR;
    *to = 0;
  }
  return to; /* Pointer to end of dir */
} /* convert_dirname */
