/* Copyright (c) 2002, tommy@valley.ne.jp
   Copyright (c) 2002, 2025, Oracle and/or its affiliates.

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

/**
  @file ctype-bin.cc
  The “binary” pseudo-charset. binary is special in that it's not really
  a character set; conversions from another charset _to_ binary means
  “erase the charset information” and conversions _from_ binary to another
  charset means “interpret these bytes as the destination charset”.
  In other words, in no event are the bytes changed; you can think of
  binary as the charset equivalent of void * that you can cast through.
  Needless to say, this also means that using it can be rather dangerous.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "m_string.h"
#include "mysql/strings/m_ctype.h"
#include "strings/m_ctype_internals.h"
#include "template_utils.h"

static const uint8_t ctype_bin[] = {
    0,  32,  32,  32,  32,  32,  32,  32,  32,  32,  40,  40, 40, 40, 40, 32,
    32, 32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32, 32, 32, 32, 32,
    32, 72,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16, 16, 16, 16, 16,
    16, 132, 132, 132, 132, 132, 132, 132, 132, 132, 132, 16, 16, 16, 16, 16,
    16, 16,  129, 129, 129, 129, 129, 129, 1,   1,   1,   1,  1,  1,  1,  1,
    1,  1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,  16, 16, 16, 16,
    16, 16,  130, 130, 130, 130, 130, 130, 2,   2,   2,   2,  2,  2,  2,  2,
    2,  2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,  16, 16, 16, 16,
    32, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,
    0,
};

/* Dummy array for toupper / tolower / sortorder */

static const uint8_t bin_char_array[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224,
    225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255};

extern "C" {
static bool my_coll_init_8bit_bin(CHARSET_INFO *cs, MY_CHARSET_LOADER *,
                                  MY_CHARSET_ERRMSG *) {
  cs->max_sort_char = 255;
  return false;
}

static int my_strnncoll_binary(const CHARSET_INFO *cs [[maybe_unused]],
                               const uint8_t *s, size_t slen, const uint8_t *t,
                               size_t tlen, bool t_is_prefix) {
  size_t const len = std::min(slen, tlen);
  const int cmp = len == 0 ? 0 : memcmp(s, t, len);  // memcmp(a, b, 0) == 0
  return cmp ? cmp : (int)((t_is_prefix ? len : slen) - tlen);
}

static size_t my_lengthsp_binary(const CHARSET_INFO *cs [[maybe_unused]],
                                 const char *ptr [[maybe_unused]],
                                 size_t length) {
  return length;
}
}  // extern "C"

/*
  Compare two strings. Result is sign(first_argument - second_argument)

  SYNOPSIS
    my_strnncollsp_binary()
    cs			Character set
    s			String to compare
    slen		Length of 's'
    t			String to compare
    tlen		Length of 't'

  NOTE
   This function is used for real binary strings, i.e. for
   BLOB, BINARY(N) and VARBINARY(N).
   It compares trailing spaces as spaces.

  RETURN
  < 0	s < t
  0	s == t
  > 0	s > t
*/

extern "C" {
static int my_strnncollsp_binary(const CHARSET_INFO *cs, const uint8_t *s,
                                 size_t slen, const uint8_t *t, size_t tlen) {
  return my_strnncoll_binary(cs, s, slen, t, tlen, false);
}

static int my_strnncoll_8bit_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                 const uint8_t *s, size_t slen,
                                 const uint8_t *t, size_t tlen,
                                 bool t_is_prefix) {
  size_t const len = std::min(slen, tlen);
  int const cmp = memcmp(s, t, len);
  return cmp ? cmp : (int)((t_is_prefix ? len : slen) - tlen);
}

/*
  Compare two strings. Result is sign(first_argument - second_argument)

  SYNOPSIS
    my_strnncollsp_8bit_bin()
    cs			Character set
    s			String to compare
    slen		Length of 's'
    t			String to compare
    tlen		Length of 't'

  NOTE
   This function is used for character strings with binary collations.
   The shorter string is extended with end space to be as long as the longer
   one.

  RETURN
  < 0	s < t
  0	s == t
  > 0	s > t
*/

