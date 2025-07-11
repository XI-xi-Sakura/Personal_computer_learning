/* Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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

#ifndef __BASE64_H_INCLUDED__
#define __BASE64_H_INCLUDED__

/**
  @file include/base64.h
*/

#include <stddef.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "my_config.h"
#include "my_inttypes.h"

/* Allow multuple chunks 'AAA= AA== AA==', binlog uses this */
#define MY_BASE64_DECODE_ALLOW_MULTIPLE_CHUNKS 1

static char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

/*
  Base64 decoder stream
*/
typedef struct my_base64_decoder_t {
  const char *src; /* Pointer to the current input position        */
  const char *end; /* Pointer to the end of input buffer           */
  uint c;          /* Collect bits into this number                */
  int error;       /* Error code                                   */
  uchar state;     /* Character number in the current group of 4   */
  uchar mark;      /* Number of padding marks in the current group */
} MY_BASE64_DECODER;

/*
  Helper table for decoder.
  -2 means "space character"
  -1 means "bad character"
  Non-negative values mean valid base64 encoding character.
*/
static int8 from_base64_table[] = {
    /*00*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -2, -2, -2, -2, -2, -1, -1,
    /*10*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*20*/ -2, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, 62, -1, -1, -1, 63, /*  !"#$%&'()*+,-./ */
    /*30*/ 52, 53, 54, 55, 56, 57, 58, 59,
    60,        61, -1, -1, -1, -1, -1, -1, /* 0123456789:;<=>? */
    /*40*/ -1, 0,  1,  2,  3,  4,  5,  6,
    7,         8,  9,  10, 11, 12, 13, 14, /* @ABCDEFGHIJKLMNO */
    /*50*/ 15, 16, 17, 18, 19, 20, 21, 22,
    23,        24, 25, -1, -1, -1, -1, -1, /* PQRSTUVWXYZ[\]^_ */
    /*60*/ -1, 26, 27, 28, 29, 30, 31, 32,
    33,        34, 35, 36, 37, 38, 39, 40, /* `abcdefghijklmno */
    /*70*/ 41, 42, 43, 44, 45, 46, 47, 48,
    49,        50, 51, -1, -1, -1, -1, -1, /* pqrstuvwxyz{|}~  */
    /*80*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*90*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*A0*/ -2, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*B0*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*C0*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*D0*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*E0*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1,
    /*F0*/ -1, -1, -1, -1, -1, -1, -1, -1,
    -1,        -1, -1, -1, -1, -1, -1, -1};

/**
 * Skip leading spaces in a base64 encoded stream
 * and stop on the first non-space character.
 * decoder->src will point to the first non-space character,
 * or to the end of the input string.
 * In case when end-of-input met on unexpected position,
 * decoder->error is also set to 1.
 *
 * @param  decoder  Pointer to MY_BASE64_DECODER
 *
 * @return
 *   false on success (there are some more non-space input characters)
 *   true  on error (end-of-input found)
 */
static inline bool my_base64_decoder_skip_spaces(MY_BASE64_DECODER *decoder) {
  for (; decoder->src < decoder->end; decoder->src++) {
    if (from_base64_table[(uchar)*decoder->src] != -2) return false;
  }
  if (decoder->state > 0)
    decoder->error = 1; /* Unexpected end-of-input found */
  return true;
}

/**
 * Convert the next character in a base64 encoded stream
 * to a number in the range [0..63]
 * and mix it with the previously collected value in decoder->c.
 *
 * @param decoder base64 decoding stream
 *
 * @return
 *   false on success
 *   true  on error (invalid base64 character found)
 */
static inline bool my_base64_add(MY_BASE64_DECODER *decoder) {
  int res;
  decoder->c <<= 6;
  if ((res = from_base64_table[(uchar)*decoder->src++]) < 0)
    return (decoder->error = true);
  decoder->c += (uint)res;
  return false;
}

/**
 * Get the next character from a base64 encoded stream.
 * Skip spaces, then scan the next base64 character or a pad character
 * and collect bits into decoder->c.
 *
 * @param  decoder  Pointer to MY_BASE64_DECODER
 * @return
 *  false on success (a valid base64 encoding character found)
 *  true  on error (unexpected character or unexpected end-of-input found)
 */
static inline bool my_base64_decoder_getch(MY_BASE64_DECODER *decoder) {
  if (my_base64_decoder_skip_spaces(decoder)) return true; /* End-of-input */

  if (!my_base64_add(decoder)) /* Valid base64 character found */
  {
    if (decoder->mark) {
      /* If we have scanned '=' already, then only '=' is valid */
      assert(decoder->state == 3);
      decoder->error = 1;
      decoder->src--;
      return true; /* expected '=', but encoding character found */
    }
    decoder->state++;
    return false;
  }

  /* Process error */
  switch (decoder->state) {
    case 0:
    case 1:
      decoder->src--;
      return true; /* base64 character expected */

    case 2:
    case 3:
      if (decoder->src[-1] == '=') {
        decoder->error = 0; /* Not an error - it's a pad character */
        decoder->mark++;
      } else {
        decoder->src--;
        return true; /* base64 character or '=' expected */
      }
      break;

    default:
      assert(0);
      return true; /* Wrong state, should not happen */
  }

  decoder->state++;
  return false;
}

