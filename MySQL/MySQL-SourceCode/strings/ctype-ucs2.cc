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

// UCS-2 support.

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdarg>  // IWYU pragma: keep
#include <cstdint>
#include <cstring>
#include <limits>

#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_sys.h"  // MY_ERRNO_ERANGE etc.
#include "mysql/strings/dtoa.h"
#include "mysql/strings/int2str.h"
#include "mysql/strings/m_ctype.h"
#include "strings/m_ctype_internals.h"
#include "template_utils.h"

#ifndef EILSEQ
#define EILSEQ ENOENT
#endif

#define ULONGLONG_MAX (~(unsigned long long)0)
#define MAX_NEGATIVE_NUMBER ((unsigned long long)0x8000000000000000LL)
#define INIT_CNT 9
#define LFACTOR 1000000000ULL
#define LFACTOR1 10000000000ULL
#define LFACTOR2 100000000000ULL

static unsigned long lfactor[9] = {
    1L, 10L, 100L, 1000L, 10000L, 100000L, 1000000L, 10000000L, 100000000L};

MY_COMPILER_DIAGNOSTIC_PUSH()
// Suppress warning C4146 unary minus operator applied to unsigned type,
// result still unsigned
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4146)
static inline long long ulonglong_with_sign(bool negative,
                                            unsigned long long ll) {
  return negative ? -ll : ll;
}
MY_COMPILER_DIAGNOSTIC_POP()

static inline int my_bincmp(const uint8_t *s, const uint8_t *se,
                            const uint8_t *t, const uint8_t *te) {
  int slen = (int)(se - s), tlen = (int)(te - t);
  int const len = std::min(slen, tlen);
  int const cmp = memcmp(s, t, len);
  return cmp ? cmp : slen - tlen;
}

extern "C" {
static size_t my_caseup_str_mb2_or_mb4(const CHARSET_INFO *cs [[maybe_unused]],
                                       char *s [[maybe_unused]]) {
  assert(0);
  return 0;
}

static size_t my_casedn_str_mb2_or_mb4(const CHARSET_INFO *cs [[maybe_unused]],
                                       char *s [[maybe_unused]]) {
  assert(0);
  return 0;
}

static int my_strcasecmp_mb2_or_mb4(const CHARSET_INFO *cs [[maybe_unused]],
                                    const char *s [[maybe_unused]],
                                    const char *t [[maybe_unused]]) {
  assert(0);
  return 0;
}

static long my_strntol_mb2_or_mb4(const CHARSET_INFO *cs, const char *nptr,
                                  size_t l, int base, const char **endptr,
                                  int *err) {
  int negative = 0;
  int overflow;
  int cnv;
  my_wc_t wc;
  unsigned int cutlim;
  uint32_t cutoff = 0;
  uint32_t res = 0;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  const uint8_t *e = pointer_cast<const uint8_t *>(nptr) + l;
  const uint8_t *save = nullptr;

  *err = 0;
  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      switch (wc) {
        case ' ':
          break;
        case '\t':
          break;
        case '-':
          negative = !negative;
          break;
        case '+':
          break;
        default:
          goto bs;
      }
    } else /* No more characters or bad multibyte sequence */
    {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = (cnv == MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    }
    s += cnv;
  } while (true);

bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = ((uint32_t)~0L) / (uint32_t)base;
  cutlim = (unsigned)(((uint32_t)~0L) % (uint32_t)base);

  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      s += cnv;
      if (wc >= '0' && wc <= '9')
        wc -= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc = wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base) break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else {
        res *= (uint32_t)base;
        res += wc;
      }
    } else if (cnv == MY_CS_ILSEQ) {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = EILSEQ;
      return 0;
    } else {
      /* No more characters */
      break;
    }
  } while (true);

  if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);

  if (s == save) {
    err[0] = EDOM;
    return 0L;
  }

  if (negative) {
    if (res > (uint32_t)std::numeric_limits<int32_t>::min()) overflow = 1;
  } else if (res > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
    overflow = 1;

  if (overflow) {
    err[0] = ERANGE;
    return negative ? std::numeric_limits<int32_t>::min()
                    : std::numeric_limits<int32_t>::max();
  }

  return (negative ? -((long)res) : (long)res);
}

static unsigned long my_strntoul_mb2_or_mb4(const CHARSET_INFO *cs,
                                            const char *nptr, size_t l,
                                            int base, const char **endptr,
                                            int *err) {
  int negative = 0;
  int overflow;
  int cnv;
  my_wc_t wc;
  unsigned int cutlim;
  uint32_t cutoff = 0;
  uint32_t res = 0;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  const uint8_t *e = pointer_cast<const uint8_t *>(nptr) + l;
  const uint8_t *save = nullptr;

  *err = 0;
  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      switch (wc) {
        case ' ':
          break;
        case '\t':
          break;
        case '-':
          negative = !negative;
          break;
        case '+':
          break;
        default:
          goto bs;
      }
    } else /* No more characters or bad multibyte sequence */
    {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = (cnv == MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    }
    s += cnv;
  } while (true);

bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = ((uint32_t)~0L) / (uint32_t)base;
  cutlim = (unsigned)(((uint32_t)~0L) % (uint32_t)base);

  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      s += cnv;
      if (wc >= '0' && wc <= '9')
        wc -= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc = wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base) break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else {
        res *= (uint32_t)base;
        res += wc;
      }
    } else if (cnv == MY_CS_ILSEQ) {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = EILSEQ;
      return 0;
    } else {
      /* No more characters */
      break;
    }
  } while (true);

  if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);

  if (s == save) {
    err[0] = EDOM;
    return 0L;
  }

  if (overflow) {
    err[0] = (ERANGE);
    return (~(uint32_t)0);
  }

  return (negative ? -((long)res) : (long)res);
}

static long long my_strntoll_mb2_or_mb4(const CHARSET_INFO *cs,
                                        const char *nptr, size_t l, int base,
                                        const char **endptr, int *err) {
  int negative = 0;
  int overflow;
  int cnv;
  my_wc_t wc;
  unsigned long long cutoff = 0;
  unsigned int cutlim = 0;
  unsigned long long res = 0;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  const uint8_t *e = pointer_cast<const uint8_t *>(nptr) + l;
  const uint8_t *save = nullptr;

  *err = 0;
  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      switch (wc) {
        case ' ':
          break;
        case '\t':
          break;
        case '-':
          negative = !negative;
          break;
        case '+':
          break;
        default:
          goto bs;
      }
    } else /* No more characters or bad multibyte sequence */
    {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = (cnv == MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    }
    s += cnv;
  } while (true);

bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = (~(unsigned long long)0) / (unsigned long int)base;
  cutlim = (unsigned)((~(unsigned long long)0) % (unsigned long int)base);

  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      s += cnv;
      if (wc >= '0' && wc <= '9')
        wc -= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc = wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base) break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else {
        res *= (unsigned long long)base;
        res += wc;
      }
    } else if (cnv == MY_CS_ILSEQ) {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = EILSEQ;
      return 0;
    } else {
      /* No more characters */
      break;
    }
  } while (true);

  if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);

  if (s == save) {
    err[0] = EDOM;
    return 0L;
  }

  if (negative) {
    if (res > (unsigned long long)LLONG_MIN) overflow = 1;
  } else if (res > (unsigned long long)LLONG_MAX)
    overflow = 1;

  if (overflow) {
    err[0] = ERANGE;
    return negative ? LLONG_MIN : LLONG_MAX;
  }

  return ulonglong_with_sign(negative, res);
}

static unsigned long long my_strntoull_mb2_or_mb4(const CHARSET_INFO *cs,
                                                  const char *nptr, size_t l,
                                                  int base, const char **endptr,
                                                  int *err) {
  int negative = 0;
  int overflow;
  int cnv;
  my_wc_t wc;
  unsigned long long cutoff = 0;
  unsigned int cutlim;
  unsigned long long res = 0;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  const uint8_t *e = pointer_cast<const uint8_t *>(nptr) + l;
  const uint8_t *save = nullptr;

  *err = 0;
  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      switch (wc) {
        case ' ':
          break;
        case '\t':
          break;
        case '-':
          negative = !negative;
          break;
        case '+':
          break;
        default:
          goto bs;
      }
    } else /* No more characters or bad multibyte sequence */
    {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = (cnv == MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    }
    s += cnv;
  } while (true);

bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = (~(unsigned long long)0) / (unsigned long int)base;
  cutlim = (unsigned)((~(unsigned long long)0) % (unsigned long int)base);

  do {
    if ((cnv = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
      s += cnv;
      if (wc >= '0' && wc <= '9')
        wc -= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc = wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base) break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else {
        res *= (unsigned long long)base;
        res += wc;
      }
    } else if (cnv == MY_CS_ILSEQ) {
      if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);
      err[0] = EILSEQ;
      return 0;
    } else {
      /* No more characters */
      break;
    }
  } while (true);

  if (endptr != nullptr) *endptr = pointer_cast<const char *>(s);

  if (s == save) {
    err[0] = EDOM;
    return 0L;
  }

  if (overflow) {
    err[0] = ERANGE;
    return (~(unsigned long long)0);
  }

  return ulonglong_with_sign(negative, res);
}

static double my_strntod_mb2_or_mb4(const CHARSET_INFO *cs, const char *nptr,
                                    size_t length, const char **endptr,
                                    int *err) {
  char buf[256];
  double res;
  char *b = buf;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  const uint8_t *end = nullptr;
  my_wc_t wc;
  int cnv;

  *err = 0;
  /* Cut too long strings */
  if (length >= sizeof(buf)) length = sizeof(buf) - 1;
  end = s + length;

  while ((cnv = cs->cset->mb_wc(cs, &wc, s, end)) > 0) {
    s += cnv;
    if (wc > (int)u'e' || !wc) break; /* Can't be part of double */
    *b++ = (char)wc;
  }

  *endptr = b;
  res = my_strtod(buf, endptr, err);
  *endptr = nptr + cs->mbminlen * (size_t)(*endptr - buf);
  return res;
}