static int my_strnncollsp_8bit_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                   const uint8_t *a, size_t a_length,
                                   const uint8_t *b, size_t b_length) {
  size_t const length = std::min(a_length, b_length);
  const uint8_t *end = a + length;
  while (a < end) {
    if (*a++ != *b++) return ((int)a[-1] - (int)b[-1]);
  }
  int res = 0;
  if (a_length != b_length) {
    int swap = 1;
    /*
      Check the next not space character of the longer key. If it's < ' ',
      then it's smaller than the other key.
    */
    if (a_length < b_length) {
      /* put shorter key in s */
      a_length = b_length;
      a = b;
      swap = -1; /* swap sign of result */
      res = -res;
    }
    for (end = a + a_length - length; a < end; a++) {
      if (*a != ' ') return (*a < ' ') ? -swap : swap;
    }
  }
  return res;
}

/* This function is used for all conversion functions */

static size_t my_case_str_bin(const CHARSET_INFO *cs [[maybe_unused]],
                              char *str [[maybe_unused]]) {
  return 0;
}

static size_t my_case_bin(const CHARSET_INFO *cs [[maybe_unused]],
                          char *src [[maybe_unused]], size_t srclen,
                          char *dst [[maybe_unused]],
                          size_t dstlen [[maybe_unused]]) {
  return srclen;
}

static int my_strcasecmp_bin(const CHARSET_INFO *cs [[maybe_unused]],
                             const char *s, const char *t) {
  return strcmp(s, t);
}
}  // extern "C"

unsigned my_mbcharlen_8bit(const CHARSET_INFO *cs [[maybe_unused]],
                           unsigned c [[maybe_unused]]) {
  return 1;
}

extern "C" {
static int my_mb_wc_bin(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t *wc,
                        const uint8_t *str, const uint8_t *end) {
  if (str >= end) return MY_CS_TOOSMALL;

  *wc = str[0];
  return 1;
}

static int my_wc_mb_bin(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t wc,
                        uint8_t *s, uint8_t *e) {
  if (s >= e) return MY_CS_TOOSMALL;

  if (wc < 256) {
    s[0] = (char)wc;
    return 1;
  }
  return MY_CS_ILUNI;
}
}  // extern "C"

extern "C" {
static void my_hash_sort_8bit_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                  const uint8_t *key, size_t len, uint64_t *nr1,
                                  uint64_t *nr2) {
  const uint8_t *pos = key;

  /*
     Remove trailing spaces. We have to do this to be able to compare
    'A ' and 'A' as identical
  */
  key = skip_trailing_space(key, len);

  uint64_t tmp1 = *nr1;
  uint64_t tmp2 = *nr2;

  for (; pos < key; pos++) {
    tmp1 ^= (uint64_t)((((unsigned)tmp1 & 63) + tmp2) * ((unsigned)*pos)) +
            (tmp1 << 8);
    tmp2 += 3;
  }

  *nr1 = tmp1;
  *nr2 = tmp2;
}

static void my_hash_sort_bin(const CHARSET_INFO *cs [[maybe_unused]],
                             const uint8_t *key, size_t len, uint64_t *nr1,
                             uint64_t *nr2) {
  const uint8_t *pos = key;

  key += len;

  uint64_t tmp1 = *nr1;
  uint64_t tmp2 = *nr2;

  for (; pos < key; pos++) {
    tmp1 ^= (uint64_t)((((unsigned)tmp1 & 63) + tmp2) * ((unsigned)*pos)) +
            (tmp1 << 8);
    tmp2 += 3;
  }

  *nr1 = tmp1;
  *nr2 = tmp2;
}
}  // extern "C"

/*
  The following defines is here to keep the following code identical to
  the one in ctype-simple.c
*/

#define likeconv(s, A) (A)
#define INC_PTR(cs, A, B) (A)++

