/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License, version 2.0, as published by the Free Software Foundation.

   This library is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the library and your derivative works with the
   separately licensed software that they have either included with
   the library or referenced in the documentation.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License, version 2.0, for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1301  USA */

/*  File   : strxnmov.c
    Author : Richard A. O'Keefe.
    Updated: 2 June 1984
    Defines: strxnmov()

    strxnmov(dst, len, src1, ..., srcn, nullptr)
    moves the first len characters of the concatenation of src1,...,srcn
    to dst and add a closing NUL character.
    It is just like strnmov except that it concatenates multiple sources.
    Beware: the last argument should be the null character pointer.
    Take VERY great care not to omit it!  Also be careful to use nullptr
    and NOT to use 0, as on some machines 0 is not the same size as a
    character pointer, or not the same bit pattern as nullptr.

    NOTE
      strxnmov is like strnmov in that it moves up to len
      characters; dst will be padded on the right with one '\0' character.
      if total-string-length >= length then dst[length] will be set to \0
*/

#ifndef INCLUDE_STRXNMOV_H_
#define INCLUDE_STRXNMOV_H_

#include <cstddef>

char *strxnmov(char *dst, size_t len, const char *src, ...);

#endif  // INCLUDE_STRXNMOV_H_