static unsigned long long my_strntoull10rnd_mb2_or_mb4(
    const CHARSET_INFO *cs, const char *nptr, size_t length, int unsign_fl,
    const char **endptr, int *err) {
  char buf[256], *b = buf;
  unsigned long long res = 0;
  const auto *s = pointer_cast<const uint8_t *>(nptr);
  my_wc_t wc;
  int cnv;

  /* Cut too long strings */
  if (length >= sizeof(buf)) length = sizeof(buf) - 1;
  const uint8_t *end = s + length;

  while ((cnv = cs->cset->mb_wc(cs, &wc, s, end)) > 0) {
    s += cnv;
    if (wc > (int)u'e' || !wc) break; /* Can't be a number part */
    *b++ = (char)wc;
  }

  res = my_strntoull10rnd_8bit(cs, buf, b - buf, unsign_fl, endptr, err);
  *endptr = nptr + cs->mbminlen * (size_t)(*endptr - buf);
  return res;
}
}  // extern "C"

/*
  This is a fast version optimized for the case of radix 10 / -10
*/

extern "C" {
static size_t my_l10tostr_mb2_or_mb4(const CHARSET_INFO *cs, char *dst,
                                     size_t len, int radix, long int val) {
  char buffer[66];
  char *p, *db, *de;
  long int new_val;
  int sl = 0;
  auto uval = (unsigned long int)val;

  p = &buffer[sizeof(buffer) - 1];
  *p = '\0';

  if (radix < 0) {
    if (val < 0) {
      sl = 1;
      /* Avoid integer overflow in (-val) for LLONG_MIN (BUG#31799). */
      uval = (unsigned long int)0 - uval;
    }
  }

  new_val = (long)(uval / 10);
  *--p = '0' + (char)(uval - (unsigned long)new_val * 10);
  val = new_val;

  while (val != 0) {
    new_val = val / 10;
    *--p = '0' + (char)(val - new_val * 10);
    val = new_val;
  }

  if (sl) {
    *--p = '-';
  }

  for (db = dst, de = dst + len; (dst < de) && *p; p++) {
    int const cnvres =
        cs->cset->wc_mb(cs, (my_wc_t)p[0], pointer_cast<uint8_t *>(dst),
                        pointer_cast<uint8_t *>(de));
    if (cnvres > 0)
      dst += cnvres;
    else
      break;
  }
  return (int)(dst - db);
}

static size_t my_ll10tostr_mb2_or_mb4(const CHARSET_INFO *cs, char *dst,
                                      size_t len, int radix, long long val) {
  char buffer[65];
  char *p, *db, *de;
  long long_val;
  int sl = 0;
  auto uval = (unsigned long long)val;

  if (radix < 0) {
    if (val < 0) {
      sl = 1;
      /* Avoid integer overflow in (-val) for LLONG_MIN (BUG#31799). */
      uval = (unsigned long long)0 - uval;
    }
  }

  p = &buffer[sizeof(buffer) - 1];
  *p = '\0';

  if (uval == 0) {
    *--p = '0';
    goto cnv;
  }

  while (uval > (unsigned long long)LONG_MAX) {
    unsigned long long const quo = uval / (unsigned)10;
    auto rem = (unsigned)(uval - quo * (unsigned)10);
    *--p = '0' + rem;
    uval = quo;
  }

  long_val = (long)uval;
  while (long_val != 0) {
    long const quo = long_val / 10;
    *--p = (char)('0' + (long_val - quo * 10));
    long_val = quo;
  }

cnv:
  if (sl) {
    *--p = '-';
  }

  for (db = dst, de = dst + len; (dst < de) && *p; p++) {
    int const cnvres =
        cs->cset->wc_mb(cs, (my_wc_t)p[0], pointer_cast<uint8_t *>(dst),
                        pointer_cast<uint8_t *>(de));
    if (cnvres > 0)
      dst += cnvres;
    else
      break;
  }
  return (int)(dst - db);
}
}  // extern "C"

extern "C" {
static long long my_strtoll10_mb2(const CHARSET_INFO *cs, const char *nptr,
                                  const char **endptr, int *error) {
  const char *s, *end, *start, *n_end, *true_end;
  uint8_t c = 0;
  unsigned long i, j, k;
  unsigned long long li = 0;
  int negative;
  unsigned long cutoff = 0;
  unsigned long cutoff2 = 0;
  unsigned long cutoff3 = 0;
  my_wc_t wc;
  int res;

  s = nptr;
  /* If fixed length string */
  if (endptr) {
    /*
      Make sure string length is even.
      Odd length indicates a bug in the caller.
      Assert in debug, round in production.
    */
    assert((*endptr - s) % 2 == 0);
    end = s + ((*endptr - s) / 2) * 2;

    for (;;) /* Skip leading spaces and tabs */
    {
      res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                            pointer_cast<const uint8_t *>(end));
      if (res <= 0) goto no_conv;
      s += res;
      if (wc != ' ' && wc != '\t') break;
    }
  } else {
    /* We don't support null terminated strings in UCS2 */
    goto no_conv;
  }

  /* Check for a sign. */
  negative = 0;
  if (wc == '-') {
    *error = -1; /* Mark as negative number */
    negative = 1;
    res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                          pointer_cast<const uint8_t *>(end));
    if (res <= 0) goto no_conv;
    s += res;
    cutoff = MAX_NEGATIVE_NUMBER / LFACTOR2;
    cutoff2 = (MAX_NEGATIVE_NUMBER % LFACTOR2) / 100;
    cutoff3 = MAX_NEGATIVE_NUMBER % 100;
  } else {
    *error = 0;
    if (wc == '+') {
      res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                            pointer_cast<const uint8_t *>(end));
      if (res <= 0) goto no_conv;
      s += res;
    }
    cutoff = ULONGLONG_MAX / LFACTOR2;
    cutoff2 = ULONGLONG_MAX % LFACTOR2 / 100;
    cutoff3 = ULONGLONG_MAX % 100;
  }

  /* Handle case where we have a lot of pre-zero */
  if (wc == '0') {
    i = 0;
    for (;; s += res) {
      if (s == end) goto end_i; /* Return 0 */
      res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                            pointer_cast<const uint8_t *>(end));
      if (res <= 0) goto no_conv;
      if (wc != '0') break;
    }
    while (wc == '0')
      ;
    n_end = s + 2 * INIT_CNT;
  } else {
    /* Read first digit to check that it's a valid number */
    if ((c = (wc - '0')) > 9) goto no_conv;
    i = c;
    n_end = s + 2 * (INIT_CNT - 1);
  }

  /* Handle first 9 digits and store them in i */
  if (n_end > end) n_end = end;
  for (;;) {
    res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                          pointer_cast<const uint8_t *>(n_end));
    if (res <= 0) break;
    if ((c = (wc - '0')) > 9) goto end_i;
    s += res;
    i = i * 10 + c;
  }
  if (s == end) goto end_i;

  /* Handle next 9 digits and store them in j */
  j = 0;
  start = s; /* Used to know how much to shift i */
  n_end = true_end = s + 2 * INIT_CNT;
  if (n_end > end) n_end = end;
  do {
    res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                          pointer_cast<const uint8_t *>(end));
    if (res <= 0) goto no_conv;
    if ((c = (wc - '0')) > 9) goto end_i_and_j;
    s += res;
    j = j * 10 + c;
  } while (s != n_end);
  if (s == end) {
    if (s != true_end) goto end_i_and_j;
    goto end3;
  }
  res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                        pointer_cast<const uint8_t *>(end));
  if (res <= 0) goto no_conv;
  if ((c = (wc - '0')) > 9) goto end3;
  s += res;

  /* Handle the next 1 or 2 digits and store them in k */
  k = c;
  if (s == end) goto end4;
  res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(s),
                        pointer_cast<const uint8_t *>(end));
  if (res <= 0) goto no_conv;
  if ((c = (wc - '0')) > 9) goto end4;
  s += res;
  k = k * 10 + c;
  *endptr = s;

  /* number string should have ended here */
  if (s != end && (c = (wc - '0')) <= 9) goto overflow;

  /* Check that we didn't get an overflow with the last digit */
  if (i > cutoff ||
      (i == cutoff && ((j > cutoff2 || j == cutoff2) && k > cutoff3)))
    goto overflow;
  li = i * LFACTOR2 + (unsigned long long)j * 100 + k;
  return (long long)li;

overflow: /* *endptr is set here */
  *error = MY_ERRNO_ERANGE;
  return negative ? LLONG_MIN : (long long)ULONGLONG_MAX;

end_i:
  *endptr = s;
  return (negative ? ((long long)-(long)i) : (long long)i);

end_i_and_j:
  li = (unsigned long long)i * lfactor[(size_t)(s - start) / 2] + j;
  *endptr = s;
  return (negative ? -((long long)li) : (long long)li);

end3:
  li = (unsigned long long)i * LFACTOR + (unsigned long long)j;
  *endptr = s;
  return (negative ? -((long long)li) : (long long)li);

end4:
  li = (unsigned long long)i * LFACTOR1 + (unsigned long long)j * 10 + k;
  *endptr = s;
  if (negative) {
    if (li > MAX_NEGATIVE_NUMBER) goto overflow;
    if (li == MAX_NEGATIVE_NUMBER) return LLONG_MIN;
    return -((long long)li);
  }
  return (long long)li;

no_conv:
  /* There was no number to convert.  */
  *error = MY_ERRNO_EDOM;
  *endptr = nptr;
  return 0;
}

static size_t my_scan_mb2(const CHARSET_INFO *cs, const char *str,
                          const char *end, int sequence_type) {
  const char *str0 = str;
  my_wc_t wc;
  int res;

  switch (sequence_type) {
    case MY_SEQ_SPACES:
      for (res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(str),
                                 pointer_cast<const uint8_t *>(end));
           res > 0 && wc == ' '; str += res,
          res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(str),
                                pointer_cast<const uint8_t *>(end))) {
      }
      return (size_t)(str - str0);
    default:
      return 0;
  }
}

static void my_fill_mb2(const CHARSET_INFO *cs, char *s, size_t slen,
                        int fill) {
  char buf[10];
  int buflen;

  assert((slen % 2) == 0);

  buflen = cs->cset->wc_mb(cs, (my_wc_t)fill, pointer_cast<uint8_t *>(buf),
                           pointer_cast<uint8_t *>(buf) + sizeof(buf));

  assert(buflen > 0);

  while (slen >= (size_t)buflen) {
    /* Enough space for the character */
    memcpy(s, buf, (size_t)buflen);
    s += buflen;
    slen -= buflen;
  }

  /*
    If there are some more space which is not enough
    for the whole multibyte character, then add trailing zeros.
  */
  for (; slen; slen--) {
    *s++ = 0x00;
  }
}
}  // extern "C"