static int my_wildcmp_bin_impl(const CHARSET_INFO *cs, const char *str,
                               const char *str_end, const char *wildstr,
                               const char *wildend, int escape, int w_one,
                               int w_many, int recurse_level) {
  int result = -1; /* Not found, using wildcards */

  if (my_string_stack_guard && my_string_stack_guard(recurse_level)) return -1;
  while (wildstr != wildend) {
    while (*wildstr != w_many && *wildstr != w_one) {
      if (*wildstr == escape && wildstr + 1 != wildend) wildstr++;
      if (str == str_end || likeconv(cs, *wildstr++) != likeconv(cs, *str++))
        return (1); /* No match */
      if (wildstr == wildend)
        return (str != str_end); /* Match if both are at end */
      result = 1;                /* Found an anchor char */
    }
    if (*wildstr == w_one) {
      do {
        if (str == str_end) /* Skip one char if possible */
          return (result);
        INC_PTR(cs, str, str_end);
      } while (++wildstr < wildend && *wildstr == w_one);
      if (wildstr == wildend) break;
    }
    if (*wildstr == w_many) { /* Found w_many */
      uint8_t cmp = 0;
      wildstr++;
      /* Remove any '%' and '_' from the wild search string */
      for (; wildstr != wildend; wildstr++) {
        if (*wildstr == w_many) continue;
        if (*wildstr == w_one) {
          if (str == str_end) return (-1);
          INC_PTR(cs, str, str_end);
          continue;
        }
        break; /* Not a wild character */
      }
      if (wildstr == wildend) return (0); /* match if w_many is last */
      if (str == str_end) return (-1);

      if ((cmp = *wildstr) == escape && wildstr + 1 != wildend)
        cmp = *++wildstr;

      INC_PTR(cs, wildstr, wildend); /* This is compared through cmp */
      cmp = likeconv(cs, cmp);
      do {
        while (str != str_end && (uint8_t)likeconv(cs, *str) != cmp) str++;
        if (str++ == str_end) return (-1);
        {
          int const tmp =
              my_wildcmp_bin_impl(cs, str, str_end, wildstr, wildend, escape,
                                  w_one, w_many, recurse_level + 1);
          if (tmp <= 0) return (tmp);
        }
      } while (str != str_end);
      return (-1);
    }
  }
  return (str != str_end ? 1 : 0);
}

int my_wildcmp_bin(const CHARSET_INFO *cs, const char *str, const char *str_end,
                   const char *wildstr, const char *wildend, int escape,
                   int w_one, int w_many) {
  return my_wildcmp_bin_impl(cs, str, str_end, wildstr, wildend, escape, w_one,
                             w_many, 1);
}

extern "C" {
static size_t my_strnxfrm_8bit_bin_pad_space(const CHARSET_INFO *cs,
                                             uint8_t *dst, size_t dstlen,
                                             unsigned nweights,
                                             const uint8_t *src, size_t srclen,
                                             unsigned flags) {
  srclen = std::min(srclen, dstlen);
  srclen = std::min<size_t>(srclen, nweights);
  if (dst != src && srclen > 0) memcpy(dst, src, srclen);
  return my_strxfrm_pad(cs, dst, dst + srclen, dst + dstlen,
                        static_cast<unsigned>(nweights - srclen), flags);
}

static size_t my_strnxfrm_8bit_bin_no_pad(const CHARSET_INFO *cs, uint8_t *dst,
                                          size_t dstlen, unsigned nweights,
                                          const uint8_t *src, size_t srclen,
                                          unsigned flags) {
  srclen = std::min(srclen, dstlen);
  srclen = std::min<size_t>(srclen, nweights);
  if (dst != src && srclen > 0) memcpy(dst, src, srclen);
  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && srclen < dstlen) {
    cs->cset->fill(cs, pointer_cast<char *>(dst) + srclen, dstlen - srclen,
                   cs->pad_char);
    return dstlen;
  }
  return srclen;
}

