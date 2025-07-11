/* Copyright (c) 2005, 2025, Oracle and/or its affiliates.

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

#ifndef MY_DECIMAL_INCLUDED
#define MY_DECIMAL_INCLUDED

/**
  @file

  It is interface module to fixed precision decimals library.

  Most functions use 'uint mask' as parameter, if during operation error
  which fit in this mask is detected then it will be processed automatically
  here. (errors are E_DEC_* constants, see include/decimal.h)

  Most function are just inline wrappers around library calls
*/

#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <algorithm>
#include <array>

#include "decimal.h"

#include "my_inttypes.h"
#include "my_macros.h"
#include "my_time_t.h"
#include "mysql/strings/dtoa.h"
#include "mysql/strings/m_ctype.h"

class String;
struct MYSQL_TIME;

static constexpr int DECIMAL_LONGLONG_DIGITS{22};

/** maximum length of buffer in our big digits (uint32). */
static constexpr int DECIMAL_BUFF_LENGTH{9};

/** the number of digits that my_decimal can possibly contain */
static constexpr int DECIMAL_MAX_POSSIBLE_PRECISION{DECIMAL_BUFF_LENGTH * 9};

/**
  maximum guaranteed precision of number in decimal digits (number of our
  digits * number of decimal digits in one our big digit - number of decimal
  digits in one our big digit decreased by 1 (because we always put decimal
  point on the border of our big digits))
*/
static constexpr int DECIMAL_MAX_PRECISION{DECIMAL_MAX_POSSIBLE_PRECISION -
                                           8 * 2};

/**
  maximum length of string representation (number of maximum decimal
  digits + 1 position for sign + 1 position for decimal point, no terminator)
*/
static constexpr int DECIMAL_MAX_STR_LENGTH{DECIMAL_MAX_POSSIBLE_PRECISION + 2};

/**
  maximum size of packet length.
*/
static constexpr int DECIMAL_MAX_FIELD_SIZE{DECIMAL_MAX_PRECISION};

inline int my_decimal_int_part(uint precision, uint decimals) {
  return precision - ((decimals == DECIMAL_NOT_SPECIFIED) ? 0 : decimals);
}

/**
  my_decimal class limits 'decimal_t' type to what we need in MySQL.

  It contains internally all necessary space needed by the instance so
  no extra memory is needed. Objects should be moved using copy CTOR
  or assignment operator, rather than memcpy/memmove.
*/

class my_decimal : public decimal_t {
  std::array<decimal_digit_t, DECIMAL_BUFF_LENGTH> buffer;

 public:
  my_decimal(const my_decimal &rhs) : decimal_t(rhs), buffer(rhs.buffer) {
    rhs.sanity_check();
    buf = buffer.data();
  }

  my_decimal &operator=(const my_decimal &rhs) {
    sanity_check();
    rhs.sanity_check();
    if (this == &rhs) return *this;
    decimal_t::intg = rhs.intg;
    decimal_t::frac = rhs.frac;
    decimal_t::len = rhs.len;
    decimal_t::sign = rhs.sign();
    buffer = rhs.buffer;
    decimal_t::buf = buffer.data();
    return *this;
  }

  void init() {
    len = DECIMAL_BUFF_LENGTH;
    buf = buffer.data();
    decimal_make_zero(this);
  }

  my_decimal() { init(); }

#ifndef NDEBUG
  ~my_decimal() { sanity_check(); }
#endif  // NDEBUG

  void sanity_check() const { assert(buf == buffer.data()); }

  bool sign() const { return decimal_t::sign; }
  void sign(bool s) { decimal_t::sign = s; }
  uint precision() const { return intg + frac; }

  /** Swap two my_decimal values */
  void swap(my_decimal &rhs) { std::swap(*this, rhs); }

#ifndef MYSQL_SERVER
  // Error reporting in server code only.
  int check_result(uint, int result) const { return result; }
#else
  int check_result(uint, int result) const;
#endif
};

#ifndef NDEBUG
void print_decimal(const my_decimal *dec);
void print_decimal_buff(const my_decimal *dec, const uchar *ptr, int length);
const char *dbug_decimal_as_string(char *buff, const my_decimal *val);
#else
#define dbug_decimal_as_string(A) NULL
#endif

bool str_set_decimal(uint mask, const my_decimal *val, String *str,
                     const CHARSET_INFO *cs, uint decimals);