static size_t my_vsnprintf_mb2(char *dst, size_t n, const char *fmt,
                               va_list ap) {
  char *start = dst, *end = dst + n - 1;
  for (; *fmt; fmt++) {
    if (fmt[0] != '%') {
      if (dst == end) /* End of buffer */
        break;

      *dst++ = '\0';
      *dst++ = *fmt; /* Copy ordinary char */
      continue;
    }

    fmt++;

    /* Skip if max size is used (to be compatible with printf) */
    while ((*fmt >= '0' && *fmt <= '9') || *fmt == '.' || *fmt == '-') fmt++;

    if (*fmt == 'l') fmt++;

    if (*fmt == 's') /* String parameter */
    {
      const char *par = va_arg(ap, char *);
      size_t plen;
      auto left_len = (size_t)(end - dst);
      if (!par) par = "(null)";
      plen = strlen(par);
      if (left_len <= plen * 2) plen = left_len / 2 - 1;

      for (; plen; plen--, dst += 2, par++) {
        dst[0] = '\0';
        dst[1] = par[0];
      }
      continue;
    }
    if (*fmt == 'd' || *fmt == 'u') /* Integer parameter */
    {
      char nbuf[16];
      char *pbuf = nbuf;

      if ((size_t)(end - dst) < 32) break;
      if (*fmt == 'd')
        longlong10_to_str(va_arg(ap, int), nbuf, -10);
      else
        longlong10_to_str(va_arg(ap, unsigned), nbuf, 10);

      for (; pbuf[0]; pbuf++) {
        *dst++ = '\0';
        *dst++ = *pbuf;
      }
      continue;
    }

    /* We come here on '%%', unknown code or too long parameter */
    if (dst == end) break;
    *dst++ = '\0';
    *dst++ = '%'; /* % used as % or unknown code */
  }

  assert(dst <= end);
  *dst = '\0'; /* End of errmessage */
  return (size_t)(dst - start);
}

extern "C" {
static size_t my_snprintf_mb2(const CHARSET_INFO *cs [[maybe_unused]], char *to,
                              size_t n, const char *fmt, ...) {
  size_t retval;
  va_list args;
  va_start(args, fmt);
  retval = my_vsnprintf_mb2(to, n, fmt, args);
  va_end(args);
  return retval;
}

static size_t my_lengthsp_mb2(const CHARSET_INFO *cs [[maybe_unused]],
                              const char *ptr, size_t length) {
  const char *end = ptr + length;
  while (end > ptr + 1 && end[-1] == ' ' && end[-2] == '\0') end -= 2;
  return (size_t)(end - ptr);
}
}  // extern "C"

/*
  D800..DB7F - Non-provate surrogate high (896 pages)
  DB80..DBFF - Private surrogate high     (128 pages)
  DC00..DFFF - Surrogate low              (1024 codes in a page)
*/
#define MY_UTF16_SURROGATE_HIGH_FIRST 0xD800
#define MY_UTF16_SURROGATE_LOW_FIRST 0xDC00
#define MY_UTF16_SURROGATE_LOW_LAST 0xDFFF

#define MY_UTF16_HIGH_HEAD(x) ((((uint8_t)(x)) & 0xFC) == 0xD8)
#define MY_UTF16_LOW_HEAD(x) ((((uint8_t)(x)) & 0xFC) == 0xDC)
#define MY_UTF16_SURROGATE(x) (((x)&0xF800) == 0xD800)

#define MY_UTF16_WC2(a, b) ((a << 8) + b)

/*
  a= 110110??  (<< 18)
  b= ????????  (<< 10)
  c= 110111??  (<<  8)
  d= ????????  (<<  0)
*/
#define MY_UTF16_WC4(a, b, c, d) \
  (((a & 3) << 18) + (b << 10) + ((c & 3) << 8) + d + 0x10000)

extern "C" {
static int my_utf16_uni(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t *pwc,
                        const uint8_t *s, const uint8_t *e) {
  if (s + 2 > e) return MY_CS_TOOSMALL2;

  /*
    High bytes: 0xD[89AB] = B'110110??'
    Low bytes:  0xD[CDEF] = B'110111??'
    Surrogate mask:  0xFC = B'11111100'
  */

  if (MY_UTF16_HIGH_HEAD(*s)) /* Surrogate head */
  {
    if (s + 4 > e) return MY_CS_TOOSMALL4;

    if (!MY_UTF16_LOW_HEAD(s[2])) /* Broken surrigate pair */
      return MY_CS_ILSEQ;

    *pwc = MY_UTF16_WC4(s[0], s[1], s[2], s[3]);
    return 4;
  }

  if (MY_UTF16_LOW_HEAD(*s)) /* Low surrogate part without high part */
    return MY_CS_ILSEQ;

  *pwc = MY_UTF16_WC2(s[0], s[1]);
  return 2;
}

static int my_uni_utf16(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t wc,
                        uint8_t *s, uint8_t *e) {
  if (wc <= 0xFFFF) {
    if (s + 2 > e) return MY_CS_TOOSMALL2;
    if (MY_UTF16_SURROGATE(wc)) return MY_CS_ILUNI;
    *s++ = (uint8_t)(wc >> 8);
    *s = (uint8_t)(wc & 0xFF);
    return 2;
  }

  if (wc <= 0x10FFFF) {
    if (s + 4 > e) return MY_CS_TOOSMALL4;
    *s++ = (uint8_t)((wc -= 0x10000) >> 18) | 0xD8;
    *s++ = (uint8_t)(wc >> 10) & 0xFF;
    *s++ = (uint8_t)((wc >> 8) & 3) | 0xDC;
    *s = (uint8_t)wc & 0xFF;
    return 4;
  }

  return MY_CS_ILUNI;
}
}  // extern "C"

static inline void my_tolower_utf16(const MY_UNICASE_INFO *uni_plane,
                                    my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page = uni_plane->page[*wc >> 8]))
    *wc = page[*wc & 0xFF].tolower;
}

static inline void my_toupper_utf16(const MY_UNICASE_INFO *uni_plane,
                                    my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page = uni_plane->page[*wc >> 8]))
    *wc = page[*wc & 0xFF].toupper;
}

static inline void my_tosort_utf16(const MY_UNICASE_INFO *uni_plane,
                                   my_wc_t *wc) {
  if (*wc <= uni_plane->maxchar) {
    const MY_UNICASE_CHARACTER *page;
    if ((page = uni_plane->page[*wc >> 8])) *wc = page[*wc & 0xFF].sort;
  } else {
    *wc = MY_CS_REPLACEMENT_CHARACTER;
  }
}