/*
  Calculate how much memory needed for dst of base64_encode()
*/
static inline uint64 base64_needed_encoded_length(uint64 length_of_data) {
  uint64 nb_base64_chars;
  if (length_of_data == 0) return 1;
  nb_base64_chars = (length_of_data + 2) / 3 * 4;

  return nb_base64_chars +            /* base64 char incl padding */
         (nb_base64_chars - 1) / 76 + /* newlines */
         1;                           /* NUL termination of string */
}

/*
  Maximum length base64_encode_needed_length() can accept with no overflow.
*/
constexpr inline uint64 base64_encode_max_arg_length() {
  /*
    6827690988321067803 ->   9223372036854775805
    6827690988321067804 ->  -9223372036854775807
  */
  return 0x5EC0D4C77B03531BLL;
}

/*
  Calculate how much memory needed for dst of base64_decode()
*/
static inline uint64 base64_needed_decoded_length(
    uint64 length_of_encoded_data) {
  return static_cast<uint64>(
      ceil(static_cast<double>(length_of_encoded_data * 3 / 4)));
}

/*
  Maximum length base64_decode_needed_length() can accept with no overflow.
*/
constexpr inline uint64 base64_decode_max_arg_length() {
  return 0x2AAAAAAAAAAAAAAALL;
}

/*
  Encode data as a base64 string
*/
static inline int base64_encode(const void *src, size_t src_len, char *dst) {
  const unsigned char *s = (const unsigned char *)src;
  size_t i = 0;
  size_t len = 0;

  for (; i < src_len; len += 4) {
    unsigned c;

    if (len == 76) {
      len = 0;
      *dst++ = '\n';
    }

    c = s[i++];
    c <<= 8;

    if (i < src_len) c += s[i];
    c <<= 8;
    i++;

    if (i < src_len) c += s[i];
    i++;

    *dst++ = base64_table[(c >> 18) & 0x3f];
    *dst++ = base64_table[(c >> 12) & 0x3f];

    if (i > (src_len + 1))
      *dst++ = '=';
    else
      *dst++ = base64_table[(c >> 6) & 0x3f];

    if (i > src_len)
      *dst++ = '=';
    else
      *dst++ = base64_table[(c >> 0) & 0x3f];
  }
  *dst = '\0';

  return 0;
}

/**
 * Decode a base64 string
 * The base64-encoded data in the range ['src','*end_ptr') will be
 * decoded and stored starting at 'dst'.  The decoding will stop
 * after 'len' characters have been read from 'src', or when padding
 * occurs in the base64-encoded data. In either case: if 'end_ptr' is
 * non-null, '*end_ptr' will be set to point to the character after
 * the last read character, even in the presence of error.
 *
 * Note: We require that 'dst' is pre-allocated to correct size.
 *
 * @param src_base Pointer to base64-encoded string
 * @param len     Length of string at 'src'
 * @param dst     Pointer to location where decoded data will be stored
 * @param end_ptr Pointer to variable that will refer to the character
 *                after the end of the encoded data that were decoded.
 *                Can be NULL.
 * @param flags   flags e.g. allow multiple chunks
 * @return Number of bytes written at 'dst', or -1 in case of failure
 */
static inline int64 base64_decode(const char *src_base, size_t len, void *dst,
                                  const char **end_ptr, int flags) {
  char *d = (char *)dst;
  MY_BASE64_DECODER decoder;

  decoder.src = src_base;
  decoder.end = src_base + len;
  decoder.error = 0;
  decoder.mark = 0;

  for (;;) {
    decoder.c = 0;
    decoder.state = 0;

    if (my_base64_decoder_getch(&decoder) ||
        my_base64_decoder_getch(&decoder) ||
        my_base64_decoder_getch(&decoder) || my_base64_decoder_getch(&decoder))
      break;

    *d++ = (decoder.c >> 16) & 0xff;
    *d++ = (decoder.c >> 8) & 0xff;
    *d++ = (decoder.c >> 0) & 0xff;

    if (decoder.mark) {
      d -= decoder.mark;
      if (!(flags & MY_BASE64_DECODE_ALLOW_MULTIPLE_CHUNKS)) break;
      decoder.mark = 0;
    }
  }

  /* Return error if there are more non-space characters */
  decoder.state = 0;
  if (!my_base64_decoder_skip_spaces(&decoder)) decoder.error = 1;

  if (end_ptr != nullptr) *end_ptr = decoder.src;

  return decoder.error ? -1 : (int)(d - (char *)dst);
}

#endif /* !__BASE64_H_INCLUDED__ */