inline void max_my_decimal(my_decimal *to, int precision, int frac) {
  assert((precision <= DECIMAL_MAX_PRECISION) && (frac <= DECIMAL_MAX_SCALE));
  max_decimal(precision, frac, to);
}

inline void max_internal_decimal(my_decimal *to) {
  max_my_decimal(to, DECIMAL_MAX_PRECISION, 0);
}

inline int check_result_and_overflow(uint mask, int result, my_decimal *val) {
  if (val->check_result(mask, result) & E_DEC_OVERFLOW) {
    const bool sign = val->sign();
    val->sanity_check();
    max_internal_decimal(val);
    val->sign(sign);
  }
  /*
    Avoid returning negative zero, cfr. decimal_cmp()
    For result == E_DEC_DIV_ZERO *val has not been assigned.
  */
  if (result != E_DEC_DIV_ZERO && val->sign() && decimal_is_zero(val))
    val->sign(false);
  return result;
}

inline uint my_decimal_length_to_precision(uint length, uint scale,
                                           bool unsigned_flag) {
  /* Precision can't be negative thus ignore unsigned_flag when length is 0. */
  assert(length || !scale);
  const uint retval =
      (uint)(length - (scale > 0 ? 1 : 0) - (unsigned_flag || !length ? 0 : 1));
  return retval;
}

inline uint32 my_decimal_precision_to_length_no_truncation(uint precision,
                                                           uint8 scale,
                                                           bool unsigned_flag) {
  /*
    When precision is 0 it means that original length was also 0. Thus
    unsigned_flag is ignored in this case.
  */
  assert(precision || !scale);
  const uint32 retval = (uint32)(precision + (scale > 0 ? 1 : 0) +
                                 (unsigned_flag || !precision ? 0 : 1));
  if (retval == 0) return 1;
  return retval;
}

inline uint32 my_decimal_precision_to_length(uint precision, uint8 scale,
                                             bool unsigned_flag) {
  /*
    When precision is 0 it means that original length was also 0. Thus
    unsigned_flag is ignored in this case.
  */
  assert(precision || !scale);
  precision = std::min(precision, uint(DECIMAL_MAX_PRECISION));
  return my_decimal_precision_to_length_no_truncation(precision, scale,
                                                      unsigned_flag);
}

inline int my_decimal_string_length(const my_decimal *d) {
  /* length of string representation including terminating '\0' */
  return decimal_string_size(d);
}

inline int my_decimal_get_binary_size(uint precision, uint scale) {
  return decimal_bin_size((int)precision, (int)scale);
}

inline void my_decimal2decimal(const my_decimal *from, my_decimal *to) {
  *to = *from;
}

int my_decimal2binary(uint mask, const my_decimal *d, uchar *bin, int prec,
                      int scale);

inline int binary2my_decimal(uint mask, const uchar *bin, my_decimal *d,
                             int prec, int scale) {
  return d->check_result(mask, bin2decimal(bin, d, prec, scale, false));
}

/**
  Decode DECIMAL from binary form

  @param mask       Error mask
  @param bin        Binary string to decode
  @param d [out]    DECIMAL buffer
  @param prec       Precision of stored value
  @param scale      Scale of stored value
  @param keep_prec  Whether to keep stored value's precision

  @returns
    conversion error
*/

inline int binary2my_decimal(uint mask, const uchar *bin, my_decimal *d,
                             int prec, int scale, bool keep_prec) {
  return d->check_result(mask, bin2decimal(bin, d, prec, scale, keep_prec));
}

inline int my_decimal_set_zero(my_decimal *d) {
  /*
    We need the up-cast here, since my_decimal has sign() member functions,
    which conflicts with decimal_t::size
    (and decimal_make_zero is a macro, rather than a function).
  */
  decimal_make_zero(static_cast<decimal_t *>(d));
  return 0;
}

inline bool my_decimal_is_zero(const my_decimal *decimal_value) {
  return decimal_is_zero(decimal_value);
}

inline int my_decimal_round(uint mask, const my_decimal *from, int scale,
                            bool truncate, my_decimal *to) {
  return from->check_result(
      mask, decimal_round(from, to, scale, (truncate ? TRUNCATE : HALF_UP)));
}

inline int my_decimal_floor(uint mask, const my_decimal *from, my_decimal *to) {
  return from->check_result(mask, decimal_round(from, to, 0, FLOOR));
}