extern "C" {
static size_t my_caseup_utf16(const CHARSET_INFO *cs, char *src, size_t srclen,
                              char *dst [[maybe_unused]],
                              size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((src < srcend) &&
         (res = cs->cset->mb_wc(cs, &wc, pointer_cast<uint8_t *>(src),
                                pointer_cast<uint8_t *>(srcend))) > 0) {
    my_toupper_utf16(uni_plane, &wc);
    if (res != cs->cset->wc_mb(cs, wc, pointer_cast<uint8_t *>(src),
                               pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static void my_hash_sort_utf16(const CHARSET_INFO *cs, const uint8_t *s,
                               size_t slen, uint64_t *n1, uint64_t *n2) {
  my_wc_t wc;
  int res;
  const uint8_t *e =
      s + cs->cset->lengthsp(cs, pointer_cast<const char *>(s), slen);
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  uint64_t tmp1 = *n1;
  uint64_t tmp2 = *n2;

  while ((s < e) && (res = cs->cset->mb_wc(cs, &wc, s, e)) > 0) {
    my_tosort_utf16(uni_plane, &wc);
    tmp1 ^= (((tmp1 & 63) + tmp2) * (wc & 0xFF)) + (tmp1 << 8);
    tmp2 += 3;
    tmp1 ^= (((tmp1 & 63) + tmp2) * (wc >> 8)) + (tmp1 << 8);
    tmp2 += 3;
    s += res;
  }

  *n1 = tmp1;
  *n2 = tmp2;
}

static size_t my_casedn_utf16(const CHARSET_INFO *cs, char *src, size_t srclen,
                              char *dst [[maybe_unused]],
                              size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((src < srcend) &&
         (res = cs->cset->mb_wc(cs, &wc, pointer_cast<uint8_t *>(src),
                                pointer_cast<uint8_t *>(srcend))) > 0) {
    my_tolower_utf16(uni_plane, &wc);
    if (res != cs->cset->wc_mb(cs, wc, pointer_cast<uint8_t *>(src),
                               pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static int my_strnncoll_utf16(const CHARSET_INFO *cs, const uint8_t *s,
                              size_t slen, const uint8_t *t, size_t tlen,
                              bool t_is_prefix) {
  int s_res, t_res;
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  while (s < se && t < te) {
    s_res = cs->cset->mb_wc(cs, &s_wc, s, se);
    t_res = cs->cset->mb_wc(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return my_bincmp(s, se, t, te);
    }

    my_tosort_utf16(uni_plane, &s_wc);
    my_tosort_utf16(uni_plane, &t_wc);

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? (t - te) : ((se - s) - (te - t)));
}

/**
  Compare strings, discarding end space

  If one string is shorter as the other, then we space extend the other
  so that the strings have equal length.

  This will ensure that the following things hold:

    "a"  == "a "
    "a\0" < "a"
    "a\0" < "a "

  @param  cs        Character set pinter.
  @param  s         First string to compare.
  @param  slen      Length of 's'.
  @param  t         Second string to compare.
  @param  tlen      Length of 't'.

  IMPLEMENTATION

  @return Comparison result.
    @retval Negative number, if a less than b.
    @retval 0, if a is equal to b
    @retval Positive number, if a > b
*/

static int my_strnncollsp_utf16(const CHARSET_INFO *cs, const uint8_t *s,
                                size_t slen, const uint8_t *t, size_t tlen) {
  int res;
  my_wc_t s_wc = 0;
  my_wc_t t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  assert((slen % 2) == 0);
  assert((tlen % 2) == 0);

  while (s < se && t < te) {
    int const s_res = cs->cset->mb_wc(cs, &s_wc, s, se);
    int const t_res = cs->cset->mb_wc(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare bytewise */
      return my_bincmp(s, se, t, te);
    }

    my_tosort_utf16(uni_plane, &s_wc);
    my_tosort_utf16(uni_plane, &t_wc);

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }

  slen = (size_t)(se - s);
  tlen = (size_t)(te - t);
  res = 0;

  if (slen != tlen) {
    int s_res, swap = 1;
    if (slen < tlen) {
      slen = tlen;
      s = t;
      se = te;
      swap = -1;
      res = -res;
    }

    for (; s < se; s += s_res) {
      if ((s_res = cs->cset->mb_wc(cs, &s_wc, s, se)) <= 0) {
        return 0;
      }
      if (s_wc != ' ') return (s_wc < ' ') ? -swap : swap;
    }
  }
  return res;
}

static unsigned my_ismbchar_utf16(const CHARSET_INFO *cs, const char *b,
                                  const char *e) {
  my_wc_t wc;
  int const res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(b),
                                  pointer_cast<const uint8_t *>(e));
  return (unsigned)(res > 0 ? res : 0);
}

static unsigned my_mbcharlen_utf16(const CHARSET_INFO *cs [[maybe_unused]],
                                   unsigned c) {
  assert(0);
  return MY_UTF16_HIGH_HEAD(c) ? 4 : 2;
}

static size_t my_numchars_utf16(const CHARSET_INFO *cs, const char *b,
                                const char *e) {
  size_t nchars = 0;
  for (;; nchars++) {
    size_t const charlen = my_ismbchar_utf16(cs, b, e);
    if (!charlen) break;
    b += charlen;
  }
  return nchars;
}

static size_t my_charpos_utf16(const CHARSET_INFO *cs, const char *b,
                               const char *e, size_t pos) {
  const char *b0 = b;
  unsigned charlen = 0;

  for (; pos; b += charlen, pos--) {
    if (!(charlen = my_ismbchar(cs, b, e)))
      return (e + 2 - b0); /* Error, return pos outside the string */
  }
  return (size_t)(pos ? (e + 2 - b0) : (b - b0));
}

static size_t my_well_formed_len_utf16(const CHARSET_INFO *cs, const char *b,
                                       const char *e, size_t nchars,
                                       int *error) {
  const char *b0 = b;
  unsigned charlen = 0;
  *error = 0;

  for (; nchars; b += charlen, nchars--) {
    if (!(charlen = my_ismbchar(cs, b, e))) {
      *error = b < e ? 1 : 0;
      break;
    }
  }
  return (size_t)(b - b0);
}

static int my_wildcmp_utf16_ci(const CHARSET_INFO *cs, const char *str,
                               const char *str_end, const char *wildstr,
                               const char *wildend, int escape, int w_one,
                               int w_many) {
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, uni_plane);
}

static int my_wildcmp_utf16_bin(const CHARSET_INFO *cs, const char *str,
                                const char *str_end, const char *wildstr,
                                const char *wildend, int escape, int w_one,
                                int w_many) {
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, nullptr);
}

static int my_strnncoll_utf16_bin(const CHARSET_INFO *cs, const uint8_t *s,
                                  size_t slen, const uint8_t *t, size_t tlen,
                                  bool t_is_prefix) {
  int s_res, t_res;
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  while (s < se && t < te) {
    s_res = cs->cset->mb_wc(cs, &s_wc, s, se);
    t_res = cs->cset->mb_wc(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return my_bincmp(s, se, t, te);
    }
    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? (t - te) : ((se - s) - (te - t)));
}

static int my_strnncollsp_utf16_bin(const CHARSET_INFO *cs, const uint8_t *s,
                                    size_t slen, const uint8_t *t,
                                    size_t tlen) {
  int res;
  my_wc_t s_wc = 0;
  my_wc_t t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  assert((slen % 2) == 0);
  assert((tlen % 2) == 0);

  while (s < se && t < te) {
    int const s_res = cs->cset->mb_wc(cs, &s_wc, s, se);
    int const t_res = cs->cset->mb_wc(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare bytewise */
      return my_bincmp(s, se, t, te);
    }

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }

  slen = (size_t)(se - s);
  tlen = (size_t)(te - t);
  res = 0;

  if (slen != tlen) {
    int s_res, swap = 1;
    if (slen < tlen) {
      slen = tlen;
      s = t;
      se = te;
      swap = -1;
      res = -res;
    }

    for (; s < se; s += s_res) {
      if ((s_res = cs->cset->mb_wc(cs, &s_wc, s, se)) <= 0) {
        return 0;
      }
      if (s_wc != ' ') return (s_wc < ' ') ? -swap : swap;
    }
  }
  return res;
}

static void my_hash_sort_utf16_bin(const CHARSET_INFO *cs, const uint8_t *pos,
                                   size_t len, uint64_t *nr1, uint64_t *nr2) {
  const uint8_t *end =
      pos + cs->cset->lengthsp(cs, pointer_cast<const char *>(pos), len);
  uint64_t tmp1 = *nr1;
  uint64_t tmp2 = *nr2;

  for (; pos < end; pos++) {
    tmp1 ^= (uint64_t)((((unsigned)tmp1 & 63) + tmp2) * ((unsigned)*pos)) +
            (tmp1 << 8);
    tmp2 += 3;
  }

  *nr1 = tmp1;
  *nr2 = tmp2;
}
}  // extern "C"

static MY_COLLATION_HANDLER my_collation_utf16_general_ci_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_utf16,
    my_strnncollsp_utf16,
    my_strnxfrm_unicode,
    my_strnxfrmlen_simple,
    my_like_range_generic,
    my_wildcmp_utf16_ci,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_utf16,
    my_propagate_simple};

static MY_COLLATION_HANDLER my_collation_utf16_bin_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_utf16_bin,
    my_strnncollsp_utf16_bin,
    my_strnxfrm_unicode_full_bin,
    my_strnxfrmlen_unicode_full_bin,
    my_like_range_generic,
    my_wildcmp_utf16_bin,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_utf16_bin,
    my_propagate_simple};

MY_CHARSET_HANDLER my_charset_utf16_handler = {
    nullptr,            /* init         */
    my_ismbchar_utf16,  /* ismbchar     */
    my_mbcharlen_utf16, /* mbcharlen    */
    my_numchars_utf16,
    my_charpos_utf16,
    my_well_formed_len_utf16,
    my_lengthsp_mb2,
    my_numcells_mb,
    my_utf16_uni, /* mb_wc        */
    my_uni_utf16, /* wc_mb        */
    my_mb_ctype_mb,
    my_caseup_str_mb2_or_mb4,
    my_casedn_str_mb2_or_mb4,
    my_caseup_utf16,
    my_casedn_utf16,
    my_snprintf_mb2,
    my_l10tostr_mb2_or_mb4,
    my_ll10tostr_mb2_or_mb4,
    my_fill_mb2,
    my_strntol_mb2_or_mb4,
    my_strntoul_mb2_or_mb4,
    my_strntoll_mb2_or_mb4,
    my_strntoull_mb2_or_mb4,
    my_strntod_mb2_or_mb4,
    my_strtoll10_mb2,
    my_strntoull10rnd_mb2_or_mb4,
    my_scan_mb2};

CHARSET_INFO my_charset_utf16_general_ci = {
    54,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_PRIMARY | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "utf16",             /* cs name    */
    "utf16_general_ci",  /* m_coll_name  */
    "UTF-16 Unicode",    /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    nullptr,             /* ctype        */
    nullptr,             /* to_lower     */
    nullptr,             /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_utf16_handler,
    &my_collation_utf16_general_ci_handler,
    PAD_SPACE};

CHARSET_INFO my_charset_utf16_bin = {
    55,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_BINSORT | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "utf16",             /* cs name      */
    "utf16_bin",         /* m_coll_name  */
    "UTF-16 Unicode",    /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    nullptr,             /* ctype        */
    nullptr,             /* to_lower     */
    nullptr,             /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_utf16_handler,
    &my_collation_utf16_bin_handler,
    PAD_SPACE};

extern "C" {
static int my_utf16le_uni(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t *pwc,
                          const uint8_t *s, const uint8_t *e) {
  my_wc_t lo;

  if (s + 2 > e) return MY_CS_TOOSMALL2;

  if ((*pwc = uint2korr(s)) < MY_UTF16_SURROGATE_HIGH_FIRST ||
      (*pwc > MY_UTF16_SURROGATE_LOW_LAST))
    return 2; /* [0000-D7FF,E000-FFFF] */

  if (*pwc >= MY_UTF16_SURROGATE_LOW_FIRST)
    return MY_CS_ILSEQ; /* [DC00-DFFF] Low surrogate part without high part */

  if (s + 4 > e) return MY_CS_TOOSMALL4;

  s += 2;

  if ((lo = uint2korr(s)) < MY_UTF16_SURROGATE_LOW_FIRST ||
      lo > MY_UTF16_SURROGATE_LOW_LAST)
    return MY_CS_ILSEQ; /* Expected low surrogate part, got something else */

  *pwc = 0x10000 + (((*pwc & 0x3FF) << 10) | (lo & 0x3FF));
  return 4;
}

static int my_uni_utf16le(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t wc,
                          uint8_t *s, uint8_t *e) {
  if (wc < MY_UTF16_SURROGATE_HIGH_FIRST ||
      (wc > MY_UTF16_SURROGATE_LOW_LAST && wc <= 0xFFFF)) {
    if (s + 2 > e) return MY_CS_TOOSMALL2;
    int2store(s, (uint16_t)wc);
    return 2; /* [0000-D7FF,E000-FFFF] */
  }

  if (wc < 0xFFFF || wc > 0x10FFFF)
    return MY_CS_ILUNI; /* [D800-DFFF,10FFFF+] */

  if (s + 4 > e) return MY_CS_TOOSMALL4;

  wc -= 0x10000;
  int2store(s, (0xD800 | ((wc >> 10) & 0x3FF)));
  s += 2;
  int2store(s, (0xDC00 | (wc & 0x3FF)));
  return 4; /* [010000-10FFFF] */
}

static size_t my_lengthsp_utf16le(const CHARSET_INFO *cs [[maybe_unused]],
                                  const char *ptr, size_t length) {
  const char *end = ptr + length;
  while (end > ptr + 1 && uint2korr(end - 2) == 0x20) end -= 2;
  return (size_t)(end - ptr);
}
}  // extern "C"

static MY_CHARSET_HANDLER my_charset_utf16le_handler = {
    nullptr, /* init         */
    my_ismbchar_utf16,
    my_mbcharlen_utf16,
    my_numchars_utf16,
    my_charpos_utf16,
    my_well_formed_len_utf16,
    my_lengthsp_utf16le,
    my_numcells_mb,
    my_utf16le_uni, /* mb_wc        */
    my_uni_utf16le, /* wc_mb        */
    my_mb_ctype_mb,
    my_caseup_str_mb2_or_mb4,
    my_casedn_str_mb2_or_mb4,
    my_caseup_utf16,
    my_casedn_utf16,
    my_snprintf_mb2,
    my_l10tostr_mb2_or_mb4,
    my_ll10tostr_mb2_or_mb4,
    my_fill_mb2,
    my_strntol_mb2_or_mb4,
    my_strntoul_mb2_or_mb4,
    my_strntoll_mb2_or_mb4,
    my_strntoull_mb2_or_mb4,
    my_strntod_mb2_or_mb4,
    my_strtoll10_mb2,
    my_strntoull10rnd_mb2_or_mb4,
    my_scan_mb2};

CHARSET_INFO my_charset_utf16le_general_ci = {
    56,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_PRIMARY | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "utf16le",            /* cs name    */
    "utf16le_general_ci", /* m_coll_name  */
    "UTF-16LE Unicode",   /* comment      */
    nullptr,              /* tailoring    */
    nullptr,              /* coll_param   */
    nullptr,              /* ctype        */
    nullptr,              /* to_lower     */
    nullptr,              /* to_upper     */
    nullptr,              /* sort_order   */
    nullptr,              /* uca          */
    nullptr,              /* tab_to_uni   */
    nullptr,              /* tab_from_uni */
    &my_unicase_default,  /* caseinfo     */
    nullptr,              /* state_map    */
    nullptr,              /* ident_map    */
    1,                    /* strxfrm_multiply */
    1,                    /* caseup_multiply  */
    1,                    /* casedn_multiply  */
    2,                    /* mbminlen     */
    4,                    /* mbmaxlen     */
    1,                    /* mbmaxlenlen  */
    0,                    /* min_sort_char */
    0xFFFF,               /* max_sort_char */
    ' ',                  /* pad char      */
    false,                /* escape_with_backslash_is_dangerous */
    1,                    /* levels_for_compare */
    &my_charset_utf16le_handler,
    &my_collation_utf16_general_ci_handler,
    PAD_SPACE};

CHARSET_INFO my_charset_utf16le_bin = {
    62,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_BINSORT | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "utf16le",           /* cs name      */
    "utf16le_bin",       /* m_coll_name  */
    "UTF-16LE Unicode",  /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    nullptr,             /* ctype        */
    nullptr,             /* to_lower     */
    nullptr,             /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_utf16le_handler,
    &my_collation_utf16_bin_handler,
    PAD_SPACE};

extern "C" {
static int my_utf32_uni(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t *pwc,
                        const uint8_t *s, const uint8_t *e) {
  if (s + 4 > e) return MY_CS_TOOSMALL4;
  *pwc = (((my_wc_t)s[0]) << 24) + (s[1] << 16) + (s[2] << 8) + (s[3]);
  return 4;
}

static int my_uni_utf32(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t wc,
                        uint8_t *s, uint8_t *e) {
  if (s + 4 > e) return MY_CS_TOOSMALL4;

  s[0] = (uint8_t)(wc >> 24);
  s[1] = (uint8_t)(wc >> 16) & 0xFF;
  s[2] = (uint8_t)(wc >> 8) & 0xFF;
  s[3] = (uint8_t)wc & 0xFF;
  return 4;
}
}  // extern "C"

static inline void my_tolower_utf32(const MY_UNICASE_INFO *uni_plane,
                                    my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page = uni_plane->page[*wc >> 8]))
    *wc = page[*wc & 0xFF].tolower;
}

static inline void my_toupper_utf32(const MY_UNICASE_INFO *uni_plane,
                                    my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page = uni_plane->page[*wc >> 8]))
    *wc = page[*wc & 0xFF].toupper;
}