static bool my_instr_bin(const CHARSET_INFO *cs [[maybe_unused]], const char *b,
                         size_t b_length, const char *s, size_t s_length,
                         my_match_t *match) {
  const uint8_t *str = nullptr;
  const uint8_t *search = nullptr;
  const uint8_t *end = nullptr;
  const uint8_t *search_end = nullptr;

  if (s_length <= b_length) {
    if (s_length == 0) {
      if (match != nullptr) {
        match->end = 0;
        match->mb_len = 0;
      }
      return true; /* Empty string is always found */
    }

    str = pointer_cast<const uint8_t *>(b);
    search = pointer_cast<const uint8_t *>(s);
    end = pointer_cast<const uint8_t *>(b) + b_length - s_length + 1;
    search_end = pointer_cast<const uint8_t *>(s) + s_length;

  skip:
    while (str != end) {
      if ((*str++) == (*search)) {
        const uint8_t *i = str;
        const uint8_t *j = search + 1;

        while (j != search_end)
          if ((*i++) != (*j++)) goto skip;

        if (match != nullptr) {
          match->end = (unsigned)(str - pointer_cast<const uint8_t *>(b) - 1);
          match->mb_len = match->end;
        }
        return true;
      }
    }
  }
  return false;
}
}  // extern "C"

MY_COLLATION_HANDLER my_collation_8bit_bin_handler = {
    my_coll_init_8bit_bin,
    nullptr,
    my_strnncoll_8bit_bin,
    my_strnncollsp_8bit_bin,
    my_strnxfrm_8bit_bin_pad_space,
    my_strnxfrmlen_simple,
    my_like_range_simple,
    my_wildcmp_bin,
    my_strcasecmp_bin,
    my_instr_bin,
    my_hash_sort_8bit_bin,
    my_propagate_simple};

static MY_COLLATION_HANDLER my_collation_binary_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_binary,
    my_strnncollsp_binary,
    my_strnxfrm_8bit_bin_no_pad,
    my_strnxfrmlen_simple,
    my_like_range_simple,
    my_wildcmp_bin,
    my_strcasecmp_bin,
    my_instr_bin,
    my_hash_sort_bin,
    my_propagate_simple};

static MY_CHARSET_HANDLER my_charset_handler = {
    nullptr,           /* init */
    nullptr,           /* ismbchar      */
    my_mbcharlen_8bit, /* mbcharlen     */
    my_numchars_8bit,
    my_charpos_8bit,
    my_well_formed_len_8bit,
    my_lengthsp_binary,
    my_numcells_8bit,
    my_mb_wc_bin,
    my_wc_mb_bin,
    my_mb_ctype_8bit,
    my_case_str_bin,
    my_case_str_bin,
    my_case_bin,
    my_case_bin,
    my_snprintf_8bit,
    my_long10_to_str_8bit,
    my_longlong10_to_str_8bit,
    my_fill_8bit,
    my_strntol_8bit,
    my_strntoul_8bit,
    my_strntoll_8bit,
    my_strntoull_8bit,
    my_strntod_8bit,
    my_strtoll10_8bit,
    my_strntoull10rnd_8bit,
    my_scan_8bit};

CHARSET_INFO my_charset_bin = {
    63,
    0,
    0,                                              /* number        */
    MY_CS_COMPILED | MY_CS_BINSORT | MY_CS_PRIMARY, /* state */
    "binary",                                       /* cs name    */
    "binary",                                       /* m_coll_name   */
    "Binary pseudo charset",                        /* comment       */
    nullptr,                                        /* tailoring     */
    nullptr,                                        /* coll_param    */
    ctype_bin,                                      /* ctype         */
    bin_char_array,                                 /* to_lower      */
    bin_char_array,                                 /* to_upper      */
    nullptr,                                        /* sort_order    */
    nullptr,                                        /* uca           */
    nullptr,                                        /* tab_to_uni    */
    nullptr,                                        /* tab_from_uni  */
    &my_unicase_default,                            /* caseinfo     */
    nullptr,                                        /* state_map    */
    nullptr,                                        /* ident_map    */
    1,                                              /* strxfrm_multiply */
    1,                                              /* caseup_multiply  */
    1,                                              /* casedn_multiply  */
    1,                                              /* mbminlen      */
    1,                                              /* mbmaxlen      */
    1,                                              /* mbmaxlenlen   */
    0,                                              /* min_sort_char */
    255,                                            /* max_sort_char */
    0,                                              /* pad char      */
    false, /* escape_with_backslash_is_dangerous */
    1,     /* levels_for_compare */
    &my_charset_handler,
    &my_collation_binary_handler,
    NO_PAD};