inline int my_decimal_ceiling(uint mask, const my_decimal *from,
                              my_decimal *to) {
  return from->check_result(mask, decimal_round(from, to, 0, CEILING));
}

int my_decimal2string(uint mask, const my_decimal *d, uint fixed_prec,
                      uint fixed_dec, String *str);

inline int my_decimal2string(uint mask, const my_decimal *d, String *str) {
  return my_decimal2string(mask, d, 0, 0, str);
}

inline int my_decimal2int(uint mask, const my_decimal *d, bool unsigned_flag,
                          longlong *l) {
  my_decimal rounded;
  /* decimal_round can return only E_DEC_TRUNCATED */
  decimal_round(d, &rounded, 0, HALF_UP);
  return d->check_result(
      mask, (unsigned_flag ? decimal2ulonglong(&rounded, (ulonglong *)l)
                           : decimal2longlong(&rounded, l)));
}

inline int my_decimal2double(uint, const my_decimal *d, double *result) {
  /* No need to call check_result as this will always succeed */
  return decimal2double(d, result);
}

inline int my_decimal2lldiv_t(uint mask, const my_decimal *d, lldiv_t *to) {
  return d->check_result(mask, decimal2lldiv_t(d, to));
}

inline int str2my_decimal(uint mask, const char *str, my_decimal *d,
                          const char **end) {
  return check_result_and_overflow(mask, string2decimal(str, d, end), d);
}

int str2my_decimal(uint mask, const char *from, size_t length,
                   const CHARSET_INFO *charset, my_decimal *decimal_value);

my_decimal *date2my_decimal(const MYSQL_TIME *ltime, my_decimal *dec);
my_decimal *time2my_decimal(const MYSQL_TIME *ltime, my_decimal *dec);
my_decimal *timeval2my_decimal(const my_timeval *tm, my_decimal *dec);

inline int double2my_decimal(uint mask, double val, my_decimal *d) {
  return check_result_and_overflow(mask, double2decimal(val, d), d);
}

inline int int2my_decimal(uint mask, longlong i, bool unsigned_flag,
                          my_decimal *d) {
  return d->check_result(mask,
                         (unsigned_flag ? ulonglong2decimal((ulonglong)i, d)
                                        : longlong2decimal(i, d)));
}

inline void my_decimal_neg(decimal_t *arg) {
  // Avoid returning negative zero, cfr. decimal_cmp()
  if (decimal_is_zero(arg)) {
    arg->sign = false;
    return;
  }
  arg->sign ^= 1;
}

inline int my_decimal_add(uint mask, my_decimal *res, const my_decimal *a,
                          const my_decimal *b) {
  return check_result_and_overflow(mask, decimal_add(a, b, res), res);
}

inline int my_decimal_sub(uint mask, my_decimal *res, const my_decimal *a,
                          const my_decimal *b) {
  return check_result_and_overflow(mask, decimal_sub(a, b, res), res);
}

inline int my_decimal_mul(uint mask, my_decimal *res, const my_decimal *a,
                          const my_decimal *b) {
  return check_result_and_overflow(mask, decimal_mul(a, b, res), res);
}

inline int my_decimal_div(uint mask, my_decimal *res, const my_decimal *a,
                          const my_decimal *b, int div_scale_inc) {
  return check_result_and_overflow(mask, decimal_div(a, b, res, div_scale_inc),
                                   res);
}

inline int my_decimal_mod(uint mask, my_decimal *res, const my_decimal *a,
                          const my_decimal *b) {
  return check_result_and_overflow(mask, decimal_mod(a, b, res), res);
}

/**
  @retval -1 if a @< b
  @retval 1 if a @> b
  @retval 0 if a == b
*/
inline int my_decimal_cmp(const my_decimal *a, const my_decimal *b) {
  return decimal_cmp(a, b);
}

inline bool operator<(const my_decimal &lhs, const my_decimal &rhs) {
  return my_decimal_cmp(&lhs, &rhs) < 0;
}

inline bool operator!=(const my_decimal &lhs, const my_decimal &rhs) {
  return my_decimal_cmp(&lhs, &rhs) != 0;
}

inline int my_decimal_intg(const my_decimal *a) { return decimal_intg(a); }

void my_decimal_trim(ulong *precision, uint *scale);

#endif  // MY_DECIMAL_INCLUDED