static inline void my_tosort_utf32(const MY_UNICASE_INFO *uni_plane,
                                   my_wc_t *wc) {
  if (*wc <= uni_plane->maxchar) {
    const MY_UNICASE_CHARACTER *page;
    if ((page = uni_plane->page[*wc >> 8])) *wc = page[*wc & 0xFF].sort;
  } else {
    *wc = MY_CS_REPLACEMENT_CHARACTER;
  }
}

extern "C" {
static size_t my_caseup_utf32(const CHARSET_INFO *cs, char *src, size_t srclen,
                              char *dst [[maybe_unused]],
                              size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((src < srcend) &&
         (res = my_utf32_uni(cs, &wc, pointer_cast<uint8_t *>(src),
                             pointer_cast<uint8_t *>(srcend))) > 0) {
    my_toupper_utf32(uni_plane, &wc);
    if (res != my_uni_utf32(cs, wc, pointer_cast<uint8_t *>(src),
                            pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static void my_hash_sort_utf32(const CHARSET_INFO *cs, const uint8_t *s,
                               size_t slen, uint64_t *n1, uint64_t *n2) {
  my_wc_t wc;
  int res;
  const uint8_t *e = s + slen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  unsigned ch = 0;

  /* Skip trailing spaces */
  while (e > s + 3 && e[-1] == ' ' && !e[-2] && !e[-3] && !e[-4]) e -= 4;

  uint64_t tmp1 = *n1;
  uint64_t tmp2 = *n2;

  while ((res = my_utf32_uni(cs, &wc, s, e)) > 0) {
    my_tosort_utf32(uni_plane, &wc);

    ch = (wc >> 24);
    tmp1 ^= (((tmp1 & 63) + tmp2) * ch) + (tmp1 << 8);
    tmp2 += 3;

    ch = (wc >> 16) & 0xFF;
    tmp1 ^= (((tmp1 & 63) + tmp2) * ch) + (tmp1 << 8);
    tmp2 += 3;

    ch = (wc >> 8) & 0xFF;
    tmp1 ^= (((tmp1 & 63) + tmp2) * ch) + (tmp1 << 8);
    tmp2 += 3;

    ch = (wc & 0xFF);
    tmp1 ^= (((tmp1 & 63) + tmp2) * ch) + (tmp1 << 8);
    tmp2 += 3;

    s += res;
  }

  *n1 = tmp1;
  *n2 = tmp2;
}

static size_t my_casedn_utf32(const CHARSET_INFO *cs, char *src, size_t srclen,
                              char *dst [[maybe_unused]],
                              size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((res = my_utf32_uni(cs, &wc, pointer_cast<uint8_t *>(src),
                             pointer_cast<uint8_t *>(srcend))) > 0) {
    my_tolower_utf32(uni_plane, &wc);
    if (res != my_uni_utf32(cs, wc, pointer_cast<uint8_t *>(src),
                            pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static int my_strnncoll_utf32(const CHARSET_INFO *cs, const uint8_t *s,
                              size_t slen, const uint8_t *t, size_t tlen,
                              bool t_is_prefix) {
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  while (s < se && t < te) {
    int const s_res = my_utf32_uni(cs, &s_wc, s, se);
    int const t_res = my_utf32_uni(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return my_bincmp(s, se, t, te);
    }

    my_tosort_utf32(uni_plane, &s_wc);
    my_tosort_utf32(uni_plane, &t_wc);

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? (t - te) : ((se - s) - (te - t)));
}

/**
  Compare strings, discarding end space

  If one string is shorter as the other, then we space extend the other
  so that the strings have equal length.

  This will ensure that the following things hold:

    "a"  == "a "
    "a\0" < "a"
    "a\0" < "a "

  @param  cs        Character set pinter.
  @param  s         First string to compare.
  @param  slen      Length of 's'.
  @param  t         Second string to compare.
  @param  tlen      Length of 't'.

  IMPLEMENTATION

  @return Comparison result.
    @retval Negative number, if a less than b.
    @retval 0, if a is equal to b
    @retval Positive number, if a > b
*/

static int my_strnncollsp_utf32(const CHARSET_INFO *cs, const uint8_t *s,
                                size_t slen, const uint8_t *t, size_t tlen) {
  int res;
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  assert((slen % 4) == 0);
  assert((tlen % 4) == 0);

  while (s < se && t < te) {
    int const s_res = my_utf32_uni(cs, &s_wc, s, se);
    int const t_res = my_utf32_uni(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare bytewise */
      return my_bincmp(s, se, t, te);
    }

    my_tosort_utf32(uni_plane, &s_wc);
    my_tosort_utf32(uni_plane, &t_wc);

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }

  slen = (size_t)(se - s);
  tlen = (size_t)(te - t);
  res = 0;

  if (slen != tlen) {
    int s_res, swap = 1;
    if (slen < tlen) {
      slen = tlen;
      s = t;
      se = te;
      swap = -1;
      res = -res;
    }

    for (; s < se; s += s_res) {
      if ((s_res = my_utf32_uni(cs, &s_wc, s, se)) < 0) {
        assert(0);
        return 0;
      }
      if (s_wc != ' ') return (s_wc < ' ') ? -swap : swap;
    }
  }
  return res;
}

static size_t my_strnxfrmlen_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                   size_t len) {
  return len / 2;
}

static unsigned my_ismbchar_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                  const char *b [[maybe_unused]],
                                  const char *e [[maybe_unused]]) {
  return 4;
}

static unsigned my_mbcharlen_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                   unsigned c [[maybe_unused]]) {
  return 4;
}
}  // extern "C"

static size_t my_vsnprintf_utf32(char *dst, size_t n, const char *fmt,
                                 va_list ap) {
  char *start = dst, *end = dst + n;
  assert((n % 4) == 0);
  for (; *fmt; fmt++) {
    if (fmt[0] != '%') {
      if (dst >= end) /* End of buffer */
        break;

      *dst++ = '\0';
      *dst++ = '\0';
      *dst++ = '\0';
      *dst++ = *fmt; /* Copy ordinary char */
      continue;
    }

    fmt++;

    /* Skip if max size is used (to be compatible with printf) */
    while ((*fmt >= '0' && *fmt <= '9') || *fmt == '.' || *fmt == '-') fmt++;

    if (*fmt == 'l') fmt++;

    if (*fmt == 's') /* String parameter */
    {
      const char *par = va_arg(ap, char *);
      size_t plen;
      auto left_len = (size_t)(end - dst);
      if (!par) par = "(null)";
      plen = strlen(par);
      if (left_len <= plen * 4) plen = left_len / 4 - 1;

      for (; plen; plen--, dst += 4, par++) {
        dst[0] = '\0';
        dst[1] = '\0';
        dst[2] = '\0';
        dst[3] = par[0];
      }
      continue;
    }
    if (*fmt == 'd' || *fmt == 'u') /* Integer parameter */
    {
      char nbuf[16];
      char *pbuf = nbuf;

      if ((size_t)(end - dst) < 64) break;
      if (*fmt == 'd')
        longlong10_to_str(va_arg(ap, int), nbuf, -10);
      else
        longlong10_to_str(va_arg(ap, unsigned), nbuf, 10);

      for (; pbuf[0]; pbuf++) {
        *dst++ = '\0';
        *dst++ = '\0';
        *dst++ = '\0';
        *dst++ = *pbuf;
      }
      continue;
    }

    /* We come here on '%%', unknown code or too long parameter */
    if (dst == end) break;
    *dst++ = '\0';
    *dst++ = '\0';
    *dst++ = '\0';
    *dst++ = '%'; /* % used as % or unknown code */
  }

  assert(dst < end);
  *dst++ = '\0';
  *dst++ = '\0';
  *dst++ = '\0';
  *dst++ = '\0'; /* End of errmessage */
  return (size_t)(dst - start - 4);
}

extern "C" {
static size_t my_snprintf_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                char *to, size_t n, const char *fmt, ...) {
  size_t retval;
  va_list args;
  va_start(args, fmt);
  retval = my_vsnprintf_utf32(to, n, fmt, args);
  va_end(args);
  return retval;
}

static long long my_strtoll10_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                    const char *nptr, const char **endptr,
                                    int *error) {
  const char *s, *end, *start, *n_end, *true_end;
  uint8_t c = 0;
  unsigned long i, j, k;
  unsigned long long li = 0;
  int negative;
  unsigned long cutoff = 0;
  unsigned long cutoff2 = 0;
  unsigned long cutoff3 = 0;

  s = nptr;
  /* If fixed length string */
  if (endptr) {
    /* Make sure string length is even */
    end = s + ((*endptr - s) / 4) * 4;
    while (s < end && !s[0] && !s[1] && !s[2] && (s[3] == ' ' || s[3] == '\t'))
      s += 4;
    if (s == end) goto no_conv;
  } else {
    /* We don't support null terminated strings in UCS2 */
    goto no_conv;
  }

  /* Check for a sign. */
  negative = 0;
  if (!s[0] && !s[1] && !s[2] && s[3] == '-') {
    *error = -1; /* Mark as negative number */
    negative = 1;
    s += 4;
    if (s == end) goto no_conv;
    cutoff = MAX_NEGATIVE_NUMBER / LFACTOR2;
    cutoff2 = (MAX_NEGATIVE_NUMBER % LFACTOR2) / 100;
    cutoff3 = MAX_NEGATIVE_NUMBER % 100;
  } else {
    *error = 0;
    if (!s[0] && !s[1] && !s[2] && s[3] == '+') {
      s += 4;
      if (s == end) goto no_conv;
    }
    cutoff = ULONGLONG_MAX / LFACTOR2;
    cutoff2 = ULONGLONG_MAX % LFACTOR2 / 100;
    cutoff3 = ULONGLONG_MAX % 100;
  }

  /* Handle case where we have a lot of pre-zero */
  if (!s[0] && !s[1] && !s[2] && s[3] == '0') {
    i = 0;
    do {
      s += 4;
      if (s == end) goto end_i; /* Return 0 */
    } while (!s[0] && !s[1] && !s[2] && s[3] == '0');
    n_end = s + 4 * INIT_CNT;
  } else {
    /* Read first digit to check that it's a valid number */
    if (s[0] || s[1] || s[2] || (c = (s[3] - '0')) > 9) goto no_conv;
    i = c;
    s += 4;
    n_end = s + 4 * (INIT_CNT - 1);
  }

  /* Handle first 9 digits and store them in i */
  if (n_end > end) n_end = end;
  for (; s != n_end; s += 4) {
    if (s[0] || s[1] || s[2] || (c = (s[3] - '0')) > 9) goto end_i;
    i = i * 10 + c;
  }
  if (s == end) goto end_i;

  /* Handle next 9 digits and store them in j */
  j = 0;
  start = s; /* Used to know how much to shift i */
  n_end = true_end = s + 4 * INIT_CNT;
  if (n_end > end) n_end = end;
  do {
    if (s[0] || s[1] || s[2] || (c = (s[3] - '0')) > 9) goto end_i_and_j;
    j = j * 10 + c;
    s += 4;
  } while (s != n_end);
  if (s == end) {
    if (s != true_end) goto end_i_and_j;
    goto end3;
  }
  if (s[0] || s[1] || s[2] || (c = (s[3] - '0')) > 9) goto end3;

  /* Handle the next 1 or 2 digits and store them in k */
  k = c;
  s += 4;
  if (s == end || s[0] || s[1] || s[2] || (c = (s[3] - '0')) > 9) goto end4;
  k = k * 10 + c;
  s += 2;
  *endptr = s;

  /* number string should have ended here */
  if (s != end && !s[0] && !s[1] && !s[2] && (c = (s[3] - '0')) <= 9)
    goto overflow;

  /* Check that we didn't get an overflow with the last digit */
  if (i > cutoff ||
      (i == cutoff && ((j > cutoff2 || j == cutoff2) && k > cutoff3)))
    goto overflow;
  li = i * LFACTOR2 + (unsigned long long)j * 100 + k;
  return (long long)li;

overflow: /* *endptr is set here */
  *error = MY_ERRNO_ERANGE;
  return negative ? LLONG_MIN : (long long)ULONGLONG_MAX;

end_i:
  *endptr = s;
  return (negative ? ((long long)-(long)i) : (long long)i);

end_i_and_j:
  li = (unsigned long long)i * lfactor[(size_t)(s - start) / 4] + j;
  *endptr = s;
  return (negative ? -((long long)li) : (long long)li);

end3:
  li = (unsigned long long)i * LFACTOR + (unsigned long long)j;
  *endptr = s;
  return (negative ? -((long long)li) : (long long)li);

end4:
  li = (unsigned long long)i * LFACTOR1 + (unsigned long long)j * 10 + k;
  *endptr = s;
  if (negative) {
    if (li > MAX_NEGATIVE_NUMBER) goto overflow;
    if (li == MAX_NEGATIVE_NUMBER) return LLONG_MIN;
    return -((long long)li);
  }
  return (long long)li;

no_conv:
  /* There was no number to convert.  */
  *error = MY_ERRNO_EDOM;
  *endptr = nptr;
  return 0;
}

static size_t my_numchars_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                const char *b, const char *e) {
  return (size_t)(e - b) / 4;
}

static size_t my_charpos_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                               const char *b, const char *e, size_t pos) {
  auto string_length = (size_t)(e - b);
  return pos * 4 > string_length ? string_length + 4 : pos * 4;
}

/*
  Valid characters are 0x00000000..0x0000D7FF and 0x0000E000..0x0010FFFF
 */
static size_t my_well_formed_len_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                       const char *b, const char *e,
                                       size_t nchars, int *error) {
  /* Ensure string length is divisible by 4 */
  const char *b0 = b;
  size_t length = e - b;
  if ((length % 4) != 0) {
    *error = 1;
    return 0;
  }
  *error = 0;
  nchars *= 4;
  if (length > nchars) {
    length = nchars;
    e = b + nchars;
  }
  for (; b < e; b += 4) {
    if (b[0] != 0 || static_cast<unsigned char>(b[1]) > 0x10) {
      *error = 1;
      return b - b0;
    }
    if (b[1] == 0 && (static_cast<unsigned char>(b[2]) >= 0xd8 &&
                      static_cast<unsigned char>(b[2]) < 0xe0)) {
      *error = 1;
      return b - b0;
    }
  }
  return length;
}

static void my_fill_utf32(const CHARSET_INFO *cs, char *s, size_t slen,
                          int fill) {
  char buf[10];
  char *e = s + slen;

  assert((slen % 4) == 0);
  {
#ifndef NDEBUG
    unsigned const buflen =
#endif
        cs->cset->wc_mb(cs, (my_wc_t)fill, pointer_cast<uint8_t *>(buf),
                        pointer_cast<uint8_t *>(buf) + sizeof(buf));
    assert(buflen == 4);
  }
  while (s < e) {
    memcpy(s, buf, 4);
    s += 4;
  }
}

static size_t my_lengthsp_utf32(const CHARSET_INFO *cs [[maybe_unused]],
                                const char *ptr, size_t length) {
  const char *end = ptr + length;
  assert((length % 4) == 0);
  while (end > ptr + 3 && end[-1] == ' ' && !end[-2] && !end[-3] && !end[-4])
    end -= 4;
  return (size_t)(end - ptr);
}

static int my_wildcmp_utf32_ci(const CHARSET_INFO *cs, const char *str,
                               const char *str_end, const char *wildstr,
                               const char *wildend, int escape, int w_one,
                               int w_many) {
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, uni_plane);
}

static int my_wildcmp_utf32_bin(const CHARSET_INFO *cs, const char *str,
                                const char *str_end, const char *wildstr,
                                const char *wildend, int escape, int w_one,
                                int w_many) {
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, nullptr);
}

static int my_strnncoll_utf32_bin(const CHARSET_INFO *cs, const uint8_t *s,
                                  size_t slen, const uint8_t *t, size_t tlen,
                                  bool t_is_prefix) {
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  while (s < se && t < te) {
    int const s_res = my_utf32_uni(cs, &s_wc, s, se);
    int const t_res = my_utf32_uni(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return my_bincmp(s, se, t, te);
    }
    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? (t - te) : ((se - s) - (te - t)));
}
}  // extern "C"

static inline my_wc_t my_utf32_get(const uint8_t *s) {
  return ((my_wc_t)s[0] << 24) + ((my_wc_t)s[1] << 16) + ((my_wc_t)s[2] << 8) +
         s[3];
}

extern "C" {
static int my_strnncollsp_utf32_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                    const uint8_t *s, size_t slen,
                                    const uint8_t *t, size_t tlen) {
  size_t minlen = 0;

  assert((slen % 4) == 0);
  assert((tlen % 4) == 0);

  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  for (minlen = std::min(slen, tlen); minlen; minlen -= 4) {
    my_wc_t const s_wc = my_utf32_get(s);
    my_wc_t const t_wc = my_utf32_get(t);
    if (s_wc != t_wc) return s_wc > t_wc ? 1 : -1;

    s += 4;
    t += 4;
  }

  if (slen != tlen) {
    int swap = 1;
    if (slen < tlen) {
      s = t;
      se = te;
      swap = -1;
    }

    for (; s < se; s += 4) {
      my_wc_t const s_wc = my_utf32_get(s);
      if (s_wc != ' ') return (s_wc < ' ') ? -swap : swap;
    }
  }
  return 0;
}

static size_t my_scan_utf32(const CHARSET_INFO *cs, const char *str,
                            const char *end, int sequence_type) {
  const char *str0 = str;

  switch (sequence_type) {
    case MY_SEQ_SPACES:
      for (; str < end;) {
        my_wc_t wc;
        int const res =
            my_utf32_uni(cs, &wc, pointer_cast<const uint8_t *>(str),
                         pointer_cast<const uint8_t *>(end));
        if (res < 0 || wc != ' ') break;
        str += res;
      }
      return (size_t)(str - str0);
    default:
      return 0;
  }
}
}  // extern "C"

static MY_COLLATION_HANDLER my_collation_utf32_general_ci_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_utf32,
    my_strnncollsp_utf32,
    my_strnxfrm_unicode,
    my_strnxfrmlen_utf32,
    my_like_range_generic,
    my_wildcmp_utf32_ci,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_utf32,
    my_propagate_simple};

static MY_COLLATION_HANDLER my_collation_utf32_bin_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_utf32_bin,
    my_strnncollsp_utf32_bin,
    my_strnxfrm_unicode_full_bin,
    my_strnxfrmlen_unicode_full_bin,
    my_like_range_generic,
    my_wildcmp_utf32_bin,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_utf32,
    my_propagate_simple};

MY_CHARSET_HANDLER my_charset_utf32_handler = {nullptr, /* init */
                                               my_ismbchar_utf32,
                                               my_mbcharlen_utf32,
                                               my_numchars_utf32,
                                               my_charpos_utf32,
                                               my_well_formed_len_utf32,
                                               my_lengthsp_utf32,
                                               my_numcells_mb,
                                               my_utf32_uni,
                                               my_uni_utf32,
                                               my_mb_ctype_mb,
                                               my_caseup_str_mb2_or_mb4,
                                               my_casedn_str_mb2_or_mb4,
                                               my_caseup_utf32,
                                               my_casedn_utf32,
                                               my_snprintf_utf32,
                                               my_l10tostr_mb2_or_mb4,
                                               my_ll10tostr_mb2_or_mb4,
                                               my_fill_utf32,
                                               my_strntol_mb2_or_mb4,
                                               my_strntoul_mb2_or_mb4,
                                               my_strntoll_mb2_or_mb4,
                                               my_strntoull_mb2_or_mb4,
                                               my_strntod_mb2_or_mb4,
                                               my_strtoll10_utf32,
                                               my_strntoull10rnd_mb2_or_mb4,
                                               my_scan_utf32};

CHARSET_INFO my_charset_utf32_general_ci = {
    60,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_PRIMARY | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_UNICODE_SUPPLEMENT | MY_CS_NONASCII,
    "utf32",             /* cs name    */
    "utf32_general_ci",  /* m_coll_name  */
    "UTF-32 Unicode",    /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    nullptr,             /* ctype        */
    nullptr,             /* to_lower     */
    nullptr,             /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_utf32_handler,
    &my_collation_utf32_general_ci_handler,
    PAD_SPACE};

CHARSET_INFO my_charset_utf32_bin = {
    61,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_BINSORT | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "utf32",             /* cs name    */
    "utf32_bin",         /* m_coll_name  */
    "UTF-32 Unicode",    /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    nullptr,             /* ctype        */
    nullptr,             /* to_lower     */
    nullptr,             /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_utf32_handler,
    &my_collation_utf32_bin_handler,
    PAD_SPACE};

static const uint8_t ctype_ucs2[] = {
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
    0};

static const uint8_t to_lower_ucs2[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106,
    107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
    122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
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

static const uint8_t to_upper_ucs2[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  65,  66,  67,  68,  69,  70,  71,  72,
    73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,
    88,  89,  90,  123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
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
static int my_ucs2_uni(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t *pwc,
                       const uint8_t *s, const uint8_t *e) {
  if (s + 2 > e) /* Need 2 characters */
    return MY_CS_TOOSMALL2;

  *pwc = ((uint8_t)s[0]) * 256 + ((uint8_t)s[1]);
  return 2;
}

static int my_uni_ucs2(const CHARSET_INFO *cs [[maybe_unused]], my_wc_t wc,
                       uint8_t *r, uint8_t *e) {
  if (r + 2 > e) return MY_CS_TOOSMALL2;

  if (wc > 0xFFFF) /* UCS2 does not support characters outside BMP */
    return MY_CS_ILUNI;

  r[0] = (uint8_t)(wc >> 8);
  r[1] = (uint8_t)(wc & 0xFF);
  return 2;
}
}  // extern "C"

static inline void my_tolower_ucs2(const MY_UNICASE_INFO *uni_plane,
                                   my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((page = uni_plane->page[(*wc >> 8) & 0xFF]))
    *wc = page[*wc & 0xFF].tolower;
}

static inline void my_toupper_ucs2(const MY_UNICASE_INFO *uni_plane,
                                   my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((page = uni_plane->page[(*wc >> 8) & 0xFF]))
    *wc = page[*wc & 0xFF].toupper;
}

static inline void my_tosort_ucs2(const MY_UNICASE_INFO *uni_plane,
                                  my_wc_t *wc) {
  const MY_UNICASE_CHARACTER *page;
  if ((page = uni_plane->page[(*wc >> 8) & 0xFF])) *wc = page[*wc & 0xFF].sort;
}

extern "C" {
static size_t my_caseup_ucs2(const CHARSET_INFO *cs, char *src, size_t srclen,
                             char *dst [[maybe_unused]],
                             size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((src < srcend) &&
         (res = my_ucs2_uni(cs, &wc, pointer_cast<uint8_t *>(src),
                            pointer_cast<uint8_t *>(srcend))) > 0) {
    my_toupper_ucs2(uni_plane, &wc);
    if (res != my_uni_ucs2(cs, wc, pointer_cast<uint8_t *>(src),
                           pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static void my_hash_sort_ucs2(const CHARSET_INFO *cs, const uint8_t *s,
                              size_t slen, uint64_t *n1, uint64_t *n2) {
  my_wc_t wc;
  int res;
  const uint8_t *e = s + slen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  while (e > s + 1 && e[-1] == ' ' && e[-2] == '\0') e -= 2;

  uint64_t tmp1 = *n1;
  uint64_t tmp2 = *n2;

  while ((s < e) && (res = my_ucs2_uni(cs, &wc, s, e)) > 0) {
    my_tosort_ucs2(uni_plane, &wc);
    tmp1 ^= (((tmp1 & 63) + tmp2) * (wc & 0xFF)) + (tmp1 << 8);
    tmp2 += 3;
    tmp1 ^= (((tmp1 & 63) + tmp2) * (wc >> 8)) + (tmp1 << 8);
    tmp2 += 3;
    s += res;
  }

  *n1 = tmp1;
  *n2 = tmp2;
}

static size_t my_casedn_ucs2(const CHARSET_INFO *cs, char *src, size_t srclen,
                             char *dst [[maybe_unused]],
                             size_t dstlen [[maybe_unused]]) {
  my_wc_t wc;
  int res;
  char *srcend = src + srclen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  assert(src == dst && srclen == dstlen);

  while ((src < srcend) &&
         (res = my_ucs2_uni(cs, &wc, pointer_cast<uint8_t *>(src),
                            pointer_cast<uint8_t *>(srcend))) > 0) {
    my_tolower_ucs2(uni_plane, &wc);
    if (res != my_uni_ucs2(cs, wc, pointer_cast<uint8_t *>(src),
                           pointer_cast<uint8_t *>(srcend)))
      break;
    src += res;
  }
  return srclen;
}

static void my_fill_ucs2(const CHARSET_INFO *cs [[maybe_unused]], char *s,
                         size_t l, int fill) {
  assert(fill <= 0xFFFF);
  for (; l >= 2; s[0] = (fill >> 8), s[1] = (fill & 0xFF), s += 2, l -= 2)
    ;
}

static int my_strnncoll_ucs2(const CHARSET_INFO *cs, const uint8_t *s,
                             size_t slen, const uint8_t *t, size_t tlen,
                             bool t_is_prefix) {
  int s_res, t_res;
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  while (s < se && t < te) {
    s_res = my_ucs2_uni(cs, &s_wc, s, se);
    t_res = my_ucs2_uni(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return ((int)s[0] - (int)t[0]);
    }

    my_tosort_ucs2(uni_plane, &s_wc);
    my_tosort_ucs2(uni_plane, &t_wc);

    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? t - te : ((se - s) - (te - t)));
}

/*
  Compare strings, discarding end space

  SYNOPSIS
    my_strnncollsp_ucs2()
    cs                  character set handler
    a                   First string to compare
    a_length            Length of 'a'
    b                   Second string to compare
    b_length            Length of 'b'

  IMPLEMENTATION
    If one string is shorter as the other, then we space extend the other
    so that the strings have equal length.

    This will ensure that the following things hold:

    "a"  == "a "
    "a\0" < "a"
    "a\0" < "a "

  RETURN
    < 0  a <  b
    = 0  a == b
    > 0  a > b
*/

static int my_strnncollsp_ucs2(const CHARSET_INFO *cs, const uint8_t *s,
                               size_t slen, const uint8_t *t, size_t tlen) {
  size_t minlen;
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;

  /* extra safety to make sure the lengths are even numbers */
  slen &= ~1;
  tlen &= ~1;

  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  for (minlen = std::min(slen, tlen); minlen; minlen -= 2) {
    int const s_wc = uni_plane->page[s[0]]
                         ? (int)uni_plane->page[s[0]][s[1]].sort
                         : (((int)s[0]) << 8) + (int)s[1];

    int const t_wc = uni_plane->page[t[0]]
                         ? (int)uni_plane->page[t[0]][t[1]].sort
                         : (((int)t[0]) << 8) + (int)t[1];
    if (s_wc != t_wc) return s_wc > t_wc ? 1 : -1;

    s += 2;
    t += 2;
  }

  if (slen != tlen) {
    int swap = 1;
    if (slen < tlen) {
      s = t;
      se = te;
      swap = -1;
    }

    for (; s < se; s += 2) {
      if (s[0] || s[1] != ' ') return (s[0] == 0 && s[1] < ' ') ? -swap : swap;
    }
  }
  return 0;
}

static unsigned my_ismbchar_ucs2(const CHARSET_INFO *cs [[maybe_unused]],
                                 const char *b [[maybe_unused]],
                                 const char *e [[maybe_unused]]) {
  return 2;
}

static unsigned my_mbcharlen_ucs2(const CHARSET_INFO *cs [[maybe_unused]],
                                  unsigned c [[maybe_unused]]) {
  return 2;
}

static size_t my_numchars_ucs2(const CHARSET_INFO *cs [[maybe_unused]],
                               const char *b, const char *e) {
  return (size_t)(e - b) / 2;
}

static size_t my_charpos_ucs2(const CHARSET_INFO *cs [[maybe_unused]],
                              const char *b, const char *e, size_t pos) {
  auto string_length = (size_t)(e - b);
  return pos > string_length ? string_length + 2 : pos * 2;
}

static size_t my_well_formed_len_ucs2(const CHARSET_INFO *cs [[maybe_unused]],
                                      const char *b, const char *e,
                                      size_t nchars, int *error) {
  /* Ensure string length is dividable with 2 */
  size_t const nbytes = ((size_t)(e - b)) & ~(size_t)1;
  *error = 0;
  nchars *= 2;
  return std::min(nbytes, nchars);
}

static int my_wildcmp_ucs2_ci(const CHARSET_INFO *cs, const char *str,
                              const char *str_end, const char *wildstr,
                              const char *wildend, int escape, int w_one,
                              int w_many) {
  const MY_UNICASE_INFO *uni_plane = cs->caseinfo;
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, uni_plane);
}

static int my_wildcmp_ucs2_bin(const CHARSET_INFO *cs, const char *str,
                               const char *str_end, const char *wildstr,
                               const char *wildend, int escape, int w_one,
                               int w_many) {
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend, escape, w_one,
                            w_many, nullptr);
}

static int my_strnncoll_ucs2_bin(const CHARSET_INFO *cs, const uint8_t *s,
                                 size_t slen, const uint8_t *t, size_t tlen,
                                 bool t_is_prefix) {
  int s_res, t_res;
  my_wc_t s_wc = 0, t_wc = 0;
  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  while (s < se && t < te) {
    s_res = my_ucs2_uni(cs, &s_wc, s, se);
    t_res = my_ucs2_uni(cs, &t_wc, t, te);

    if (s_res <= 0 || t_res <= 0) {
      /* Incorrect string, compare by char value */
      return ((int)s[0] - (int)t[0]);
    }
    if (s_wc != t_wc) {
      return s_wc > t_wc ? 1 : -1;
    }

    s += s_res;
    t += t_res;
  }
  return (int)(t_is_prefix ? t - te : ((se - s) - (te - t)));
}

static int my_strnncollsp_ucs2_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                   const uint8_t *s, size_t slen,
                                   const uint8_t *t, size_t tlen) {
  size_t minlen;

  /* extra safety to make sure the lengths are even numbers */
  slen = (slen >> 1) << 1;
  tlen = (tlen >> 1) << 1;

  const uint8_t *se = s + slen;
  const uint8_t *te = t + tlen;

  for (minlen = std::min(slen, tlen); minlen; minlen -= 2) {
    int const s_wc = s[0] * 256 + s[1];
    int const t_wc = t[0] * 256 + t[1];
    if (s_wc != t_wc) return s_wc > t_wc ? 1 : -1;

    s += 2;
    t += 2;
  }

  if (slen != tlen) {
    int swap = 1;
    if (slen < tlen) {
      s = t;
      se = te;
      swap = -1;
    }

    for (; s < se; s += 2) {
      if (s[0] || s[1] != ' ') return (s[0] == 0 && s[1] < ' ') ? -swap : swap;
    }
  }
  return 0;
}

static void my_hash_sort_ucs2_bin(const CHARSET_INFO *cs [[maybe_unused]],
                                  const uint8_t *key, size_t len, uint64_t *nr1,
                                  uint64_t *nr2) {
  const uint8_t *pos = key;

  key += len;

  while (key > pos + 1 && key[-1] == ' ' && key[-2] == '\0') key -= 2;

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

static MY_COLLATION_HANDLER my_collation_ucs2_general_ci_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_ucs2,
    my_strnncollsp_ucs2,
    my_strnxfrm_unicode,
    my_strnxfrmlen_simple,
    my_like_range_generic,
    my_wildcmp_ucs2_ci,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2,
    my_propagate_simple};

static MY_COLLATION_HANDLER my_collation_ucs2_bin_handler = {
    nullptr, /* init */
    nullptr,
    my_strnncoll_ucs2_bin,
    my_strnncollsp_ucs2_bin,
    my_strnxfrm_unicode,
    my_strnxfrmlen_simple,
    my_like_range_generic,
    my_wildcmp_ucs2_bin,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2_bin,
    my_propagate_simple};

MY_CHARSET_HANDLER my_charset_ucs2_handler = {nullptr,           /* init */
                                              my_ismbchar_ucs2,  /* ismbchar  */
                                              my_mbcharlen_ucs2, /* mbcharlen */
                                              my_numchars_ucs2,
                                              my_charpos_ucs2,
                                              my_well_formed_len_ucs2,
                                              my_lengthsp_mb2,
                                              my_numcells_mb,
                                              my_ucs2_uni, /* mb_wc        */
                                              my_uni_ucs2, /* wc_mb        */
                                              my_mb_ctype_mb,
                                              my_caseup_str_mb2_or_mb4,
                                              my_casedn_str_mb2_or_mb4,
                                              my_caseup_ucs2,
                                              my_casedn_ucs2,
                                              my_snprintf_mb2,
                                              my_l10tostr_mb2_or_mb4,
                                              my_ll10tostr_mb2_or_mb4,
                                              my_fill_ucs2,
                                              my_strntol_mb2_or_mb4,
                                              my_strntoul_mb2_or_mb4,
                                              my_strntoll_mb2_or_mb4,
                                              my_strntoull_mb2_or_mb4,
                                              my_strntod_mb2_or_mb4,
                                              my_strtoll10_mb2,
                                              my_strntoull10rnd_mb2_or_mb4,
                                              my_scan_mb2};

CHARSET_INFO my_charset_ucs2_general_ci = {
    35,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_PRIMARY | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,
    "ucs2",              /* cs name    */
    "ucs2_general_ci",   /* m_coll_name  */
    "UCS-2 Unicode",     /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    ctype_ucs2,          /* ctype        */
    to_lower_ucs2,       /* to_lower     */
    to_upper_ucs2,       /* to_upper     */
    to_upper_ucs2,       /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_general_ci_handler,
    PAD_SPACE};

CHARSET_INFO my_charset_ucs2_general_mysql500_ci = {
    159,
    0,
    0, /* number           */
    MY_CS_COMPILED | MY_CS_STRNXFRM | MY_CS_UNICODE |
        MY_CS_NONASCII,         /* state */
    "ucs2",                     /* cs name          */
    "ucs2_general_mysql500_ci", /* m_coll_name      */
    "UCS-2 Unicode",            /* comment          */
    nullptr,                    /* tailoring        */
    nullptr,                    /* coll_param       */
    ctype_ucs2,                 /* ctype            */
    to_lower_ucs2,              /* to_lower         */
    to_upper_ucs2,              /* to_upper         */
    to_upper_ucs2,              /* sort_order       */
    nullptr,                    /* uca              */
    nullptr,                    /* tab_to_uni       */
    nullptr,                    /* tab_from_uni     */
    &my_unicase_mysql500,       /* caseinfo         */
    nullptr,                    /* state_map        */
    nullptr,                    /* ident_map        */
    1,                          /* strxfrm_multiply */
    1,                          /* caseup_multiply  */
    1,                          /* casedn_multiply  */
    2,                          /* mbminlen         */
    2,                          /* mbmaxlen         */
    1,                          /* mbmaxlenlen      */
    0,                          /* min_sort_char    */
    0xFFFF,                     /* max_sort_char    */
    ' ',                        /* pad char         */
    false,                      /* escape_with_backslash_is_dangerous    */
    1,                          /* levels_for_compare */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_general_ci_handler,
    PAD_SPACE};

CHARSET_INFO my_charset_ucs2_bin = {
    90,
    0,
    0, /* number       */
    MY_CS_COMPILED | MY_CS_BINSORT | MY_CS_UNICODE | MY_CS_NONASCII,
    "ucs2",              /* cs name    */
    "ucs2_bin",          /* m_coll_name  */
    "UCS-2 Unicode",     /* comment      */
    nullptr,             /* tailoring    */
    nullptr,             /* coll_param   */
    ctype_ucs2,          /* ctype        */
    to_lower_ucs2,       /* to_lower     */
    to_upper_ucs2,       /* to_upper     */
    nullptr,             /* sort_order   */
    nullptr,             /* uca          */
    nullptr,             /* tab_to_uni   */
    nullptr,             /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    nullptr,             /* state_map    */
    nullptr,             /* ident_map    */
    1,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    0,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    false,               /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_bin_handler,
    PAD_SPACE};
