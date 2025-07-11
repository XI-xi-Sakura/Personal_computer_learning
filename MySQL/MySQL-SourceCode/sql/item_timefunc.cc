/*
   Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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

/**
  @file

  @brief
  This file defines all time functions
*/

#include "sql/item_timefunc.h"

#include "my_config.h"
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "decimal.h"
#include "lex_string.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "my_systime.h"  // my_micro_time
#include "mysql/strings/int2str.h"
#include "mysql/strings/m_ctype.h"
#include "mysql/strings/my_strtoll10.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "nulls.h"
#include "sql-common/my_decimal.h"
#include "sql/current_thd.h"
#include "sql/dd/info_schema/table_stats.h"
#include "sql/dd/object_id.h"          // dd::Object_id
#include "sql/derror.h"                // ER_THD
#include "sql/parse_tree_node_base.h"  // Parse_context
#include "sql/sql_class.h"             // THD
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_locale.h"  // my_locale_en_US
#include "sql/sql_time.h"    // make_truncated_value_warning
#include "sql/strfunc.h"     // check_word
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/tztime.h"  // Time_zone
#include "sql_string.h"  // stringcmp
#include "string_with_len.h"
#include "strmake.h"
#include "template_utils.h"
#include "typelib.h"

using std::max;
using std::min;

/**
  Check and adjust a time value with a warning.

  @param ltime    Time variable.
  @param decimals Precision.
  @retval         True on error, false of success.
*/
static bool adjust_time_range_with_warn(MYSQL_TIME *ltime, uint8 decimals) {
  /* Fatally bad value should not come here */
  if (check_time_range_quick(*ltime)) {
    int warning = 0;
    if (make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                     ErrConvString(ltime, decimals),
                                     MYSQL_TIMESTAMP_TIME, NullS))
      return true;
    adjust_time_range(ltime, &warning);
  }
  return false;
}

/*
  Convert seconds to MYSQL_TIME value with overflow checking.

  SYNOPSIS:
    sec_to_time()
    seconds          number of seconds
    ltime            output MYSQL_TIME value

  DESCRIPTION
    If the 'seconds' argument is inside MYSQL_TIME data range, convert it to a
    corresponding value.
    Otherwise, truncate the resulting value to the nearest endpoint.
    Note: Truncation in this context means setting the result to the MAX/MIN
          value of TIME type if value is outside the allowed range.
          If the number of decimals exceeds what is supported, the value
          is rounded to the supported number of decimals.

  RETURN
    1                if the value was truncated during conversion
    0                otherwise
*/

static bool sec_to_time(lldiv_t seconds, MYSQL_TIME *ltime) {
  int warning = 0;

  set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);

  if (seconds.quot < 0 || seconds.rem < 0) {
    ltime->neg = true;
    seconds.quot = -seconds.quot;
    seconds.rem = -seconds.rem;
  }

  if (seconds.quot > TIME_MAX_VALUE_SECONDS) {
    set_max_hhmmss(ltime);
    return true;
  }

  ltime->hour = (uint)(seconds.quot / 3600);
  const uint sec = (uint)(seconds.quot % 3600);
  ltime->minute = sec / 60;
  ltime->second = sec % 60;
  time_add_nanoseconds_adjust_frac(ltime, static_cast<uint>(seconds.rem),
                                   &warning,
                                   current_thd->is_fsp_truncate_mode());

  adjust_time_range(ltime, &warning);
  return warning ? true : false;
}

/** Array of known date_time formats */
static constexpr const Known_date_time_format known_date_time_formats[6] = {
    {"USA", "%m.%d.%Y", "%Y-%m-%d %H.%i.%s", "%h:%i:%s %p"},
    {"JIS", "%Y-%m-%d", "%Y-%m-%d %H:%i:%s", "%H:%i:%s"},
    {"ISO", "%Y-%m-%d", "%Y-%m-%d %H:%i:%s", "%H:%i:%s"},
    {"EUR", "%d.%m.%Y", "%Y-%m-%d %H.%i.%s", "%H.%i.%s"},
    {"INTERNAL", "%Y%m%d", "%Y%m%d%H%i%s", "%H%i%s"},
    {nullptr, nullptr, nullptr, nullptr}};

/*
  Date formats corresponding to compound %r and %T conversion specifiers
*/
static const Date_time_format time_ampm_format = {{0}, {"%I:%i:%S %p", 11}};
static const Date_time_format time_24hrs_format = {{0}, {"%H:%i:%S", 8}};

/**
  Extract datetime value to MYSQL_TIME struct from string value
  according to format string.

  @param format		date/time format specification
  @param val			String to decode
  @param length		Length of string
  @param l_time		Store result here
  @param cached_timestamp_type  It uses to get an appropriate warning
                                in the case when the value is truncated.
  @param sub_pattern_end    if non-zero then we are parsing string which
                            should correspond compound specifier (like %T or
                            %r) and this parameter is pointer to place where
                            pointer to end of string matching this specifier
                            should be stored.
  @param date_time_type "time" or "datetime", used for the error/warning msg
  @param data_type  Type of data
  @param flags      flags used by check_date()

  @note
    Possibility to parse strings matching to patterns equivalent to compound
    specifiers is mainly intended for use from inside of this function in
    order to understand %T and %r conversion specifiers, so number of
    conversion specifiers that can be used in such sub-patterns is limited.
    Also most of checks are skipped in this case.

  @note
    If one adds new format specifiers to this function he should also
    consider adding them to Item_func_str_to_date::fix_from_format().

  @retval
    0	ok
  @retval
    1	error
*/

static bool extract_date_time(const Date_time_format *format, const char *val,
                              size_t length, MYSQL_TIME *l_time,
                              enum_mysql_timestamp_type cached_timestamp_type,
                              const char **sub_pattern_end,
                              const char *date_time_type,
                              enum_field_types data_type,
                              my_time_flags_t flags) {
  int weekday = 0, yearday = 0, daypart = 0;
  int week_number = -1;
  int error = 0;
  int strict_week_number_year = -1;
  int frac_part;
  bool usa_time = false;
  bool sunday_first_n_first_week_non_iso = false;
  bool strict_week_number = false;
  bool strict_week_number_year_type = false;
  const char *val_begin = val;
  const char *val_end = val + length;
  const char *ptr = format->format.str;
  const char *end = ptr + format->format.length;
  const CHARSET_INFO *cs = &my_charset_bin;
  DBUG_TRACE;

  if (!sub_pattern_end) memset(l_time, 0, sizeof(*l_time));

  for (; ptr != end && val != val_end; ptr++) {
    /* Skip pre-space between each argument */
    if ((val += cs->cset->scan(cs, val, val_end, MY_SEQ_SPACES)) >= val_end)
      break;

    if (*ptr == '%' && ptr + 1 != end) {
      int val_len;
      const char *tmp;

      error = 0;

      val_len = (uint)(val_end - val);
      switch (*++ptr) {
          /* Year */
        case 'Y':
          tmp = val + min(4, val_len);
          l_time->year = (int)my_strtoll10(val, &tmp, &error);
          if ((int)(tmp - val) <= 2)
            l_time->year = year_2000_handling(l_time->year);
          val = tmp;
          break;
        case 'y':
          tmp = val + min(2, val_len);
          l_time->year = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          l_time->year = year_2000_handling(l_time->year);
          break;

          /* Month */
        case 'm':
        case 'c':
          tmp = val + min(2, val_len);
          l_time->month = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;
        case 'M':
          if ((l_time->month = check_word(my_locale_en_US.month_names, val,
                                          val_end, &val)) <= 0)
            goto err;
          break;
        case 'b':
          if ((l_time->month = check_word(my_locale_en_US.ab_month_names, val,
                                          val_end, &val)) <= 0)
            goto err;
          break;
          /* Day */
        case 'd':
        case 'e':
          tmp = val + min(2, val_len);
          l_time->day = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;
        case 'D':
          tmp = val + min(2, val_len);
          l_time->day = (int)my_strtoll10(val, &tmp, &error);
          /* Skip 'st, 'nd, 'th .. */
          val = tmp + min<long>((val_end - tmp), 2);
          break;

          /* Hour */
        case 'h':
        case 'I':
        case 'l':
          usa_time = true;
          [[fallthrough]];
        case 'k':
        case 'H':
          tmp = val + min(2, val_len);
          l_time->hour = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;

          /* Minute */
        case 'i':
          tmp = val + min(2, val_len);
          l_time->minute = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;

          /* Second */
        case 's':
        case 'S':
          tmp = val + min(2, val_len);
          l_time->second = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;

          /* Second part */
        case 'f':
          tmp = val_end;
          if (tmp - val > 6) tmp = val + 6;
          l_time->second_part = (int)my_strtoll10(val, &tmp, &error);
          frac_part = 6 - (int)(tmp - val);
          if (frac_part > 0)
            l_time->second_part *= (ulong)log_10_int[frac_part];
          val = tmp;
          break;

          /* AM / PM */
        case 'p':
          if (val_len < 2 || !usa_time) goto err;
          if (!my_strnncoll(&my_charset_latin1, (const uchar *)val, 2,
                            (const uchar *)"PM", 2))
            daypart = 12;
          else if (my_strnncoll(&my_charset_latin1, (const uchar *)val, 2,
                                (const uchar *)"AM", 2))
            goto err;
          val += 2;
          break;

          /* Exotic things */
        case 'W':
          if ((weekday = check_word(my_locale_en_US.day_names, val, val_end,
                                    &val)) <= 0)
            goto err;
          break;
        case 'a':
          if ((weekday = check_word(my_locale_en_US.ab_day_names, val, val_end,
                                    &val)) <= 0)
            goto err;
          break;
        case 'w':
          tmp = val + 1;
          if ((weekday = (int)my_strtoll10(val, &tmp, &error)) < 0 ||
              weekday >= 7)
            goto err;
          /* We should use the same 1 - 7 scale for %w as for %W */
          if (!weekday) weekday = 7;
          val = tmp;
          break;
        case 'j':
          tmp = val + min(val_len, 3);
          yearday = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;

          /* Week numbers */
        case 'V':
        case 'U':
        case 'v':
        case 'u':
          sunday_first_n_first_week_non_iso = (*ptr == 'U' || *ptr == 'V');
          strict_week_number = (*ptr == 'V' || *ptr == 'v');
          tmp = val + min(val_len, 2);
          if ((week_number = (int)my_strtoll10(val, &tmp, &error)) < 0 ||
              (strict_week_number && !week_number) || week_number > 53)
            goto err;
          val = tmp;
          break;

          /* Year used with 'strict' %V and %v week numbers */
        case 'X':
        case 'x':
          strict_week_number_year_type = (*ptr == 'X');
          tmp = val + min(4, val_len);
          strict_week_number_year = (int)my_strtoll10(val, &tmp, &error);
          val = tmp;
          break;

          /* Time in AM/PM notation */
        case 'r':
          /*
            We can't just set error here, as we don't want to generate two
            warnings in case of errors
          */
          if (extract_date_time(&time_ampm_format, val, (uint)(val_end - val),
                                l_time, cached_timestamp_type, &val, "time",
                                data_type, flags))
            return true;
          break;

          /* Time in 24-hour notation */
        case 'T':
          if (extract_date_time(&time_24hrs_format, val, (uint)(val_end - val),
                                l_time, cached_timestamp_type, &val, "time",
                                data_type, flags))
            return true;
          break;

          /* Conversion specifiers that match classes of characters */
        case '.':
          while (val < val_end && my_ispunct(cs, *val)) val++;
          break;
        case '@':
          while (val < val_end && my_isalpha(cs, *val)) val++;
          break;
        case '#':
          while (val < val_end && my_isdigit(cs, *val)) val++;
          break;
        default:
          goto err;
      }
      if (error)  // Error from my_strtoll10
        goto err;
    } else if (!my_isspace(cs, *ptr)) {
      if (*val != *ptr) goto err;
      val++;
    }
  }
  if (usa_time) {
    if (l_time->hour > 12 || l_time->hour < 1) goto err;
    l_time->hour = l_time->hour % 12 + daypart;
  }

  /*
    If we are recursively called for parsing string matching compound
    specifiers we are already done.
  */
  if (sub_pattern_end) {
    *sub_pattern_end = val;
    return false;
  }

  if (yearday > 0) {
    uint days;
    days = calc_daynr(l_time->year, 1, 1) + yearday - 1;
    if (days <= 0 || days > MAX_DAY_NUMBER) goto err;
    get_date_from_daynr(days, &l_time->year, &l_time->month, &l_time->day);
  }

  if (week_number >= 0 && weekday) {
    int days;
    uint weekday_b;

    /*
      %V,%v require %X,%x resprectively,
      %U,%u should be used with %Y and not %X or %x
    */
    if ((strict_week_number &&
         (strict_week_number_year < 0 ||
          strict_week_number_year_type != sunday_first_n_first_week_non_iso)) ||
        (!strict_week_number && strict_week_number_year >= 0))
      goto err;

    /* Number of days since year 0 till 1st Jan of this year */
    days = calc_daynr(
        (strict_week_number ? strict_week_number_year : l_time->year), 1, 1);
    /* Which day of week is 1st Jan of this year */
    weekday_b = calc_weekday(days, sunday_first_n_first_week_non_iso);

    /*
      Below we are going to sum:
      1) number of days since year 0 till 1st day of 1st week of this year
      2) number of days between 1st week and our week
      3) and position of our day in the week
    */
    if (sunday_first_n_first_week_non_iso) {
      days += ((weekday_b == 0) ? 0 : 7) - weekday_b + (week_number - 1) * 7 +
              weekday % 7;
    } else {
      days += ((weekday_b <= 3) ? 0 : 7) - weekday_b + (week_number - 1) * 7 +
              (weekday - 1);
    }

    if (days <= 0 || days > MAX_DAY_NUMBER) goto err;
    get_date_from_daynr(days, &l_time->year, &l_time->month, &l_time->day);
  }

  assert(l_time->year <= 9999);
  if (data_type == MYSQL_TYPE_TIME) flags &= ~TIME_NO_ZERO_DATE;
  int warnings;
  if (check_datetime_range(*l_time) ||
      check_date(*l_time, non_zero_date(*l_time), flags, &warnings))
    goto err;

  if (val != val_end) {
    do {
      if (!my_isspace(&my_charset_latin1, *val)) {
        // TS-TODO: extract_date_time is not UCS2 safe
        if (make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                         ErrConvString(val_begin, length),
                                         cached_timestamp_type, NullS))
          goto err;
        break;
      }
    } while (++val != val_end);
  }
  return false;

err : {
  char buff[128];
  strmake(buff, val_begin, min<size_t>(length, sizeof(buff) - 1));
  push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                      ER_WRONG_VALUE_FOR_TYPE,
                      ER_THD(current_thd, ER_WRONG_VALUE_FOR_TYPE),
                      date_time_type, buff, "str_to_date");
}
  return true;
}

/**
  Create a formatted date/time value in a string.
*/

bool make_date_time(Date_time_format *format, MYSQL_TIME *l_time,
                    enum_mysql_timestamp_type type, String *str) {
  char intbuff[15];
  uint hours_i;
  uint weekday;
  ulong length;
  const char *ptr, *end;
  THD *thd = current_thd;
  MY_LOCALE *locale = thd->variables.lc_time_names;

  str->length(0);

  if (l_time->neg) str->append('-');

  end = (ptr = format->format.str) + format->format.length;
  for (; ptr != end; ptr++) {
    if (*ptr != '%' || ptr + 1 == end)
      str->append(*ptr);
    else {
      switch (*++ptr) {
        case 'M':
          if (!l_time->month) return true;
          str->append(
              locale->month_names->type_names[l_time->month - 1],
              strlen(locale->month_names->type_names[l_time->month - 1]),
              system_charset_info);
          break;
        case 'b':
          if (!l_time->month) return true;
          str->append(
              locale->ab_month_names->type_names[l_time->month - 1],
              strlen(locale->ab_month_names->type_names[l_time->month - 1]),
              system_charset_info);
          break;
        case 'W':
          if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
            return true;
          weekday = calc_weekday(
              calc_daynr(l_time->year, l_time->month, l_time->day), false);
          str->append(locale->day_names->type_names[weekday],
                      strlen(locale->day_names->type_names[weekday]),
                      system_charset_info);
          break;
        case 'a':
          if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
            return true;
          weekday = calc_weekday(
              calc_daynr(l_time->year, l_time->month, l_time->day), false);
          str->append(locale->ab_day_names->type_names[weekday],
                      strlen(locale->ab_day_names->type_names[weekday]),
                      system_charset_info);
          break;
        case 'D':
          if (type == MYSQL_TIMESTAMP_TIME) return true;
          length = longlong10_to_str(l_time->day, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          if (l_time->day >= 10 && l_time->day <= 19)
            str->append(STRING_WITH_LEN("th"));
          else {
            switch (l_time->day % 10) {
              case 1:
                str->append(STRING_WITH_LEN("st"));
                break;
              case 2:
                str->append(STRING_WITH_LEN("nd"));
                break;
              case 3:
                str->append(STRING_WITH_LEN("rd"));
                break;
              default:
                str->append(STRING_WITH_LEN("th"));
                break;
            }
          }
          break;
        case 'Y':
          length = longlong10_to_str(l_time->year, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 4, '0');
          break;
        case 'y':
          length = longlong10_to_str(l_time->year % 100, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'm':
          length = longlong10_to_str(l_time->month, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'c':
          length = longlong10_to_str(l_time->month, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          break;
        case 'd':
          length = longlong10_to_str(l_time->day, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'e':
          length = longlong10_to_str(l_time->day, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          break;
        case 'f':
          length =
              longlong10_to_str(l_time->second_part, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 6, '0');
          break;
        case 'H':
          length = longlong10_to_str(l_time->hour, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'h':
        case 'I':
          hours_i = (l_time->hour % 24 + 11) % 12 + 1;
          length = longlong10_to_str(hours_i, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'i': /* minutes */
          length = longlong10_to_str(l_time->minute, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'j': {
          if (type == MYSQL_TIMESTAMP_TIME) return true;

          int radix = 10;
          const int diff =
              calc_daynr(l_time->year, l_time->month, l_time->day) -
              calc_daynr(l_time->year, 1, 1) + 1;
          if (diff < 0) radix = -10;

          length = longlong10_to_str(diff, intbuff, radix) - intbuff;
          str->append_with_prefill(intbuff, length, 3, '0');
        } break;
        case 'k':
          length = longlong10_to_str(l_time->hour, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          break;
        case 'l':
          hours_i = (l_time->hour % 24 + 11) % 12 + 1;
          length = longlong10_to_str(hours_i, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          break;
        case 'p':
          hours_i = l_time->hour % 24;
          str->append(hours_i < 12 ? "AM" : "PM", 2);
          break;
        case 'r':
          length = sprintf(intbuff,
                           ((l_time->hour % 24) < 12) ? "%02d:%02d:%02d AM"
                                                      : "%02d:%02d:%02d PM",
                           (l_time->hour + 11) % 12 + 1, l_time->minute,
                           l_time->second);
          str->append(intbuff, length);
          break;
        case 'S':
        case 's':
          length = longlong10_to_str(l_time->second, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
          break;
        case 'T':
          length = sprintf(intbuff, "%02d:%02d:%02d", l_time->hour,
                           l_time->minute, l_time->second);
          str->append(intbuff, length);
          break;
        case 'U':
        case 'u': {
          uint year;
          if (type == MYSQL_TIMESTAMP_TIME) return true;
          length =
              longlong10_to_str(calc_week(*l_time,
                                          (*ptr) == 'U' ? WEEK_FIRST_WEEKDAY
                                                        : WEEK_MONDAY_FIRST,
                                          &year),
                                intbuff, 10) -
              intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
        } break;
        case 'v':
        case 'V': {
          uint year;
          if (type == MYSQL_TIMESTAMP_TIME) return true;
          length =
              longlong10_to_str(
                  calc_week(*l_time,
                            ((*ptr) == 'V' ? (WEEK_YEAR | WEEK_FIRST_WEEKDAY)
                                           : (WEEK_YEAR | WEEK_MONDAY_FIRST)),
                            &year),
                  intbuff, 10) -
              intbuff;
          str->append_with_prefill(intbuff, length, 2, '0');
        } break;
        case 'x':
        case 'X': {
          uint year;
          if (type == MYSQL_TIMESTAMP_TIME) return true;
          (void)calc_week(*l_time,
                          ((*ptr) == 'X' ? WEEK_YEAR | WEEK_FIRST_WEEKDAY
                                         : WEEK_YEAR | WEEK_MONDAY_FIRST),
                          &year);
          length = longlong10_to_str(year, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 4, '0');
        } break;
        case 'w':
          if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
            return true;
          weekday = calc_weekday(
              calc_daynr(l_time->year, l_time->month, l_time->day), true);
          length = longlong10_to_str(weekday, intbuff, 10) - intbuff;
          str->append_with_prefill(intbuff, length, 1, '0');
          break;

        default:
          str->append(*ptr);
          break;
      }
    }
  }
  return false;
}

/**
  @details
  Get a array of positive numbers from a string object.
  Each number is separated by 1 non digit character
  Return error if there is too many numbers.
  If there is too few numbers, assume that the numbers are left out
  from the high end. This allows one to give:
  DAY_TO_SECOND as "D MM:HH:SS", "MM:HH:SS" "HH:SS" or as seconds.

  @param args            item expression which we convert to an ASCII string
  @param str_value       string buffer
  @param is_negative     set to true if interval is prefixed by '-'
  @param count           count of elements in result array
  @param values          array of results
  @param transform_msec  if value is true we suppose
                         that the last part of string value is microseconds
                         and we should transform value to six digit value.
                         For example, '1.1' -> '1.100000'
*/

static bool get_interval_info(Item *args, String *str_value, bool *is_negative,
                              uint count, ulonglong *values,
                              bool transform_msec) {
  String *res;
  if (!(res = args->val_str_ascii(str_value))) return true;

  const CHARSET_INFO *cs = res->charset();
  const char *str = res->ptr();
  const char *end = str + res->length();

  str += cs->cset->scan(cs, str, end, MY_SEQ_SPACES);
  if (str < end && *str == '-') {
    *is_negative = true;
    str++;
  }

  while (str < end && !my_isdigit(cs, *str)) str++;

  long msec_length = 0;
  for (uint i = 0; i < count; i++) {
    longlong value;
    const char *start = str;
    for (value = 0; str != end && my_isdigit(cs, *str); str++) {
      if (value > (LLONG_MAX - 10) / 10) return true;
      value = value * 10LL + (longlong)(*str - '0');
    }
    msec_length = 6 - (str - start);
    values[i] = value;
    while (str != end && !my_isdigit(cs, *str)) str++;
    if (str == end && i != count - 1) {
      i++;
      /* Change values[0...i-1] -> values[0...count-1] */
      const size_t len = sizeof(*values) * i;
      memmove(reinterpret_cast<uchar *>(values + count) - len,
              reinterpret_cast<uchar *>(values + i) - len, len);
      memset(values, 0, sizeof(*values) * (count - i));
      break;
    }
  }

  if (transform_msec && msec_length > 0)
    values[count - 1] *= (long)log_10_int[msec_length];

  return (str != end);
}

/*** Abstract classes ****************************************/

bool Item_temporal_func::check_precision() {
  if (decimals > DATETIME_MAX_DECIMALS) {
    my_error(ER_TOO_BIG_PRECISION, MYF(0), (int)decimals, func_name(),
             DATETIME_MAX_DECIMALS);
    return true;
  }
  return false;
}

/**
  Appends function name with argument list or fractional seconds part
  to the String str.

  @param[in]      thd         Thread handle
  @param[in,out]  str         String to which the func_name and decimals/
                              argument list should be appended.
  @param[in]      query_type  Query type

*/

void Item_temporal_func::print(const THD *thd, String *str,
                               enum_query_type query_type) const {
  str->append(func_name());
  str->append('(');

  // When the functions have arguments specified
  if (arg_count) {
    print_args(thd, str, 0, query_type);
  } else if (decimals) {
    /*
      For temporal functions like NOW, CURTIME and SYSDATE which can specify
      fractional seconds part.
    */
    if (unsigned_flag)
      str->append_ulonglong(decimals);
    else
      str->append_longlong(decimals);
  }

  str->append(')');
}

type_conversion_status Item_temporal_hybrid_func::save_in_field_inner(
    Field *field, bool no_conversions) {
  if (data_type() == MYSQL_TYPE_TIME) return save_time_in_field(field);
  if (is_temporal_type_with_date(data_type())) return save_date_in_field(field);
  return Item_str_func::save_in_field_inner(field, no_conversions);
}

my_decimal *Item_temporal_hybrid_func::val_decimal(my_decimal *decimal_value) {
  assert(fixed);
  if (data_type() == MYSQL_TYPE_TIME)
    return val_decimal_from_time(decimal_value);
  else if (data_type() == MYSQL_TYPE_DATETIME)
    return val_decimal_from_date(decimal_value);
  else {
    MYSQL_TIME ltime;
    my_time_flags_t flags = TIME_FUZZY_DATE;
    if (sql_mode & MODE_NO_ZERO_IN_DATE) flags |= TIME_NO_ZERO_IN_DATE;
    if (sql_mode & MODE_NO_ZERO_DATE) flags |= TIME_NO_ZERO_DATE;
    if (sql_mode & MODE_INVALID_DATES) flags |= TIME_INVALID_DATES;

    val_datetime(&ltime, flags);
    return null_value ? nullptr
           : ltime.time_type == MYSQL_TIMESTAMP_TIME
               ? time2my_decimal(&ltime, decimal_value)
               : date2my_decimal(&ltime, decimal_value);
  }
}

bool Item_temporal_hybrid_func::get_date(MYSQL_TIME *ltime,
                                         my_time_flags_t fuzzy_date) {
  MYSQL_TIME tm;
  if (val_datetime(&tm, fuzzy_date)) {
    assert(null_value == true);
    return true;
  }
  if (data_type() == MYSQL_TYPE_TIME || tm.time_type == MYSQL_TIMESTAMP_TIME)
    time_to_datetime(current_thd, &tm, ltime);
  else
    *ltime = tm;
  return false;
}

bool Item_temporal_hybrid_func::get_time(MYSQL_TIME *ltime) {
  if (val_datetime(ltime, TIME_FUZZY_DATE)) {
    assert(null_value == true);
    return true;
  }
  if (data_type() == MYSQL_TYPE_TIME &&
      ltime->time_type != MYSQL_TIMESTAMP_TIME)
    datetime_to_time(ltime);
  return false;
}

String *Item_temporal_hybrid_func::val_str_ascii(String *str) {
  assert(fixed);
  MYSQL_TIME ltime;

  if (val_datetime(&ltime, TIME_FUZZY_DATE) ||
      (null_value =
           my_TIME_to_str(&ltime, str,
                          data_type() == MYSQL_TYPE_STRING
                              ? ltime.second_part ? DATETIME_MAX_DECIMALS : 0
                              : decimals)))
    return nullptr;

  /* Check that the returned timestamp type matches to the function type */
  assert((data_type() == MYSQL_TYPE_TIME &&
          ltime.time_type == MYSQL_TIMESTAMP_TIME) ||
         (data_type() == MYSQL_TYPE_DATE &&
          ltime.time_type == MYSQL_TIMESTAMP_DATE) ||
         (data_type() == MYSQL_TYPE_DATETIME &&
          ltime.time_type == MYSQL_TIMESTAMP_DATETIME) ||
         data_type() == MYSQL_TYPE_STRING ||
         ltime.time_type == MYSQL_TIMESTAMP_NONE);
  return str;
}

longlong Item_time_func::val_time_temporal() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_time(&ltime) ? 0LL : TIME_to_longlong_time_packed(ltime);
}

longlong Item_date_func::val_date_temporal() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_date(&ltime, TIME_FUZZY_DATE)
             ? 0LL
             : TIME_to_longlong_date_packed(ltime);
}

longlong Item_datetime_func::val_date_temporal() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_date(&ltime, TIME_FUZZY_DATE)
             ? 0LL
             : TIME_to_longlong_datetime_packed(ltime);
}

bool Item_date_literal::eq_specific(const Item *item) const {
  return cached_time.eq(
      down_cast<const Item_date_literal *>(item)->cached_time);
}

void Item_date_literal::print(const THD *, String *str, enum_query_type) const {
  str->append("DATE'");
  str->append(cached_time.cptr());
  str->append('\'');
}

bool Item_datetime_literal::eq_specific(const Item *item) const {
  return cached_time.eq(
      down_cast<const Item_datetime_literal *>(item)->cached_time);
}

void Item_datetime_literal::print(const THD *, String *str,
                                  enum_query_type) const {
  str->append("TIMESTAMP'");
  str->append(cached_time.cptr());
  str->append('\'');
}

bool Item_time_literal::eq_specific(const Item *item) const {
  return cached_time.eq(
      down_cast<const Item_time_literal *>(item)->cached_time);
}

void Item_time_literal::print(const THD *, String *str, enum_query_type) const {
  str->append("TIME'");
  str->append(cached_time.cptr());
  str->append('\'');
}

bool Item_func_at_time_zone::resolve_type(THD *thd) {
  if (check_type()) return true;

  if (strcmp(specifier_string(), "+00:00") != 0 &&
      (m_is_interval || strcmp(specifier_string(), "UTC") != 0)) {
    my_error(ER_UNKNOWN_TIME_ZONE, MYF(0), specifier_string());
    return true;
  }

  return set_time_zone(thd);
}

void Item_func_at_time_zone::print(const THD *thd, String *str,
                                   enum_query_type query_type) const {
  args[0]->print(thd, str, query_type);
  str->append(" AT TIME ZONE '+00:00'");
}

bool Item_func_at_time_zone::set_time_zone(THD *thd) {
  String s(m_specifier_string, strlen(m_specifier_string),
           &my_charset_utf8mb3_bin);
  m_tz = my_tz_find(thd, &s);
  if (m_tz == nullptr) {
    my_error(ER_UNKNOWN_TIME_ZONE, MYF(0), m_specifier_string);
    return true;
  }
  return false;
}

bool Item_func_at_time_zone::get_date(MYSQL_TIME *res, my_time_flags_t flags) {
  my_timeval tm;
  int warnings = 0;

  if (args[0]->data_type() == MYSQL_TYPE_TIMESTAMP) {
    if (args[0]->get_timeval(&tm, &warnings)) {
      null_value = true;
      return true;
    }

    m_tz->gmt_sec_to_TIME(res, tm.m_tv_sec);
    return warnings != 0;
  }

  const bool is_error = args[0]->get_date(res, flags);
  null_value = args[0]->null_value;
  if (is_error || null_value) return true;
  // Datetime value is in local time zone, convert to UTC:
  if (datetime_to_timeval(res, *current_thd->time_zone(), &tm, &warnings))
    return true;  // Value is out of the supported range
  // Finally, convert the temporal value to the desired time zone:
  m_tz->gmt_sec_to_TIME(res, tm.m_tv_sec);
  return warnings != 0;
}

bool Item_func_at_time_zone::check_type() const {
  if (args[0]->data_type() == MYSQL_TYPE_TIMESTAMP) return false;
  // A NULL literal must be allowed, and it has this type.
  if (args[0]->data_type() == MYSQL_TYPE_NULL) return false;

  if (args[0]->type() == Item::FUNC_ITEM &&
      down_cast<const Item_func *>(args[0])->functype() ==
          Item_func::DATETIME_LITERAL)
    return false;

  my_error(ER_INVALID_CAST, MYF(0), "TIMESTAMP WITH TIME ZONE");
  return true;
}

bool Item_func_period_add::resolve_type(THD *thd) {
  if (reject_vector_args()) return true;
  return param_type_is_default(thd, 0, -1, MYSQL_TYPE_LONGLONG);
}

longlong Item_func_period_add::val_int() {
  assert(fixed);
  const longlong period = args[0]->val_int();
  if ((null_value = args[0]->null_value)) return 0;
  const longlong months = args[1]->val_int();
  if ((null_value = args[1]->null_value)) return 0;

  if (!valid_period(period)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_int();
  }
  return convert_month_to_period(convert_period_to_month(period) + months);
}

bool Item_func_period_diff::resolve_type(THD *thd) {
  if (reject_vector_args()) return true;
  return param_type_is_default(thd, 0, -1, MYSQL_TYPE_LONGLONG);
}

longlong Item_func_period_diff::val_int() {
  assert(fixed);
  const longlong period1 = args[0]->val_int();
  if ((null_value = args[0]->null_value)) return 0;
  const longlong period2 = args[1]->val_int();
  if ((null_value = args[1]->null_value)) return 0;

  if (!valid_period(period1) || !valid_period(period2)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_int();
  }
  return static_cast<longlong>(convert_period_to_month(period1)) -
         static_cast<longlong>(convert_period_to_month(period2));
}

bool Item_func_to_days::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // The maximum string length returned by TO_DAYS is 7, as its range is
  // [0000-01-01, 9999-12-31] -> [0, 3652424]. Set the maximum length to one
  // higher, to account for the sign, even though the function never returns
  // negative values. (Needed in order to get decimal_precision() to return a
  // correct value.)
  fix_char_length(8);
  assert(decimal_precision() == 7);
  set_nullable(true);
  return false;
}

longlong Item_func_to_days::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;
  return (longlong)calc_daynr(ltime.year, ltime.month, ltime.day);
}

longlong Item_func_to_seconds::val_int_endpoint(bool, bool *) {
  assert(fixed);
  MYSQL_TIME ltime;
  longlong seconds;
  longlong days;
  int dummy; /* unused */
  if (get_arg0_date(&ltime, TIME_FUZZY_DATE)) {
    /* got NULL, leave the incl_endp intact */
    return LLONG_MIN;
  }
  seconds = ltime.hour * 3600L + ltime.minute * 60 + ltime.second;
  seconds = ltime.neg ? -seconds : seconds;
  days = (longlong)calc_daynr(ltime.year, ltime.month, ltime.day);
  seconds += days * 24L * 3600L;
  /* Set to NULL if invalid date, but keep the value */
  null_value = check_date(ltime, non_zero_date(ltime),
                          (TIME_NO_ZERO_IN_DATE | TIME_NO_ZERO_DATE), &dummy);
  /*
    Even if the evaluation return NULL, seconds is useful for pruning
  */
  return seconds;
}

bool Item_func_to_seconds::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  set_nullable(true);
  return false;
}

longlong Item_func_to_seconds::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  longlong seconds;
  longlong days;
  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;
  seconds = ltime.hour * 3600L + ltime.minute * 60 + ltime.second;
  seconds = ltime.neg ? -seconds : seconds;
  days = (longlong)calc_daynr(ltime.year, ltime.month, ltime.day);
  return seconds + days * 24L * 3600L;
}

/*
  Get information about this Item tree monotonicity

  SYNOPSIS
    Item_func_to_days::get_monotonicity_info()

  DESCRIPTION
  Get information about monotonicity of the function represented by this item
  tree.

  RETURN
    See enum_monotonicity_info.
*/

enum_monotonicity_info Item_func_to_days::get_monotonicity_info() const {
  if (args[0]->type() == Item::FIELD_ITEM) {
    if (args[0]->data_type() == MYSQL_TYPE_DATE)
      return MONOTONIC_STRICT_INCREASING_NOT_NULL;
    if (args[0]->data_type() == MYSQL_TYPE_DATETIME)
      return MONOTONIC_INCREASING_NOT_NULL;
  }
  return NON_MONOTONIC;
}

enum_monotonicity_info Item_func_to_seconds::get_monotonicity_info() const {
  if (args[0]->type() == Item::FIELD_ITEM) {
    if (args[0]->data_type() == MYSQL_TYPE_DATE ||
        args[0]->data_type() == MYSQL_TYPE_DATETIME)
      return MONOTONIC_STRICT_INCREASING_NOT_NULL;
  }
  return NON_MONOTONIC;
}

longlong Item_func_to_days::val_int_endpoint(bool left_endp, bool *incl_endp) {
  assert(fixed);
  MYSQL_TIME ltime;
  longlong res;
  int dummy; /* unused */
  if (get_arg0_date(&ltime, TIME_FUZZY_DATE)) {
    /* got NULL, leave the incl_endp intact */
    return LLONG_MIN;
  }
  res = (longlong)calc_daynr(ltime.year, ltime.month, ltime.day);
  /* Set to NULL if invalid date, but keep the value */
  null_value = check_date(ltime, non_zero_date(ltime),
                          (TIME_NO_ZERO_IN_DATE | TIME_NO_ZERO_DATE), &dummy);
  if (null_value) {
    /*
      Even if the evaluation return NULL, the calc_daynr is useful for pruning
    */
    if (args[0]->data_type() != MYSQL_TYPE_DATE) *incl_endp = true;
    return res;
  }

  if (args[0]->data_type() == MYSQL_TYPE_DATE) {
    // TO_DAYS() is strictly monotonic for dates, leave incl_endp intact
    return res;
  }

  /*
    Handle the special but practically useful case of datetime values that
    point to day bound ("strictly less" comparison stays intact):

      col < '2007-09-15 00:00:00'  -> TO_DAYS(col) <  TO_DAYS('2007-09-15')
      col > '2007-09-15 23:59:59'  -> TO_DAYS(col) >  TO_DAYS('2007-09-15')

    which is different from the general case ("strictly less" changes to
    "less or equal"):

      col < '2007-09-15 12:34:56'  -> TO_DAYS(col) <= TO_DAYS('2007-09-15')
  */
  if ((!left_endp &&
       !(ltime.hour || ltime.minute || ltime.second || ltime.second_part)) ||
      (left_endp && ltime.hour == 23 && ltime.minute == 59 &&
       ltime.second == 59))
    /* do nothing */
    ;
  else
    *incl_endp = true;
  return res;
}

bool Item_func_dayofyear::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Returns a value in the range [1, 366], so max three digits. Add one to the
  // character length for the sign.
  fix_char_length(4);
  assert(decimal_precision() == 3);
  set_nullable(true);
  return false;
}

longlong Item_func_dayofyear::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;
  return (longlong)calc_daynr(ltime.year, ltime.month, ltime.day) -
         calc_daynr(ltime.year, 1, 1) + 1;
}

bool Item_func_dayofmonth::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Returns a value in the range [0, 31], so max two digits. Add one to the
  // character length for the sign.
  fix_char_length(3);
  assert(decimal_precision() == 2);
  set_nullable(true);
  return false;
}

longlong Item_func_dayofmonth::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_date(&ltime, TIME_FUZZY_DATE) ? 0 : (longlong)ltime.day;
}

bool Item_func_month::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Returns a value in the range [1, 12], so max two digits. Add one to the
  // character length for the sign.
  fix_char_length(3);
  assert(decimal_precision() == 2);
  set_nullable(true);
  return false;
}

longlong Item_func_month::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_date(&ltime, TIME_FUZZY_DATE) ? 0 : (longlong)ltime.month;
}

bool Item_func_monthname::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  const CHARSET_INFO *cs = thd->variables.collation_connection;
  const uint32 repertoire = my_charset_repertoire(cs);
  locale = thd->variables.lc_time_names;
  collation.set(cs, DERIVATION_COERCIBLE, repertoire);
  set_data_type_string(locale->max_month_name_length);
  set_nullable(true);
  return false;
}

String *Item_func_monthname::val_str(String *str) {
  assert(fixed);
  const char *month_name;
  uint err;
  MYSQL_TIME ltime;

  if ((null_value = (get_arg0_date(&ltime, TIME_FUZZY_DATE) || !ltime.month)))
    return (String *)nullptr;

  month_name = locale->month_names->type_names[ltime.month - 1];
  str->copy(month_name, strlen(month_name), &my_charset_utf8mb3_bin,
            collation.collation, &err);
  return str;
}

bool Item_func_quarter::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Always one digit [1, 4]. Add one character for the sign.
  fix_char_length(2);
  assert(decimal_precision() == 1);
  set_nullable(true);
  return false;
}

/**
  Returns the quarter of the year.
*/

longlong Item_func_quarter::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_FUZZY_DATE)) return 0;
  return (longlong)((ltime.month + 2) / 3);
}

bool Item_func_hour::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Can have up to three digits (TIME_MAX_HOUR == 838). Add one for the sign.
  fix_char_length(4);
  assert(decimal_precision() == 3);
  set_nullable(true);
  return false;
}

longlong Item_func_hour::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_time(&ltime) ? 0 : ltime.hour;
}

bool Item_func_minute::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Can have up to two digits [0, 59]. Add one for the sign.
  fix_char_length(3);
  assert(decimal_precision() == 2);
  set_nullable(true);
  return false;
}

longlong Item_func_minute::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_time(&ltime) ? 0 : ltime.minute;
}

bool Item_func_second::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Can have up to two digits [0, 59]. Add one for the sign.
  fix_char_length(3);
  assert(decimal_precision() == 2);
  set_nullable(true);
  return false;
}

/**
  Returns the second in time_exp in the range of 0 - 59.
*/
longlong Item_func_second::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_time(&ltime) ? 0 : ltime.second;
}

static uint week_mode(uint mode) {
  uint week_format = (mode & 7);
  if (!(week_format & WEEK_MONDAY_FIRST)) week_format ^= WEEK_FIRST_WEEKDAY;
  return week_format;
}

bool Item_func_week::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (args[1] == nullptr) {
    THD *thd = pc->thd;
    args[1] = new (pc->mem_root)
        Item_int(NAME_STRING("0"), thd->variables.default_week_format, 1);
    if (args[1] == nullptr) return true;
  }
  return super::do_itemize(pc, res);
}

bool Item_func_week::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (param_type_is_default(thd, 1, 2, MYSQL_TYPE_LONGLONG)) return true;
  if (reject_vector_args()) return true;
  // Can have up to two digits [0, 53] (0 when using WEEK_YEAR, otherwise [1,
  // 53]). Add one for the sign.
  fix_char_length(3);
  assert(decimal_precision() == 2);
  set_nullable(true);
  return false;
}

/**
 @verbatim
  The bits in week_format(for calc_week() function) has the following meaning:
   WEEK_MONDAY_FIRST (0)  If not set	Sunday is first day of week
                          If set	Monday is first day of week
   WEEK_YEAR (1)	  If not set	Week is in range 0-53

        Week 0 is returned for the the last week of the previous year (for
        a date at start of january) In this case one can get 53 for the
        first week of next year.  This flag ensures that the week is
        relevant for the given year. Note that this flag is only
        relevant if WEEK_JANUARY is not set.

                          If set	 Week is in range 1-53.

        In this case one may get week 53 for a date in January (when
        the week is that last week of previous year) and week 1 for a
        date in December.

  WEEK_FIRST_WEEKDAY (2)  If not set	Weeks are numbered according
                                        to ISO 8601:1988
                          If set	The week that contains the first
                                        'first-day-of-week' is week 1.

        ISO 8601:1988 means that if the week containing January 1 has
        four or more days in the new year, then it is week 1;
        Otherwise it is the last week of the previous year, and the
        next week is week 1.
 @endverbatim
*/

longlong Item_func_week::val_int() {
  assert(fixed);
  uint year;
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;
  return (longlong)calc_week(ltime, week_mode((uint)args[1]->val_int()), &year);
}

bool Item_func_yearweek::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (param_type_is_default(thd, 1, 2, MYSQL_TYPE_LONGLONG)) return true;
  if (reject_vector_args()) return true;
  // Returns six digits (YYYYWW). Add one character for the sign.
  fix_char_length(7);
  assert(decimal_precision() == 6);
  set_nullable(true);
  return false;
}

longlong Item_func_yearweek::val_int() {
  assert(fixed);
  uint year, week;
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;
  week = calc_week(ltime, (week_mode((uint)args[1]->val_int()) | WEEK_YEAR),
                   &year);
  return week + year * 100;
}

bool Item_func_weekday::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  // Always one digit (either [0, 6] or [1, 7], depending on whether it's called
  // as WEEKDAY or DAYOFWEEK). Add one character for the sign.
  fix_char_length(2);
  set_nullable(true);
  return false;
}

longlong Item_func_weekday::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;

  if (get_arg0_date(&ltime, TIME_NO_ZERO_DATE)) return 0;

  return (longlong)calc_weekday(calc_daynr(ltime.year, ltime.month, ltime.day),
                                odbc_type) +
         odbc_type;
}

bool Item_func_dayname::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  const CHARSET_INFO *cs = thd->variables.collation_connection;
  const uint32 repertoire = my_charset_repertoire(cs);
  locale = thd->variables.lc_time_names;
  collation.set(cs, DERIVATION_COERCIBLE, repertoire);
  set_data_type_string(locale->max_day_name_length);
  set_nullable(true);
  return false;
}

String *Item_func_dayname::val_str(String *str) {
  assert(fixed);
  uint weekday = (uint)val_int();  // Always Item_func_daynr()
  const char *day_name;
  uint err;

  if (null_value) return (String *)nullptr;

  day_name = locale->day_names->type_names[weekday];
  str->copy(day_name, strlen(day_name), &my_charset_utf8mb3_bin,
            collation.collation, &err);
  return str;
}

bool Item_func_year::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  set_data_type_year();
  set_nullable(true);
  return false;
}

longlong Item_func_year::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_date(&ltime, TIME_FUZZY_DATE) ? 0 : (longlong)ltime.year;
}

bool Item_typecast_year::resolve_type(THD *thd) {
  if (reject_geometry_args()) return true;
  if (reject_vector_args()) return true;
  if (args[0]->propagate_type(thd, MYSQL_TYPE_YEAR, false, true)) return true;
  assert(decimal_precision() == 4);
  set_nullable(true);
  return false;
}

void Item_typecast_year::print(const THD *thd, String *str,
                               enum_query_type query_type) const {
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(thd, str, query_type);
  str->append(STRING_WITH_LEN(" as year)"));
}

longlong Item_typecast_year::val_int() {
  assert(fixed);
  longlong value{0};
  THD *thd = current_thd;
  null_value = false;

  // For temporal values, the YEAR value is extracted directly
  if (args[0]->is_temporal() && args[0]->data_type() != MYSQL_TYPE_YEAR) {
    MYSQL_TIME ltime;
    if (!get_arg0_date(&ltime, TIME_FUZZY_DATE))
      value = static_cast<longlong>(ltime.year);
  } else {
    const bool is_int_type = args[0]->cast_to_int_type() != STRING_RESULT;
    // For numeric data types, the int value is extracted
    if (is_int_type) {
      value = args[0]->val_int();
      null_value = args[0]->null_value;
    } else {
      // For string-based data types, attempt int value conversion
      StringBuffer<STRING_BUFFER_USUAL_SIZE> string_buffer;
      const String *string_value;
      if (!(string_value = args[0]->val_str(&string_buffer))) {
        null_value = true;
        return 0;
      }
      const CHARSET_INFO *const cs = string_value->charset();
      const char *const start = string_value->ptr();
      const char *const end_of_string = start + string_value->length();
      const char *end_of_number = end_of_string;
      int error{0};
      value = cs->cset->strtoll10(cs, start, &end_of_number, &error);
      // Report here the error as we have access to the string value
      // extracted by val_str.
      if (error != 0) {
        const ErrConvString err(string_value);
        push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                            ER_WRONG_VALUE, ER_THD(current_thd, ER_WRONG_VALUE),
                            "YEAR", err.ptr());
        null_value = true;
        return 0;
      }
      if (end_of_number != end_of_string) {
        const ErrConvString err(string_value);
        push_warning_printf(
            thd, Sql_condition::SL_WARNING, ER_TRUNCATED_WRONG_VALUE,
            ER_THD(current_thd, ER_TRUNCATED_WRONG_VALUE), "YEAR", err.ptr());
      }
    }
    // Only for string values we replace 0 with 2000
    if (!is_int_type && value == 0) value += 2000;
    // Values in the interval (0,70) represent years in the range [2000,2070)
    if (value > 0 && value < 70) value += 2000;
    // Values in the interval [70,100) represent years in the range [1970,2000)
    if (value >= 70 && value < 100) value += 1900;
  }
  // If date extraction failed or the YEAR value is outside the allowed range
  if (value > 2155 || (value < 1901 && (value != 0))) {
    const ErrConvString err(value);
    push_warning_printf(
        thd, Sql_condition::SL_WARNING, ER_TRUNCATED_WRONG_VALUE,
        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), "YEAR", err.ptr());
    null_value = true;
    return 0;
  }

  return value;
}
/*
  Get information about this Item tree monotonicity

  SYNOPSIS
    Item_func_year::get_monotonicity_info()

  DESCRIPTION
  Get information about monotonicity of the function represented by this item
  tree.

  RETURN
    See enum_monotonicity_info.
*/

enum_monotonicity_info Item_func_year::get_monotonicity_info() const {
  if (args[0]->type() == Item::FIELD_ITEM &&
      (args[0]->data_type() == MYSQL_TYPE_DATE ||
       args[0]->data_type() == MYSQL_TYPE_DATETIME))
    return MONOTONIC_INCREASING;
  return NON_MONOTONIC;
}

longlong Item_func_year::val_int_endpoint(bool left_endp, bool *incl_endp) {
  assert(fixed);
  MYSQL_TIME ltime;
  if (get_arg0_date(&ltime, TIME_FUZZY_DATE)) {
    /* got NULL, leave the incl_endp intact */
    return LLONG_MIN;
  }

  /*
    Handle the special but practically useful case of datetime values that
    point to year bound ("strictly less" comparison stays intact) :

      col < '2007-01-01 00:00:00'  -> YEAR(col) <  2007

    which is different from the general case ("strictly less" changes to
    "less or equal"):

      col < '2007-09-15 23:00:00'  -> YEAR(col) <= 2007
  */
  if (!left_endp && ltime.day == 1 && ltime.month == 1 &&
      !(ltime.hour || ltime.minute || ltime.second || ltime.second_part))
    ; /* do nothing */
  else
    *incl_endp = true;
  return ltime.year;
}

longlong Item_timeval_func::val_int() {
  my_timeval tm;
  return val_timeval(&tm) ? 0 : tm.m_tv_sec;
}

my_decimal *Item_timeval_func::val_decimal(my_decimal *decimal_value) {
  my_timeval tm;
  if (val_timeval(&tm)) {
    return error_decimal(decimal_value);
  }
  return timeval2my_decimal(&tm, decimal_value);
}

double Item_timeval_func::val_real() {
  my_timeval tm;
  return val_timeval(&tm)
             ? 0
             : (double)tm.m_tv_sec + (double)tm.m_tv_usec / (double)1000000;
}

String *Item_timeval_func::val_str(String *str) {
  my_timeval tm;
  if (val_timeval(&tm) || (null_value = str->alloc(MAX_DATE_STRING_REP_LENGTH)))
    return (String *)nullptr;
  str->length(my_timeval_to_str(&tm, str->ptr(), decimals));
  str->set_charset(collation.collation);
  return str;
}

bool Item_func_unix_timestamp::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (super::do_itemize(pc, res)) return true;
  if (arg_count == 0) pc->thd->lex->safe_to_cache_query = false;
  return false;
}

/**
   @retval true  args[0] is SQL NULL, so item is set to SQL NULL
   @retval false item's value is set, to 0 if out of range
*/
bool Item_func_unix_timestamp::val_timeval(my_timeval *tm) {
  assert(fixed);
  if (arg_count == 0) {
    tm->m_tv_sec = current_thd->query_start_in_secs();
    tm->m_tv_usec = 0;
    // no args: null_value is set in constructor and is always false.
    return false;
  }
  int warnings = 0;
  return (null_value = args[0]->get_timeval(tm, &warnings));
}

enum_monotonicity_info Item_func_unix_timestamp::get_monotonicity_info() const {
  if (args[0]->type() == Item::FIELD_ITEM &&
      (args[0]->data_type() == MYSQL_TYPE_TIMESTAMP))
    return MONOTONIC_INCREASING;
  return NON_MONOTONIC;
}

longlong Item_func_unix_timestamp::val_int_endpoint(bool, bool *) {
  assert(fixed);
  assert(arg_count == 1 && args[0]->type() == Item::FIELD_ITEM &&
         args[0]->data_type() == MYSQL_TYPE_TIMESTAMP);
  /* Leave the incl_endp intact */
  my_timeval tm;
  return val_timeval(&tm) ? 0 : tm.m_tv_sec;
}

bool Item_func_time_to_sec::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_TIME)) return true;
  if (reject_vector_args()) return true;
  fix_char_length(10);
  set_nullable(true);
  return false;
}

longlong Item_func_time_to_sec::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  longlong seconds;
  if (get_arg0_time(&ltime)) return 0;
  seconds = ltime.hour * 3600L + ltime.minute * 60 + ltime.second;
  return ltime.neg ? -seconds : seconds;
}

/**
  Convert a string to a interval value.

  To make code easy, allow interval objects without separators.
*/

bool get_interval_value(Item *args, interval_type int_type, String *str_value,
                        Interval *interval) {
  ulonglong array[5];
  longlong value = 0;

  memset(interval, 0, sizeof(*interval));
  if (int_type == INTERVAL_SECOND && args->decimals) {
    my_decimal decimal_value, *val;
    lldiv_t tmp;
    if (!(val = args->val_decimal(&decimal_value))) return true;
    if (args->null_value) return true;
    const int lldiv_result = my_decimal2lldiv_t(E_DEC_FATAL_ERROR, val, &tmp);
    if (lldiv_result == E_DEC_OVERFLOW) return true;

    if (tmp.quot >= 0 && tmp.rem >= 0) {
      interval->neg = false;
      interval->second = tmp.quot;
      interval->second_part = tmp.rem / 1000;
    } else {
      interval->neg = true;
      interval->second = -tmp.quot;
      interval->second_part = -tmp.rem / 1000;
    }
    return false;
  } else if (int_type <= INTERVAL_MICROSECOND) {
    value = args->val_int();
    if (args->null_value) return true;
    /*
      Large floating-point values will be truncated to LLONG_MIN / LLONG_MAX
      LLONG_MIN cannot be negated below, so reject it here.
    */
    if (value == LLONG_MIN) return true;
    if (value < 0) {
      interval->neg = true;
      value = -value;
    }
  }

  switch (int_type) {
    case INTERVAL_YEAR:
      interval->year = (ulong)value;
      break;
    case INTERVAL_QUARTER:
      if (value >= UINT_MAX / 3) return true;
      interval->month = (ulong)(value * 3);
      break;
    case INTERVAL_MONTH:
      interval->month = (ulong)value;
      break;
    case INTERVAL_WEEK:
      if (value >= UINT_MAX / 7) return true;
      interval->day = (ulong)(value * 7);
      break;
    case INTERVAL_DAY:
      interval->day = (ulong)value;
      break;
    case INTERVAL_HOUR:
      interval->hour = (ulong)value;
      break;
    case INTERVAL_MINUTE:
      interval->minute = value;
      break;
    case INTERVAL_SECOND:
      interval->second = value;
      break;
    case INTERVAL_MICROSECOND:
      interval->second_part = value;
      break;
    case INTERVAL_YEAR_MONTH:  // Allow YEAR-MONTH YYYYYMM
      if (get_interval_info(args, str_value, &interval->neg, 2, array, false))
        return true;
      interval->year = (ulong)array[0];
      interval->month = (ulong)array[1];
      break;
    case INTERVAL_DAY_HOUR:
      if (get_interval_info(args, str_value, &interval->neg, 2, array, false))
        return true;
      interval->day = (ulong)array[0];
      interval->hour = (ulong)array[1];
      break;
    case INTERVAL_DAY_MINUTE:
      if (get_interval_info(args, str_value, &interval->neg, 3, array, false))
        return true;
      interval->day = (ulong)array[0];
      interval->hour = (ulong)array[1];
      interval->minute = array[2];
      break;
    case INTERVAL_DAY_SECOND:
      if (get_interval_info(args, str_value, &interval->neg, 4, array, false))
        return true;
      interval->day = (ulong)array[0];
      interval->hour = (ulong)array[1];
      interval->minute = array[2];
      interval->second = array[3];
      break;
    case INTERVAL_HOUR_MINUTE:
      if (get_interval_info(args, str_value, &interval->neg, 2, array, false))
        return true;
      interval->hour = (ulong)array[0];
      interval->minute = array[1];
      break;
    case INTERVAL_HOUR_SECOND:
      if (get_interval_info(args, str_value, &interval->neg, 3, array, false))
        return true;
      interval->hour = (ulong)array[0];
      interval->minute = array[1];
      interval->second = array[2];
      break;
    case INTERVAL_MINUTE_SECOND:
      if (get_interval_info(args, str_value, &interval->neg, 2, array, false))
        return true;
      interval->minute = array[0];
      interval->second = array[1];
      break;
    case INTERVAL_DAY_MICROSECOND:
      if (get_interval_info(args, str_value, &interval->neg, 5, array, true))
        return true;
      interval->day = (ulong)array[0];
      interval->hour = (ulong)array[1];
      interval->minute = array[2];
      interval->second = array[3];
      interval->second_part = array[4];
      break;
    case INTERVAL_HOUR_MICROSECOND:
      if (get_interval_info(args, str_value, &interval->neg, 4, array, true))
        return true;
      interval->hour = (ulong)array[0];
      interval->minute = array[1];
      interval->second = array[2];
      interval->second_part = array[3];
      break;
    case INTERVAL_MINUTE_MICROSECOND:
      if (get_interval_info(args, str_value, &interval->neg, 3, array, true))
        return true;
      interval->minute = array[0];
      interval->second = array[1];
      interval->second_part = array[2];
      break;
    case INTERVAL_SECOND_MICROSECOND:
      if (get_interval_info(args, str_value, &interval->neg, 2, array, true))
        return true;
      interval->second = array[0];
      interval->second_part = array[1];
      break;
    case INTERVAL_LAST: /* purecov: begin deadcode */
      assert(0);
      break; /* purecov: end */
  }
  return false;
}

bool Item_func_from_days::get_date(MYSQL_TIME *ltime,
                                   my_time_flags_t fuzzy_date) {
  const longlong value = args[0]->val_int();
  if ((null_value = args[0]->null_value)) return true;
  memset(ltime, 0, sizeof(MYSQL_TIME));
  get_date_from_daynr(value, &ltime->year, &ltime->month, &ltime->day);

  if (check_datetime_range(*ltime)) {
    // Value is out of range, cannot use our printing functions to output it.
    push_warning_printf(
        current_thd, Sql_condition::SL_WARNING, ER_DATETIME_FUNCTION_OVERFLOW,
        ER_THD(current_thd, ER_DATETIME_FUNCTION_OVERFLOW), func_name());
    null_value = true;
    return true;
  }

  if ((null_value = (fuzzy_date & TIME_NO_ZERO_DATE) &&
                    (ltime->year == 0 || ltime->month == 0 || ltime->day == 0)))
    return true;

  ltime->time_type = MYSQL_TIMESTAMP_DATE;
  return false;
}

void MYSQL_TIME_cache::set_time(MYSQL_TIME *ltime, uint8 dec_arg) {
  assert(ltime->time_type == MYSQL_TIMESTAMP_TIME);
  time = *ltime;
  time_packed = TIME_to_longlong_time_packed(time);
  dec = dec_arg;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

void MYSQL_TIME_cache::set_date(MYSQL_TIME *ltime) {
  assert(ltime->time_type == MYSQL_TIMESTAMP_DATE);
  time = *ltime;
  time_packed = TIME_to_longlong_date_packed(time);
  dec = 0;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

void MYSQL_TIME_cache::set_datetime(MYSQL_TIME *ltime, uint8 dec_arg,
                                    const Time_zone *tz) {
  assert(ltime->time_type == MYSQL_TIMESTAMP_DATETIME ||
         ltime->time_type == MYSQL_TIMESTAMP_DATETIME_TZ);
  time = *ltime;
  if (convert_time_zone_displacement(tz, &time)) {
    assert(false);
  }
  time_packed = TIME_to_longlong_datetime_packed(time);

  dec = dec_arg;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

void MYSQL_TIME_cache::set_datetime(my_timeval tv, uint8 dec_arg,
                                    Time_zone *tz) {
  tz->gmt_sec_to_TIME(&time, tv);
  time_packed = TIME_to_longlong_datetime_packed(time);
  dec = dec_arg;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

void MYSQL_TIME_cache::set_date(my_timeval tv, Time_zone *tz) {
  tz->gmt_sec_to_TIME(&time, (my_time_t)tv.m_tv_sec);
  time.time_type = MYSQL_TIMESTAMP_DATE;
  /* We don't need to set second_part and neg because they are already 0 */
  time.hour = time.minute = time.second = 0;
  time_packed = TIME_to_longlong_date_packed(time);
  dec = 0;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

void MYSQL_TIME_cache::set_time(my_timeval tv, uint8 dec_arg, Time_zone *tz) {
  tz->gmt_sec_to_TIME(&time, tv);
  datetime_to_time(&time);
  time_packed = TIME_to_longlong_time_packed(time);
  dec = dec_arg;
  string_length = my_TIME_to_str(time, string_buff, decimals());
}

bool MYSQL_TIME_cache::get_date(MYSQL_TIME *ltime,
                                my_time_flags_t fuzzydate) const {
  int warnings;
  get_TIME(ltime);
  return check_date(*ltime, non_zero_date(*ltime), fuzzydate, &warnings);
}

String *MYSQL_TIME_cache::val_str(String *str) {
  str->set(string_buff, string_length, &my_charset_latin1);
  return str;
}

/* CURDATE() and UTC_DATE() */

bool Item_func_curdate::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (super::do_itemize(pc, res)) return true;
  pc->thd->lex->safe_to_cache_query = false;
  return false;
}

bool Item_func_curdate::resolve_type(THD *thd) {
  if (Item_date_func::resolve_type(thd)) return true;
  return false;
}

Time_zone *Item_func_curdate_local::time_zone() {
  return current_thd->time_zone();
}

Time_zone *Item_func_curdate_utc::time_zone() { return my_tz_UTC; }

longlong Item_func_curdate::val_date_temporal() {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_date(current_thd->query_start_timeval_trunc(decimals), time_zone());
  return tm.val_packed();
}

bool Item_func_curdate::get_date(MYSQL_TIME *res, my_time_flags_t) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_date(current_thd->query_start_timeval_trunc(decimals), time_zone());
  return tm.get_time(res);
}

String *Item_func_curdate::val_str(String *str) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_date(current_thd->query_start_timeval_trunc(decimals), time_zone());
  if (str->alloc(10)) return nullptr;

  str->set_charset(&my_charset_numeric);
  str->length(my_TIME_to_str(*tm.get_TIME_ptr(), (char *)str->ptr(), decimals));

  return str;
}

/* CURTIME() and UTC_TIME() */

bool Item_func_curtime::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (super::do_itemize(pc, res)) return true;
  pc->thd->lex->safe_to_cache_query = false;
  return false;
}

bool Item_func_curtime::resolve_type(THD *) {
  if (check_precision()) return true;

  set_data_type_time(decimals);

  /*
    Subtract 2 from MAX_TIME_WIDTH (which is 10) because:
    - there is no sign
    - hour is in the 2-digit range
  */
  max_length -= 2 * collation.collation->mbmaxlen;

  return false;
}

longlong Item_func_curtime::val_time_temporal() {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_time(current_thd->query_start_timeval_trunc(decimals), decimals,
              time_zone());
  return tm.val_packed();
}

bool Item_func_curtime::get_time(MYSQL_TIME *ltime) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_time(current_thd->query_start_timeval_trunc(decimals), decimals,
              time_zone());
  return tm.get_time(ltime);
}

String *Item_func_curtime::val_str(String *str) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_time(current_thd->query_start_timeval_trunc(decimals), decimals,
              time_zone());
  if (str->alloc(15)) return nullptr;

  str->set_charset(&my_charset_numeric);
  str->length(my_TIME_to_str(*tm.get_TIME_ptr(), (char *)str->ptr(), decimals));

  return str;
}

Time_zone *Item_func_curtime_local::time_zone() {
  return current_thd->time_zone();
}

Time_zone *Item_func_curtime_utc::time_zone() { return my_tz_UTC; }

/* NOW() and UTC_TIMESTAMP () */

bool Item_func_now::resolve_type(THD *) {
  if (check_precision()) return true;

  set_data_type_datetime(decimals);

  return false;
}

void Item_func_now_local::store_in(Field *field) {
  THD *thd = current_thd;
  const my_timeval tm = thd->query_start_timeval_trunc(field->decimals());
  field->set_notnull();
  return field->store_timestamp(&tm);
}

Time_zone *Item_func_now_local::time_zone() { return current_thd->time_zone(); }

bool Item_func_now_utc::do_itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (super::do_itemize(pc, res)) return true;
  pc->thd->lex->safe_to_cache_query = false;
  return false;
}

Time_zone *Item_func_now_utc::time_zone() { return my_tz_UTC; }

longlong Item_func_now::val_date_temporal() {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_datetime(current_thd->query_start_timeval_trunc(decimals), decimals,
                  time_zone());
  return tm.val_packed();
}

bool Item_func_now::get_date(MYSQL_TIME *res, my_time_flags_t) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_datetime(current_thd->query_start_timeval_trunc(decimals), decimals,
                  time_zone());
  return tm.get_time(res);
}

String *Item_func_now::val_str(String *str) {
  assert(fixed);
  MYSQL_TIME_cache tm;
  tm.set_datetime(current_thd->query_start_timeval_trunc(decimals), decimals,
                  time_zone());
  if (str->alloc(26)) return nullptr;

  str->set_charset(&my_charset_numeric);
  str->length(my_TIME_to_str(*tm.get_TIME_ptr(), (char *)str->ptr(), decimals));

  return str;
}

type_conversion_status Item_func_now::save_in_field_inner(Field *to, bool) {
  to->set_notnull();
  MYSQL_TIME_cache tm;
  tm.set_datetime(current_thd->query_start_timeval_trunc(decimals), decimals,
                  time_zone());

  return to->store_time(tm.get_TIME_ptr(), decimals);
}

/**
    Converts current time in my_time_t to MYSQL_TIME representation for local
    time zone. Defines time zone (local) used for whole SYSDATE function.
*/
bool Item_func_sysdate_local::get_date(MYSQL_TIME *now_time,
                                       my_time_flags_t fuzzy_date
                                       [[maybe_unused]]) {
  THD *thd = current_thd;
  const ulonglong tmp = my_micro_time();
  thd->time_zone()->gmt_sec_to_TIME(now_time, (my_time_t)(tmp / 1000000));
  if (decimals) {
    now_time->second_part = tmp % 1000000;
    my_datetime_trunc(now_time, decimals);
  }
  return false;
}

bool Item_func_sysdate_local::resolve_type(THD *) {
  if (check_precision()) return true;
  set_data_type_datetime(decimals);
  return false;
}

bool Item_func_sec_to_time::get_time(MYSQL_TIME *ltime) {
  my_decimal tmp, *val = args[0]->val_decimal(&tmp);
  if (val == nullptr) return (null_value = true);
  if ((null_value = args[0]->null_value)) return true;

  lldiv_t seconds;
  if (my_decimal2lldiv_t(0, val, &seconds)) {
    set_max_time(ltime, val->sign());
    return make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                        ErrConvString(val),
                                        MYSQL_TIMESTAMP_TIME, NullS);
  }
  if (sec_to_time(seconds, ltime))
    return make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                        ErrConvString(val),
                                        MYSQL_TIMESTAMP_TIME, NullS);
  return false;
}

bool Item_func_date_format::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (param_type_is_default(thd, 1, 2)) return true;
  if (reject_vector_args()) return true;
  /*
    Must use this_item() in case it's a local SP variable
    (for ->max_length and ->str_value)
  */
  Item *arg1 = args[1]->this_item();
  uint32 char_length;
  const CHARSET_INFO *cs = thd->variables.collation_connection;
  uint32 repertoire = arg1->collation.repertoire;
  if (!thd->variables.lc_time_names->is_ascii)
    repertoire |= MY_REPERTOIRE_EXTENDED;
  collation.set(cs, arg1->collation.derivation, repertoire);
  if (arg1->type() == STRING_ITEM) {  // Optimize the normal case
    fixed_length = true;
    String str;
    char_length = format_length(arg1->val_str(&str));
  } else {
    fixed_length = false;
    char_length = min(min(arg1->max_char_length(), uint32(MAX_BLOB_WIDTH)) * 10,
                      uint32(MAX_BLOB_WIDTH));
  }
  set_data_type_string(char_length);
  set_nullable(true);  // If wrong date
  return false;
}

bool Item_func_date_format::eq_specific(const Item *item) const {
  const Item_func_date_format *item_func =
      down_cast<const Item_func_date_format *>(item);
  /*
    Arguments have already been compared for equality with regular collation.
    However, the format string must be compared case sensitive, because
    format modifiers with different case, for example %m and %M,
    have different meanings.
  */
  if (args[1]->type() != STRING_ITEM ||
      item_func->args[1]->type() != STRING_ITEM) {
    return true;
  }
  const Item_string *str1 = down_cast<Item_string *>(args[1]);
  const Item_string *str2 = down_cast<Item_string *>(item_func->args[1]);

  return str1->eq_binary(str2);
}

uint Item_func_date_format::format_length(const String *format) {
  uint size = 0;
  const char *ptr = format->ptr();
  const char *end = ptr + format->length();

  for (; ptr != end; ptr++) {
    if (*ptr != '%' || ptr == end - 1)
      size++;
    else {
      switch (*++ptr) {
        case 'M':     /* month, textual */
        case 'W':     /* day (of the week), textual */
          size += 64; /* large for UTF8 locale data */
          break;
        case 'D': /* day (of the month), numeric plus english suffix */
        case 'Y': /* year, numeric, 4 digits */
        case 'x': /* Year, used with 'v' */
        case 'X': /* Year, used with 'v, where week starts with Monday' */
          size += 4;
          break;
        case 'a':     /* locale's abbreviated weekday name (Sun..Sat) */
        case 'b':     /* locale's abbreviated month name (Jan.Dec) */
          size += 32; /* large for UTF8 locale data */
          break;
        case 'j': /* day of year (001..366) */
          size += 3;
          break;
        case 'U': /* week (00..52) */
        case 'u': /* week (00..52), where week starts with Monday */
        case 'V': /* week 1..53 used with 'x' */
        case 'v': /* week 1..53 used with 'x', where week starts with Monday */
        case 'y': /* year, numeric, 2 digits */
        case 'm': /* month, numeric */
        case 'd': /* day (of the month), numeric */
        case 'h': /* hour (01..12) */
        case 'I': /* --||-- */
        case 'i': /* minutes, numeric */
        case 'l': /* hour ( 1..12) */
        case 'p': /* locale's AM or PM */
        case 'S': /* second (00..61) */
        case 's': /* seconds, numeric */
        case 'c': /* month (0..12) */
        case 'e': /* day (0..31) */
          size += 2;
          break;
        case 'k': /* hour ( 0..23) */
        case 'H': /* hour (00..23; value > 23 OK, padding always 2-digit) */
          size +=
              7; /* docs allow > 23, range depends on sizeof(unsigned int) */
          break;
        case 'r': /* time, 12-hour (hh:mm:ss [AP]M) */
          size += 11;
          break;
        case 'T': /* time, 24-hour (hh:mm:ss) */
          size += 8;
          break;
        case 'f': /* microseconds */
          size += 6;
          break;
        case 'w': /* day (of the week), numeric */
        case '%':
        default:
          size++;
          break;
      }
    }
  }
  return size;
}

String *Item_func_date_format::val_str(String *str) {
  String *format;
  MYSQL_TIME l_time;
  uint size;
  assert(fixed);

  if (!is_time_format) {
    if (get_arg0_date(&l_time, TIME_FUZZY_DATE)) return nullptr;
  } else {
    if (get_arg0_time(&l_time)) return nullptr;
    l_time.year = l_time.month = l_time.day = 0;
  }

  if (!(format = args[1]->val_str(str)) || !format->length()) goto null_date;

  if (fixed_length)
    size = max_length;
  else
    size = format_length(format);

  if (size < MAX_DATE_STRING_REP_LENGTH) size = MAX_DATE_STRING_REP_LENGTH;

  // If format uses the buffer provided by 'str' then store result locally.
  if (format == str || format->uses_buffer_owned_by(str)) str = &value;
  if (str->alloc(size)) goto null_date;

  Date_time_format date_time_format;
  date_time_format.format.str = format->ptr();
  date_time_format.format.length = format->length();

  /* Create the result string */
  str->set_charset(collation.collation);
  if (!make_date_time(
          &date_time_format, &l_time,
          is_time_format ? MYSQL_TIMESTAMP_TIME : MYSQL_TIMESTAMP_DATE, str))
    return str;

null_date:
  null_value = true;
  return nullptr;
}

bool Item_func_from_unixtime::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_NEWDECIMAL)) return true;
  if (reject_vector_args()) return true;
  set_data_type_datetime(min(args[0]->decimals, uint8{DATETIME_MAX_DECIMALS}));
  set_nullable(true);
  thd->time_zone_used = true;
  return false;
}

bool Item_func_from_unixtime::get_date(MYSQL_TIME *ltime,
                                       my_time_flags_t fuzzy_date
                                       [[maybe_unused]]) {
  THD *thd = current_thd;
  lldiv_t lld;
  if (decimals) {
    my_decimal *val, decimal_value;
    if (!(val = args[0]->val_decimal(&decimal_value)) || args[0]->null_value)
      return (null_value = true);
    if (0 != my_decimal2lldiv_t(E_DEC_FATAL_ERROR, val, &lld))
      return (null_value = true);
  } else {
    lld.quot = args[0]->val_int();
    lld.rem = 0;
  }

  // Return NULL for timestamps after 2038-01-19 03:14:07 UTC (32 bits OS time)
  // or after 3001-01-18 23:59:59 (64 bits OS time)
  if ((null_value = (args[0]->null_value || lld.quot > MYTIME_MAX_VALUE) ||
                    lld.quot < 0 || lld.rem < 0))
    return true;

  const bool is_end_of_epoch = (lld.quot == MYTIME_MAX_VALUE);

  thd->variables.time_zone->gmt_sec_to_TIME(ltime, (my_time_t)lld.quot);
  if (ltime->year == 0) {
    // Overflow can happen in time zones east of UTC on Dec 31
    null_value = true;
    return true;
  }
  int warnings = 0;
  ltime->second_part = decimals ? static_cast<ulong>(lld.rem / 1000) : 0;
  bool ret = propagate_datetime_overflow(
      thd, &warnings,
      datetime_add_nanoseconds_adjust_frac(ltime, lld.rem % 1000, &warnings,
                                           thd->is_fsp_truncate_mode()));
  // Disallow round-up to one second past end of epoch.
  if (decimals && is_end_of_epoch) {
    MYSQL_TIME max_ltime;
    thd->variables.time_zone->gmt_sec_to_TIME(&max_ltime, MYTIME_MAX_VALUE);
    max_ltime.second_part = 999999UL;

    const longlong max_t = TIME_to_longlong_datetime_packed(max_ltime);
    const longlong ret_t = TIME_to_longlong_datetime_packed(*ltime);
    // The first test below catches the situation with 64 bits time, the
    // second test catches it with 32 bits time
    if ((null_value =
             (warnings & MYSQL_TIME_WARN_OUT_OF_RANGE) || (ret_t > max_t)))
      return true;
  }
  return ret;
}

bool Item_func_convert_tz::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (param_type_is_default(thd, 1, -1)) return true;
  if (reject_vector_args()) return true;
  set_data_type_datetime(args[0]->datetime_precision());
  set_nullable(true);
  return false;
}

bool Item_func_convert_tz::get_date(MYSQL_TIME *ltime,
                                    my_time_flags_t fuzzy_date
                                    [[maybe_unused]]) {
  my_time_t my_time_tmp;
  String str;
  THD *thd = current_thd;

  if (!from_tz_cached) {
    from_tz = my_tz_find(thd, args[1]->val_str_ascii(&str));
    from_tz_cached = args[1]->const_item();
  }

  if (!to_tz_cached) {
    to_tz = my_tz_find(thd, args[2]->val_str_ascii(&str));
    to_tz_cached = args[2]->const_item();
  }

  if (from_tz == nullptr || to_tz == nullptr ||
      get_arg0_date(ltime, TIME_NO_ZERO_DATE)) {
    null_value = true;
    return true;
  }

  {
    bool not_used;
    const uint second_part = ltime->second_part;
    my_time_tmp = from_tz->TIME_to_gmt_sec(ltime, &not_used);
    /* my_time_tmp is guaranteed to be in the allowed range */
    if (my_time_tmp) {
      to_tz->gmt_sec_to_TIME(ltime, my_time_tmp);
      ltime->second_part = second_part;
    }
  }

  null_value = false;
  return false;
}

void Item_func_convert_tz::cleanup() {
  from_tz_cached = false;
  to_tz_cached = false;
  Item_datetime_func::cleanup();
}

bool Item_date_add_interval::resolve_type(THD *thd) {
  if (reject_geometry_args()) return true;
  if (reject_vector_args()) return true;
  set_nullable(true);

  /*
    If first argument is a dynamic parameter, type DATE is assumed if the
    provided interval is a YEAR, MONTH or DAY interval, otherwise
    type DATETIME is assumed.
    If the assumed type is DATE and the user provides a DATETIME on execution,
    a reprepare will happen.
  */
  const enum_field_types assumed_type =
      m_interval_type <= INTERVAL_DAY || m_interval_type == INTERVAL_YEAR_MONTH
          ? MYSQL_TYPE_DATE
          : MYSQL_TYPE_DATETIME;

  if (param_type_is_default(thd, 0, 1, assumed_type)) return true;
  /*
    Syntax may be:
    - either DATE_ADD(x, ?): then '?' is an integer number of days
    - or DATE_ADD(x, interval ? some_query_expression): then '?' may be
    an integer, a decimal, a string in format "days hours:minutes",
    depending on m_interval_type, see
    https://dev.mysql.com/doc/refman/8.0/en/date-and-time-functions.html#
    function_date-add
  */
  enum_field_types arg1_type;
  if (m_interval_type <= INTERVAL_MINUTE)
    arg1_type = MYSQL_TYPE_LONGLONG;
  else if (m_interval_type == INTERVAL_SECOND)  // decimals allowed
    arg1_type = MYSQL_TYPE_NEWDECIMAL;
  else if (m_interval_type == INTERVAL_MICROSECOND)
    arg1_type = MYSQL_TYPE_LONGLONG;
  else
    arg1_type = MYSQL_TYPE_VARCHAR;  // composite, as in "HOUR:MINUTE"
  if (param_type_is_default(thd, 1, 2, arg1_type)) return true;

  /*
    The result type of an Item_date_add_interval function is defined as follows:

    - If first argument is MYSQL_TYPE_DATETIME, result is MYSQL_TYPE_DATETIME.
    - If first argument is MYSQL_TYPE_DATE,
        if the interval type uses hour, minute or second,
        then type is MYSQL_TYPE_DATETIME, otherwise type is MYSQL_TYPE_DATE.
    - If first argument is MYSQL_TYPE_TIME,
        if the interval type uses year, month or days
        then type is MYSQL_TYPE_DATETIME, otherwise type is MYSQL_TYPE_TIME.
    - Otherwise the result is MYSQL_TYPE_STRING
      This is because the first argument is interpreted as a string which
      may contain a DATE, TIME or DATETIME value, but we don't know which yet.
  */
  const enum_field_types arg0_data_type = args[0]->data_type();
  uint8 interval_dec = 0;
  if (m_interval_type == INTERVAL_MICROSECOND ||
      (m_interval_type >= INTERVAL_DAY_MICROSECOND &&
       m_interval_type <= INTERVAL_SECOND_MICROSECOND))
    interval_dec = DATETIME_MAX_DECIMALS;
  else if (m_interval_type == INTERVAL_SECOND && args[1]->decimals > 0)
    interval_dec = min(args[1]->decimals, uint8{DATETIME_MAX_DECIMALS});

  if (arg0_data_type == MYSQL_TYPE_DATETIME ||
      arg0_data_type == MYSQL_TYPE_TIMESTAMP) {
    uint8 dec = max<uint8>(args[0]->datetime_precision(), interval_dec);
    set_data_type_datetime(dec);
  } else if (arg0_data_type == MYSQL_TYPE_DATE) {
    if (m_interval_type <= INTERVAL_DAY ||
        m_interval_type == INTERVAL_YEAR_MONTH)
      set_data_type_date();
    else
      set_data_type_datetime(interval_dec);
  } else if (arg0_data_type == MYSQL_TYPE_TIME) {
    if ((m_interval_type >= INTERVAL_HOUR &&
         m_interval_type <= INTERVAL_MICROSECOND) ||
        (m_interval_type >= INTERVAL_HOUR_MINUTE &&
         m_interval_type <= INTERVAL_SECOND_MICROSECOND)) {
      uint8 dec = max<uint8>(args[0]->time_precision(), interval_dec);
      set_data_type_time(dec);
    } else {
      uint8 dec = max<uint8>(args[0]->datetime_precision(), interval_dec);
      set_data_type_datetime(dec);
    }
  } else {
    /* Behave as a usual string function when return type is VARCHAR. */
    set_data_type_char(MAX_DATETIME_FULL_WIDTH, default_charset());
  }
  if (value.alloc(max_length)) return true;

  return false;
}

/* Here arg[1] is a Item_interval object */
bool Item_date_add_interval::get_date_internal(MYSQL_TIME *ltime,
                                               my_time_flags_t) {
  Interval interval;

  if (args[0]->get_date(ltime, TIME_NO_ZERO_DATE)) return (null_value = true);

  if (get_interval_value(args[1], m_interval_type, &value, &interval)) {
    // Do not warn about "overflow" for NULL
    if (!args[1]->null_value) {
      push_warning_printf(
          current_thd, Sql_condition::SL_WARNING, ER_DATETIME_FUNCTION_OVERFLOW,
          ER_THD(current_thd, ER_DATETIME_FUNCTION_OVERFLOW), func_name());
    }
    return (null_value = true);
  }

  if (m_subtract) interval.neg = !interval.neg;

  /*
    Make sure we return proper time_type.
    It's important for val_str().
  */
  if (data_type() == MYSQL_TYPE_DATE &&
      ltime->time_type == MYSQL_TIMESTAMP_DATETIME)
    datetime_to_date(ltime);
  else if (data_type() == MYSQL_TYPE_DATETIME &&
           ltime->time_type == MYSQL_TIMESTAMP_DATE)
    date_to_datetime(ltime);

  if ((null_value = date_add_interval_with_warn(current_thd, ltime,
                                                m_interval_type, interval)))
    return true;
  return false;
}

bool Item_date_add_interval::get_time_internal(MYSQL_TIME *ltime) {
  Interval interval;

  null_value = args[0]->get_time(ltime) ||
               get_interval_value(args[1], m_interval_type, &value, &interval);
  if (null_value) {
    return true;
  }
  if (m_subtract) interval.neg = !interval.neg;

  assert(!check_time_range_quick(*ltime));

  const longlong usec1 =
      ((((ltime->day * 24 + ltime->hour) * 60 + ltime->minute) * 60 +
        ltime->second) *
           1000000LL +
       ltime->second_part) *
      (ltime->neg ? -1 : 1);
  const longlong usec2 =
      ((((interval.day * 24 + interval.hour) * 60 + interval.minute) * 60 +
        interval.second) *
           1000000LL +
       interval.second_part) *
      (interval.neg ? -1 : 1);

  // Possible overflow adding date and interval values below.
  if ((usec1 > 0 && usec2 > 0) || (usec1 < 0 && usec2 < 0)) {
    lldiv_t usec2_as_seconds;
    usec2_as_seconds.quot = usec2 / 1000000;
    usec2_as_seconds.rem = 0;
    MYSQL_TIME unused;
    if ((null_value = sec_to_time(usec2_as_seconds, &unused))) {
      push_warning_printf(
          current_thd, Sql_condition::SL_WARNING, ER_DATETIME_FUNCTION_OVERFLOW,
          ER_THD(current_thd, ER_DATETIME_FUNCTION_OVERFLOW), "time");
      return true;
    }
  }

  const longlong diff = usec1 + usec2;
  lldiv_t seconds;
  seconds.quot = diff / 1000000;
  seconds.rem = diff % 1000000 * 1000; /* time->second_part= lldiv.rem / 1000 */
  if ((null_value =
           (interval.year || interval.month || sec_to_time(seconds, ltime)))) {
    push_warning_printf(
        current_thd, Sql_condition::SL_WARNING, ER_DATETIME_FUNCTION_OVERFLOW,
        ER_THD(current_thd, ER_DATETIME_FUNCTION_OVERFLOW), "time");
    return true;
  }
  return false;
}

bool Item_date_add_interval::val_datetime(MYSQL_TIME *ltime,
                                          my_time_flags_t fuzzy_date) {
  if (data_type() != MYSQL_TYPE_TIME)
    return get_date_internal(ltime, fuzzy_date | TIME_NO_ZERO_DATE);
  return get_time_internal(ltime);
}

bool Item_date_add_interval::eq_specific(const Item *item) const {
  const Item_date_add_interval *other =
      down_cast<const Item_date_add_interval *>(item);
  return m_interval_type == other->m_interval_type &&
         m_subtract == other->m_subtract;
}

/*
   'interval_names' reflects the order of the enumeration interval_type.
   See item_timefunc.h
 */

const char *interval_names[] = {"year",
                                "quarter",
                                "month",
                                "week",
                                "day",
                                "hour",
                                "minute",
                                "second",
                                "microsecond",
                                "year_month",
                                "day_hour",
                                "day_minute",
                                "day_second",
                                "hour_minute",
                                "hour_second",
                                "minute_second",
                                "day_microsecond",
                                "hour_microsecond",
                                "minute_microsecond",
                                "second_microsecond"};

void Item_date_add_interval::print(const THD *thd, String *str,
                                   enum_query_type query_type) const {
  str->append('(');
  args[0]->print(thd, str, query_type);
  str->append(m_subtract ? " - interval " : " + interval ");
  args[1]->print(thd, str, query_type);
  str->append(' ');
  str->append(interval_names[m_interval_type]);
  str->append(')');
}

void Item_date_add_interval::add_json_info(Json_object *obj) {
  super::add_json_info(obj);
  obj->add_alias("interval_type",
                 create_dom_ptr<Json_string>(interval_names[m_interval_type]));
  obj->add_alias("subtract", create_dom_ptr<Json_boolean>(m_subtract));
}

void Item_extract::print(const THD *thd, String *str,
                         enum_query_type query_type) const {
  str->append(STRING_WITH_LEN("extract("));
  str->append(interval_names[int_type]);
  str->append(STRING_WITH_LEN(" from "));
  args[0]->print(thd, str, query_type);
  str->append(')');
}

bool Item_extract::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  set_nullable(true);  // If wrong date
  switch (int_type) {
    case INTERVAL_YEAR:
      max_length = 5;  // YYYY + sign
      date_value = true;
      break;
    case INTERVAL_YEAR_MONTH:
      max_length = 7;  // YYYYMM + sign
      date_value = true;
      break;
    case INTERVAL_QUARTER:
      max_length = 2;
      date_value = true;
      break;
    case INTERVAL_MONTH:
      max_length = 3;  // MM + sign
      date_value = true;
      break;
    case INTERVAL_WEEK:
      max_length = 3;  // WW + sign
      date_value = true;
      break;
    case INTERVAL_DAY:
      max_length = 3;  // DD + sign
      date_value = true;
      break;
    case INTERVAL_DAY_HOUR:
      max_length = 9;
      date_value = false;
      break;
    case INTERVAL_DAY_MINUTE:
      max_length = 11;
      date_value = false;
      break;
    case INTERVAL_DAY_SECOND:
      max_length = 13;
      date_value = false;
      break;
    case INTERVAL_HOUR:
      max_length = 4;  // HHH + sign
      date_value = false;
      break;
    case INTERVAL_HOUR_MINUTE:
      max_length = 6;  // HHHMM + sign
      date_value = false;
      break;
    case INTERVAL_HOUR_SECOND:
      max_length = 8;  // HHHMMSS + sign
      date_value = false;
      break;
    case INTERVAL_MINUTE:
      max_length = 3;  // MM + sign
      date_value = false;
      break;
    case INTERVAL_MINUTE_SECOND:
      max_length = 5;  // MMSS + sign
      date_value = false;
      break;
    case INTERVAL_SECOND:
      max_length = 3;  // SS + sign
      date_value = false;
      break;
    case INTERVAL_MICROSECOND:
      max_length = 7;  // six digits + sign
      date_value = false;
      break;
    case INTERVAL_DAY_MICROSECOND:
      max_length = 20;
      date_value = false;
      break;
    case INTERVAL_HOUR_MICROSECOND:
      max_length = 14;  // HHHMMSSFFFFFF + sign
      date_value = false;
      break;
    case INTERVAL_MINUTE_MICROSECOND:
      max_length = 11;
      date_value = false;
      break;
    case INTERVAL_SECOND_MICROSECOND:
      max_length = 9;
      date_value = false;
      break;
    case INTERVAL_LAST:
      assert(0);
      break; /* purecov: deadcode */
  }
  return false;
}

longlong Item_extract::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  uint year;
  ulong week_format;
  long neg;
  if (date_value) {
    if (get_arg0_date(&ltime, TIME_FUZZY_DATE)) return 0;
    neg = 1;
  } else {
    if (get_arg0_time(&ltime)) return 0;
    neg = ltime.neg ? -1 : 1;
  }
  switch (int_type) {
    case INTERVAL_YEAR:
      return ltime.year;
    case INTERVAL_YEAR_MONTH:
      return ltime.year * 100L + ltime.month;
    case INTERVAL_QUARTER:
      return (ltime.month + 2) / 3;
    case INTERVAL_MONTH:
      return ltime.month;
    case INTERVAL_WEEK: {
      week_format = current_thd->variables.default_week_format;
      return calc_week(ltime, week_mode(week_format), &year);
    }
    case INTERVAL_DAY:
      return ltime.day;
    case INTERVAL_DAY_HOUR:
      return (long)(ltime.day * 100L + ltime.hour) * neg;
    case INTERVAL_DAY_MINUTE:
      return (long)(ltime.day * 10000L + ltime.hour * 100L + ltime.minute) *
             neg;
    case INTERVAL_DAY_SECOND:
      return ((longlong)ltime.day * 1000000L +
              (longlong)(ltime.hour * 10000L + ltime.minute * 100 +
                         ltime.second)) *
             neg;
    case INTERVAL_HOUR:
      return (long)ltime.hour * neg;
    case INTERVAL_HOUR_MINUTE:
      return (long)(ltime.hour * 100 + ltime.minute) * neg;
    case INTERVAL_HOUR_SECOND:
      return (long)(ltime.hour * 10000 + ltime.minute * 100 + ltime.second) *
             neg;
    case INTERVAL_MINUTE:
      return (long)ltime.minute * neg;
    case INTERVAL_MINUTE_SECOND:
      return (long)(ltime.minute * 100 + ltime.second) * neg;
    case INTERVAL_SECOND:
      return (long)ltime.second * neg;
    case INTERVAL_MICROSECOND:
      return (long)ltime.second_part * neg;
    case INTERVAL_DAY_MICROSECOND:
      return (((longlong)ltime.day * 1000000L + (longlong)ltime.hour * 10000L +
               ltime.minute * 100 + ltime.second) *
                  1000000L +
              ltime.second_part) *
             neg;
    case INTERVAL_HOUR_MICROSECOND:
      return (((longlong)ltime.hour * 10000L + ltime.minute * 100 +
               ltime.second) *
                  1000000L +
              ltime.second_part) *
             neg;
    case INTERVAL_MINUTE_MICROSECOND:
      return (((longlong)(ltime.minute * 100 + ltime.second)) * 1000000L +
              ltime.second_part) *
             neg;
    case INTERVAL_SECOND_MICROSECOND:
      return ((longlong)ltime.second * 1000000L + ltime.second_part) * neg;
    case INTERVAL_LAST:
      assert(0);
      break; /* purecov: deadcode */
  }
  return 0;  // Impossible
}

bool Item_extract::eq_specific(const Item *item) const {
  const Item_extract *ie = down_cast<const Item_extract *>(item);
  if (ie->int_type != int_type) return false;
  return true;
}

void Item_typecast_datetime::print(const THD *thd, String *str,
                                   enum_query_type query_type) const {
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(thd, str, query_type);
  str->append(STRING_WITH_LEN(" as "));
  str->append(cast_type());
  if (decimals) str->append_parenthesized(decimals);
  str->append(')');
}

bool Item_typecast_datetime::get_date(MYSQL_TIME *ltime,
                                      my_time_flags_t fuzzy_date) {
  THD *const thd = current_thd;

  my_time_flags_t flags = fuzzy_date | TIME_NO_DATE_FRAC_WARN;
  if (thd->variables.sql_mode & MODE_NO_ZERO_DATE) flags |= TIME_NO_ZERO_DATE;
  if (thd->variables.sql_mode & MODE_NO_ZERO_IN_DATE)
    flags |= TIME_NO_ZERO_IN_DATE;
  if (thd->variables.sql_mode & MODE_INVALID_DATES) flags |= TIME_INVALID_DATES;
  if (thd->is_fsp_truncate_mode()) flags |= TIME_FRAC_TRUNCATE;

  if (get_arg0_date(ltime, flags)) {
    ltime->time_type = MYSQL_TIMESTAMP_DATETIME;
    if (args[0]->null_value || m_explicit_cast) return true;
    // The implicit CAST to DATETIME returns 0-date on invalid argument
    null_value = false;
    set_zero_time(ltime, ltime->time_type);
    return false;
  }
  assert(ltime->time_type != MYSQL_TIMESTAMP_TIME);
  ltime->time_type = MYSQL_TIMESTAMP_DATETIME;  // In case it was DATE
  int warnings = 0;
  return (null_value = propagate_datetime_overflow(
              thd, &warnings,
              my_datetime_adjust_frac(ltime, decimals, &warnings,
                                      thd->is_fsp_truncate_mode())));
}

void Item_typecast_time::print(const THD *thd, String *str,
                               enum_query_type query_type) const {
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(thd, str, query_type);
  str->append(STRING_WITH_LEN(" as "));
  str->append(cast_type());
  if (decimals) str->append_parenthesized(decimals);
  str->append(')');
}

bool Item_typecast_time::get_time(MYSQL_TIME *ltime) {
  if (get_arg0_time(ltime)) return true;
  my_time_adjust_frac(ltime, decimals, current_thd->is_fsp_truncate_mode());

  /*
    For MYSQL_TIMESTAMP_TIME value we can have non-zero day part,
    which we should not lose.
  */
  if (ltime->time_type != MYSQL_TIMESTAMP_TIME) datetime_to_time(ltime);
  return false;
}

void Item_typecast_date::print(const THD *thd, String *str,
                               enum_query_type query_type) const {
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(thd, str, query_type);
  str->append(STRING_WITH_LEN(" as "));
  str->append(cast_type());
  str->append(')');
}

bool Item_typecast_date::get_date(MYSQL_TIME *ltime,
                                  my_time_flags_t fuzzy_date) {
  THD *const thd = current_thd;

  my_time_flags_t flags = fuzzy_date | TIME_NO_DATE_FRAC_WARN;
  if (thd->variables.sql_mode & MODE_NO_ZERO_DATE) flags |= TIME_NO_ZERO_DATE;
  if (thd->variables.sql_mode & MODE_NO_ZERO_IN_DATE)
    flags |= TIME_NO_ZERO_IN_DATE;
  if (thd->variables.sql_mode & MODE_INVALID_DATES) flags |= TIME_INVALID_DATES;

  if (get_arg0_date(ltime, flags)) {
    if (args[0]->null_value || m_explicit_cast) return true;
    // The implicit cast to DATE returns 0-date instead of NULL
    null_value = false;
    set_zero_time(ltime, ltime->time_type);
    return false;
  }

  ltime->hour = 0;
  ltime->minute = 0;
  ltime->second = 0;
  ltime->second_part = 0;
  ltime->time_type = MYSQL_TIMESTAMP_DATE;

  return false;
}

/**
  MAKEDATE(a,b) is a date function that creates a date value
  from a year and day value.

  NOTES:
    As arguments are integers, we can't know if the year is a 2 digit or 4 digit
  year. In this case we treat all years < 100 as 2 digit years. Ie, this is not
  safe for dates between 0000-01-01 and 0099-12-31
*/

bool Item_func_makedate::get_date(MYSQL_TIME *ltime, my_time_flags_t) {
  assert(fixed);
  const longlong daynr = args[1]->val_int();
  if ((null_value = args[1]->null_value)) return true;
  longlong year = args[0]->val_int();
  if ((null_value = args[0]->null_value)) return true;

  if (year < 0 || year > 9999 || daynr <= 0 || daynr > MAX_DAY_NUMBER) {
    null_value = true;
    return true;
  }

  if (year < 100) year = year_2000_handling(year);

  const long days = calc_daynr(year, 1, 1) + daynr - 1;
  /* Day number from year 0 to 9999-12-31 */
  if (days >= 0 && days <= MAX_DAY_NUMBER) {
    null_value = false;
    get_date_from_daynr(days, &ltime->year, &ltime->month, &ltime->day);
    ltime->neg = false;
    ltime->hour = ltime->minute = ltime->second = ltime->second_part = 0;
    ltime->time_type = MYSQL_TIMESTAMP_DATE;
    return false;
  }

  null_value = true;
  return true;
}

bool Item_func_add_time::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 1,
                            m_datetime ? MYSQL_TYPE_DATETIME : MYSQL_TYPE_TIME))
    return true;
  if (param_type_is_default(thd, 1, 2, MYSQL_TYPE_TIME)) return true;
  if (reject_vector_args()) return true;

  /*
    The field type for the result of an Item_func_add_time function is defined
    as follows:

    - If first argument is MYSQL_TYPE_DATETIME, MYSQL_TYPE_TIMESTAMP or
      MYSQL_TYPE_DATE, then result is MYSQL_TYPE_DATETIME
    - If first argument is MYSQL_TYPE_TIME, then result is MYSQL_TYPE_TIME
    - Result type is overridden as MYSQL_TYPE_DATETIME if m_datetime,
      meaning that this is the implementation of the two-argument
      TIMESTAMP function.
    - Otherwise the result is MYSQL_TYPE_STRING
  */
  if (args[0]->data_type() == MYSQL_TYPE_TIME && !m_datetime) {
    const uint8 dec = max(args[0]->time_precision(), args[1]->time_precision());
    set_data_type_time(dec);
  } else if (args[0]->data_type() == MYSQL_TYPE_DATETIME ||
             args[0]->data_type() == MYSQL_TYPE_TIMESTAMP ||
             args[0]->data_type() == MYSQL_TYPE_DATE || m_datetime) {
    const uint8 dec =
        max(args[0]->datetime_precision(), args[1]->time_precision());
    set_data_type_datetime(dec);
  } else {
    set_data_type_char(MAX_DATETIME_FULL_WIDTH, default_charset());
  }
  set_nullable(true);
  return false;
}

/**
  ADDTIME(t,a) and SUBTIME(t,a) are time functions that calculate a
  time/datetime value

  @param time       time or datetime_expression.
  @param fuzzy_date flags that control temporal operation.

  @returns false on success, true on error or NULL value return.
*/

bool Item_func_add_time::val_datetime(MYSQL_TIME *time,
                                      my_time_flags_t fuzzy_date) {
  assert(fixed);
  MYSQL_TIME l_time1, l_time2;
  bool is_time = false;
  long days, microseconds;
  longlong seconds;
  int l_sign = m_sign;

  null_value = false;
  if (data_type() == MYSQL_TYPE_DATETIME)  // TIMESTAMP function
  {
    if (get_arg0_date(&l_time1, fuzzy_date) || args[1]->get_time(&l_time2) ||
        l_time1.time_type == MYSQL_TIMESTAMP_TIME ||
        l_time2.time_type != MYSQL_TIMESTAMP_TIME)
      goto null_date;
  } else  // ADDTIME function
  {
    if (args[0]->get_time(&l_time1) || args[1]->get_time(&l_time2) ||
        l_time2.time_type == MYSQL_TIMESTAMP_DATETIME ||
        l_time2.time_type == MYSQL_TIMESTAMP_DATETIME_TZ)
      goto null_date;
    is_time = (l_time1.time_type == MYSQL_TIMESTAMP_TIME);
  }
  if (l_time1.neg != l_time2.neg) l_sign = -l_sign;

  memset(time, 0, sizeof(MYSQL_TIME));

  time->neg =
      calc_time_diff(l_time1, l_time2, -l_sign, &seconds, &microseconds);

  /*
    If first argument was negative and diff between arguments
    is non-zero we need to swap sign to get proper result.
  */
  if (l_time1.neg && (seconds || microseconds))
    time->neg = 1 - time->neg;  // Swap sign of result

  if (!is_time && time->neg) goto null_date;

  days = (long)(seconds / SECONDS_IN_24H);

  calc_time_from_sec(time, seconds % SECONDS_IN_24H, microseconds);

  if (!is_time) {
    get_date_from_daynr(days, &time->year, &time->month, &time->day);
    time->time_type = MYSQL_TIMESTAMP_DATETIME;

    if (check_datetime_range(*time)) {
      // Value is out of range, cannot use our printing functions to output it.
      push_warning_printf(
          current_thd, Sql_condition::SL_WARNING, ER_DATETIME_FUNCTION_OVERFLOW,
          ER_THD(current_thd, ER_DATETIME_FUNCTION_OVERFLOW), func_name());
      goto null_date;
    }

    if (time->day) return false;
    goto null_date;
  }
  time->time_type = MYSQL_TIMESTAMP_TIME;
  time->hour += days * 24;
  if (adjust_time_range_with_warn(time, 0)) goto null_date;
  return false;

null_date:
  null_value = true;
  return true;
}

void Item_func_add_time::print(const THD *thd, String *str,
                               enum_query_type query_type) const {
  if (m_datetime) {
    assert(m_sign > 0);
    str->append(STRING_WITH_LEN("timestamp("));
  } else {
    if (m_sign > 0)
      str->append(STRING_WITH_LEN("addtime("));
    else
      str->append(STRING_WITH_LEN("subtime("));
  }
  args[0]->print(thd, str, query_type);
  str->append(',');
  args[1]->print(thd, str, query_type);
  str->append(')');
}

/**
  TIMEDIFF(t,s) is a time function that calculates the
  time value between a start and end time.

  t and s: time_or_datetime_expression
  @param[out]  l_time3   Result is stored here.

  @retval   false  On success
  @retval   true   On error
*/

bool Item_func_timediff::get_time(MYSQL_TIME *l_time3) {
  assert(fixed);
  longlong seconds;
  long microseconds;
  int l_sign = 1;
  MYSQL_TIME l_time1, l_time2;

  null_value = false;

  if ((args[0]->is_temporal_with_date() &&
       args[1]->data_type() == MYSQL_TYPE_TIME) ||
      (args[1]->is_temporal_with_date() &&
       args[0]->data_type() == MYSQL_TYPE_TIME))
    goto null_date;  // Incompatible types

  if (args[0]->is_temporal_with_date() || args[1]->is_temporal_with_date()) {
    if (args[0]->get_date(&l_time1, TIME_FUZZY_DATE) ||
        args[1]->get_date(&l_time2, TIME_FUZZY_DATE))
      goto null_date;
  } else {
    if (args[0]->get_time(&l_time1) || args[1]->get_time(&l_time2))
      goto null_date;
  }

  if (l_time1.time_type != l_time2.time_type)
    goto null_date;  // Incompatible types

  if (l_time1.neg != l_time2.neg) l_sign = -l_sign;

  memset(l_time3, 0, sizeof(*l_time3));

  l_time3->neg =
      calc_time_diff(l_time1, l_time2, l_sign, &seconds, &microseconds);

  /*
    For MYSQL_TIMESTAMP_TIME only:
      If first argument was negative and diff between arguments
      is non-zero we need to swap sign to get proper result.
  */
  if (l_time1.neg && (seconds || microseconds))
    l_time3->neg = 1 - l_time3->neg;  // Swap sign of result

  calc_time_from_sec(l_time3, seconds, microseconds);
  if (adjust_time_range_with_warn(l_time3, decimals)) goto null_date;
  return false;

null_date:
  return (null_value = true);
}

/**
  MAKETIME(h,m,s) is a time function that calculates a time value
  from the total number of hours, minutes, and seconds.
  Result: Time value
*/

bool Item_func_maketime::get_time(MYSQL_TIME *ltime) {
  assert(fixed);
  bool overflow = false;
  const longlong hour = args[0]->val_int();
  if ((null_value = args[0]->null_value)) return true;

  const longlong minute = args[1]->val_int();
  if ((null_value = args[1]->null_value)) return true;

  my_decimal tmp, *sec = args[2]->val_decimal(&tmp);
  if (sec == nullptr) return (null_value = true);
  if ((null_value = args[2]->null_value)) return true;

  lldiv_t second;
  if ((null_value = (my_decimal2lldiv_t(E_DEC_FATAL_ERROR, sec, &second) ||
                     minute < 0 || minute > 59 || second.quot < 0 ||
                     second.quot > 59 || second.rem < 0)))
    return true;

  set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);

  /* Check for integer overflows */
  if (hour < 0) {
    if (args[0]->unsigned_flag)
      overflow = true;
    else
      ltime->neg = true;
  }
  if (-hour > UINT_MAX || hour > UINT_MAX) overflow = true;

  if (!overflow) {
    ltime->hour = (uint)((hour < 0 ? -hour : hour));
    ltime->minute = (uint)minute;
    ltime->second = (uint)second.quot;
    int warnings = 0;
    ltime->second_part = static_cast<ulong>(second.rem / 1000);
    if (adjust_time_range_with_warn(ltime, decimals)) return true;
    time_add_nanoseconds_adjust_frac(ltime, second.rem % 1000, &warnings,
                                     current_thd->is_fsp_truncate_mode());

    if (!warnings) return false;
  }

  // Return maximum value (positive or negative)
  set_max_hhmmss(ltime);
  char
      buf[MAX_BIGINT_WIDTH /* hh */ + 6 /* :mm:ss */ + 10 /* .fffffffff */ + 1];
  char *ptr = longlong10_to_str(hour, buf, args[0]->unsigned_flag ? 10 : -10);
  int len = (int)(ptr - buf) +
            sprintf(ptr, ":%02u:%02u", (uint)minute, (uint)second.quot);
  if (second.rem) {
    /*
      Display fractional part up to nanoseconds (9 digits),
      which is the maximum precision of my_decimal2lldiv_t().
    */
    const int dec = min(args[2]->decimals, uint8{9});
    len += sprintf(buf + len, ".%0*lld", dec,
                   second.rem / (ulong)log_10_int[9 - dec]);
  }
  assert(strlen(buf) < sizeof(buf));
  return make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                      ErrConvString(buf, len),
                                      MYSQL_TIMESTAMP_TIME, NullS);
}

/**
  MICROSECOND(a) is a function ( extraction) that extracts the microseconds
  from a.

  a: Datetime or time value
  Result: int value
*/

longlong Item_func_microsecond::val_int() {
  assert(fixed);
  MYSQL_TIME ltime;
  return get_arg0_time(&ltime) ? 0 : ltime.second_part;
}

bool Item_func_microsecond::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_DATETIME)) return true;
  if (reject_vector_args()) return true;
  set_nullable(true);
  return false;
}

longlong Item_func_timestamp_diff::val_int() {
  MYSQL_TIME ltime1, ltime2;
  longlong seconds;
  long microseconds;
  long months = 0;
  int neg = 1;

  null_value = false;
  if (args[0]->get_date(&ltime1, TIME_NO_ZERO_DATE) ||
      args[1]->get_date(&ltime2, TIME_NO_ZERO_DATE))
    goto null_date;

  if (calc_time_diff(ltime2, ltime1, 1, &seconds, &microseconds)) neg = -1;

  if (int_type == INTERVAL_YEAR || int_type == INTERVAL_QUARTER ||
      int_type == INTERVAL_MONTH) {
    uint year_beg, year_end, month_beg, month_end, day_beg, day_end;
    uint years = 0;
    uint second_beg, second_end, microsecond_beg, microsecond_end;

    if (neg == -1) {
      year_beg = ltime2.year;
      year_end = ltime1.year;
      month_beg = ltime2.month;
      month_end = ltime1.month;
      day_beg = ltime2.day;
      day_end = ltime1.day;
      second_beg = ltime2.hour * 3600 + ltime2.minute * 60 + ltime2.second;
      second_end = ltime1.hour * 3600 + ltime1.minute * 60 + ltime1.second;
      microsecond_beg = ltime2.second_part;
      microsecond_end = ltime1.second_part;
    } else {
      year_beg = ltime1.year;
      year_end = ltime2.year;
      month_beg = ltime1.month;
      month_end = ltime2.month;
      day_beg = ltime1.day;
      day_end = ltime2.day;
      second_beg = ltime1.hour * 3600 + ltime1.minute * 60 + ltime1.second;
      second_end = ltime2.hour * 3600 + ltime2.minute * 60 + ltime2.second;
      microsecond_beg = ltime1.second_part;
      microsecond_end = ltime2.second_part;
    }

    /* calc years */
    years = year_end - year_beg;
    if (month_end < month_beg || (month_end == month_beg && day_end < day_beg))
      years -= 1;

    /* calc months */
    months = 12 * years;
    if (month_end < month_beg || (month_end == month_beg && day_end < day_beg))
      months += 12 - (month_beg - month_end);
    else
      months += (month_end - month_beg);

    if (day_end < day_beg)
      months -= 1;
    else if ((day_end == day_beg) &&
             ((second_end < second_beg) ||
              (second_end == second_beg && microsecond_end < microsecond_beg)))
      months -= 1;
  }

  switch (int_type) {
    case INTERVAL_YEAR:
      return months / 12 * neg;
    case INTERVAL_QUARTER:
      return months / 3 * neg;
    case INTERVAL_MONTH:
      return months * neg;
    case INTERVAL_WEEK:
      return seconds / SECONDS_IN_24H / 7L * neg;
    case INTERVAL_DAY:
      return seconds / SECONDS_IN_24H * neg;
    case INTERVAL_HOUR:
      return seconds / 3600L * neg;
    case INTERVAL_MINUTE:
      return seconds / 60L * neg;
    case INTERVAL_SECOND:
      return seconds * neg;
    case INTERVAL_MICROSECOND:
      /*
        In MySQL difference between any two valid datetime values
        in microseconds fits into longlong.
      */
      return (seconds * 1000000L + microseconds) * neg;
    default:
      break;
  }

null_date:
  null_value = true;
  return 0;
}

void Item_func_timestamp_diff::print(const THD *thd, String *str,
                                     enum_query_type query_type) const {
  str->append(func_name());
  str->append('(');

  switch (int_type) {
    case INTERVAL_YEAR:
      str->append(STRING_WITH_LEN("YEAR"));
      break;
    case INTERVAL_QUARTER:
      str->append(STRING_WITH_LEN("QUARTER"));
      break;
    case INTERVAL_MONTH:
      str->append(STRING_WITH_LEN("MONTH"));
      break;
    case INTERVAL_WEEK:
      str->append(STRING_WITH_LEN("WEEK"));
      break;
    case INTERVAL_DAY:
      str->append(STRING_WITH_LEN("DAY"));
      break;
    case INTERVAL_HOUR:
      str->append(STRING_WITH_LEN("HOUR"));
      break;
    case INTERVAL_MINUTE:
      str->append(STRING_WITH_LEN("MINUTE"));
      break;
    case INTERVAL_SECOND:
      str->append(STRING_WITH_LEN("SECOND"));
      break;
    case INTERVAL_MICROSECOND:
      str->append(STRING_WITH_LEN("MICROSECOND"));
      break;
    default:
      break;
  }

  for (uint i = 0; i < 2; i++) {
    str->append(',');
    args[i]->print(thd, str, query_type);
  }
  str->append(')');
}

String *Item_func_get_format::val_str_ascii(String *str) {
  assert(fixed);
  const char *format_name;
  const Known_date_time_format *format;
  String *val = args[0]->val_str_ascii(str);
  size_t val_len;

  if ((null_value = args[0]->null_value)) return nullptr;

  val_len = val->length();
  for (format = &known_date_time_formats[0];
       (format_name = format->format_name); format++) {
    size_t format_name_len;
    format_name_len = strlen(format_name);
    if (val_len == format_name_len &&
        !my_strnncoll(&my_charset_latin1, (const uchar *)val->ptr(), val_len,
                      (const uchar *)format_name, val_len)) {
      const char *format_str = get_date_time_format_str(format, type);
      str->set(format_str, strlen(format_str), &my_charset_numeric);
      return str;
    }
  }

  null_value = true;
  return nullptr;
}

void Item_func_get_format::print(const THD *thd, String *str,
                                 enum_query_type query_type) const {
  str->append(func_name());
  str->append('(');

  switch (type) {
    case MYSQL_TIMESTAMP_DATE:
      str->append(STRING_WITH_LEN("DATE, "));
      break;
    case MYSQL_TIMESTAMP_DATETIME:
      str->append(STRING_WITH_LEN("DATETIME, "));
      break;
    case MYSQL_TIMESTAMP_TIME:
      str->append(STRING_WITH_LEN("TIME, "));
      break;
    default:
      assert(0);
  }
  args[0]->print(thd, str, query_type);
  str->append(')');
}

/**
  Set type of datetime value (DATE/TIME/...) which will be produced
  according to format string.

  @param format   format string
  @param length   length of format string

  @note
    We don't process day format's characters('D', 'd', 'e') because day
    may be a member of all date/time types.

  @note
    Format specifiers supported by this function should be in sync with
    specifiers supported by extract_date_time() function.
*/
void Item_func_str_to_date::fix_from_format(const char *format, size_t length) {
  const char *time_part_frms = "HISThiklrs";
  const char *date_part_frms = "MVUXYWabcjmvuxyw";
  bool date_part_used = false, time_part_used = false, frac_second_used = false;
  const char *val = format;
  const char *end = format + length;

  for (; val != end; val++) {
    if (*val == '%' && val + 1 != end) {
      val++;
      if (*val == 'f')
        frac_second_used = time_part_used = true;
      else if (!time_part_used && strchr(time_part_frms, *val))
        time_part_used = true;
      else if (!date_part_used && strchr(date_part_frms, *val))
        date_part_used = true;
      if (date_part_used && frac_second_used) {
        /*
          frac_second_used implies time_part_used, and thus we already
          have all types of date-time components and can end our search.
        */
        cached_timestamp_type = MYSQL_TIMESTAMP_DATETIME;
        set_data_type_datetime(DATETIME_MAX_DECIMALS);
        return;
      }
    }
  }

  /* We don't have all three types of date-time components */
  if (frac_second_used) /* TIME with microseconds */
  {
    cached_timestamp_type = MYSQL_TIMESTAMP_TIME;
    set_data_type_time(DATETIME_MAX_DECIMALS);
  } else if (time_part_used) {
    if (date_part_used) /* DATETIME, no microseconds */
    {
      cached_timestamp_type = MYSQL_TIMESTAMP_DATETIME;
      set_data_type_datetime(0);
    } else /* TIME, no microseconds */
    {
      cached_timestamp_type = MYSQL_TIMESTAMP_TIME;
      set_data_type_time(0);
    }
  } else /* DATE */
  {
    cached_timestamp_type = MYSQL_TIMESTAMP_DATE;
    set_data_type_date();
  }
}

bool Item_func_str_to_date::resolve_type(THD *thd) {
  if (param_type_is_default(thd, 0, 2)) return true;
  if (reject_vector_args()) return true;
  set_nullable(true);
  cached_timestamp_type = MYSQL_TIMESTAMP_DATETIME;
  set_data_type_datetime(DATETIME_MAX_DECIMALS);
  sql_mode = thd->variables.sql_mode &
             (MODE_NO_ZERO_DATE | MODE_NO_ZERO_IN_DATE | MODE_INVALID_DATES);
  if (args[1]->const_item() && args[1]->may_eval_const_item(thd)) {
    char format_buff[64];
    String format_str(format_buff, sizeof(format_buff), &my_charset_bin);
    String *format = args[1]->val_str(&format_str);
    if (!args[1]->null_value) fix_from_format(format->ptr(), format->length());
  }
  return false;
}

/**
  Determines whether this date should be NULL (and a warning raised) under the
  given sql_mode. Zeroes are allowed in the date if the data type is TIME.

  @param target_type The data type of the time/date.
  @param time Date and time data
  @param fuzzy_date What sql_mode dictates.
  @return Whether the result is valid or NULL.
*/
static bool date_should_be_null(enum_field_types target_type,
                                const MYSQL_TIME &time,
                                my_time_flags_t fuzzy_date) {
  return (fuzzy_date & TIME_NO_ZERO_DATE) != 0 &&
         (target_type != MYSQL_TYPE_TIME) &&
         (time.year == 0 || time.month == 0 || time.day == 0);
}

bool Item_func_str_to_date::val_datetime(MYSQL_TIME *ltime,
                                         my_time_flags_t fuzzy_date) {
  Date_time_format date_time_format;
  char val_buff[64], format_buff[64];
  String val_string(val_buff, sizeof(val_buff), &my_charset_bin), *val;
  String format_str(format_buff, sizeof(format_buff), &my_charset_bin), *format;

  if (sql_mode & MODE_NO_ZERO_IN_DATE) fuzzy_date |= TIME_NO_ZERO_IN_DATE;
  if (sql_mode & MODE_NO_ZERO_DATE) fuzzy_date |= TIME_NO_ZERO_DATE;
  if (sql_mode & MODE_INVALID_DATES) fuzzy_date |= TIME_INVALID_DATES;

  val = args[0]->val_str(&val_string);
  format = args[1]->val_str(&format_str);
  if (args[0]->null_value || args[1]->null_value) goto null_date;

  null_value = false;
  memset(ltime, 0, sizeof(*ltime));
  date_time_format.format.str = format->ptr();
  date_time_format.format.length = format->length();
  if (extract_date_time(&date_time_format, val->ptr(), val->length(), ltime,
                        cached_timestamp_type, nullptr, "datetime", data_type(),
                        fuzzy_date))
    goto null_date;
  if (date_should_be_null(data_type(), *ltime, fuzzy_date)) {
    char buff[128];
    strmake(buff, val->ptr(), min<size_t>(val->length(), sizeof(buff) - 1));
    push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                        ER_WRONG_VALUE_FOR_TYPE,
                        ER_THD(current_thd, ER_WRONG_VALUE_FOR_TYPE),
                        "datetime", buff, "str_to_date");
    goto null_date;
  }
  ltime->time_type = cached_timestamp_type;
  if (cached_timestamp_type == MYSQL_TIMESTAMP_TIME && ltime->day) {
    /*
      Day part for time type can be nonzero value and so
      we should add hours from day part to hour part to
      keep valid time value.
    */
    ltime->hour += ltime->day * 24;
    ltime->day = 0;
  }
  return false;

null_date:
  null_value = true;

  return true;
}

bool Item_func_last_day::get_date(MYSQL_TIME *ltime,
                                  my_time_flags_t fuzzy_date) {
  if ((null_value = get_arg0_date(ltime, fuzzy_date))) return true;

  if (ltime->month == 0) {
    /*
      Cannot calculate last day for zero month.
      Let's print a warning and return NULL.
    */
    ltime->time_type = MYSQL_TIMESTAMP_DATE;
    const ErrConvString str(ltime, 0);
    if (make_truncated_value_warning(current_thd, Sql_condition::SL_WARNING,
                                     str, MYSQL_TIMESTAMP_ERROR, NullS))
      return true;
    return (null_value = true);
  }

  const uint month_idx = ltime->month - 1;
  ltime->day = days_in_month[month_idx];
  if (month_idx == 1 && calc_days_in_year(ltime->year) == 366) ltime->day = 29;
  datetime_to_date(ltime);
  return false;
}

bool Item_func_internal_update_time::resolve_type(THD *thd) {
  set_data_type_datetime(0);
  set_nullable(true);
  null_on_null = false;
  thd->time_zone_used = true;
  return false;
}

bool Item_func_internal_update_time::get_date(MYSQL_TIME *ltime,
                                              my_time_flags_t) {
  DBUG_TRACE;

  String schema_name;
  String *schema_name_ptr;
  String table_name;
  String *table_name_ptr = nullptr;
  String engine_name;
  String *engine_name_ptr = nullptr;
  String partition_name;
  String *partition_name_ptr = nullptr;
  const bool skip_hidden_table = args[4]->val_int();
  String ts_se_private_data;
  String *ts_se_private_data_ptr = args[5]->val_str(&ts_se_private_data);
  const ulonglong stat_data = args[6]->val_uint();
  const ulonglong cached_timestamp = args[7]->val_uint();
  ulonglong unixtime = 0;

  if ((schema_name_ptr = args[0]->val_str(&schema_name)) != nullptr &&
      (table_name_ptr = args[1]->val_str(&table_name)) != nullptr &&
      (engine_name_ptr = args[2]->val_str(&engine_name)) != nullptr &&
      !is_infoschema_db(schema_name_ptr->c_ptr_safe()) && !skip_hidden_table) {
    const dd::Object_id se_private_id = (dd::Object_id)args[3]->val_uint();
    THD *thd = current_thd;

    MYSQL_TIME time;
    bool not_used;
    // Convert longlong time to MYSQL_TIME format
    my_longlong_to_datetime_with_warn(stat_data, &time, MYF(0));

    // Convert MYSQL_TIME to epoc second according to local time_zone as
    // cached_timestamp value is with local time_zone
    my_time_t timestamp;
    timestamp = thd->variables.time_zone->TIME_to_gmt_sec(&time, &not_used);

    // Make sure we have safe string to access.
    schema_name_ptr->c_ptr_safe();
    table_name_ptr->c_ptr_safe();
    engine_name_ptr->c_ptr_safe();

    /*
      The same native function used by I_S.PARTITIONS is used by I_S.TABLES.
      We invoke native function with partition name only with I_S.PARTITIONS
      as a last argument. So, we check for argument count below, before
      reading partition name.
    */
    if (arg_count == 10)
      partition_name_ptr = args[9]->val_str(&partition_name);
    else if (arg_count == 9)
      partition_name_ptr = args[8]->val_str(&partition_name);

    unixtime = thd->lex->m_IS_table_stats.read_stat(
        thd, *schema_name_ptr, *table_name_ptr, *engine_name_ptr,
        (partition_name_ptr ? partition_name_ptr->c_ptr_safe() : nullptr),
        se_private_id,
        (ts_se_private_data_ptr ? ts_se_private_data_ptr->c_ptr_safe()
                                : nullptr),
        nullptr, static_cast<ulonglong>(timestamp), cached_timestamp,
        dd::info_schema::enum_table_stats_type::TABLE_UPDATE_TIME);
    if (unixtime) {
      null_value = false;
      thd->variables.time_zone->gmt_sec_to_TIME(ltime, (my_time_t)unixtime);
      return false;
    }
  }

  null_value = true;
  return true;
}

bool Item_func_internal_check_time::resolve_type(THD *thd) {
  set_data_type_datetime(0);
  set_nullable(true);
  null_on_null = false;
  thd->time_zone_used = true;
  return false;
}

bool Item_func_internal_check_time::get_date(MYSQL_TIME *ltime,
                                             my_time_flags_t) {
  DBUG_TRACE;

  String schema_name;
  String *schema_name_ptr;
  String table_name;
  String *table_name_ptr = nullptr;
  String engine_name;
  String *engine_name_ptr = nullptr;
  String partition_name;
  String *partition_name_ptr = nullptr;
  const bool skip_hidden_table = args[4]->val_int();
  String ts_se_private_data;
  String *ts_se_private_data_ptr = args[5]->val_str(&ts_se_private_data);
  const ulonglong stat_data = args[6]->val_uint();
  const ulonglong cached_timestamp = args[7]->val_uint();
  ulonglong unixtime = 0;

  if ((schema_name_ptr = args[0]->val_str(&schema_name)) != nullptr &&
      (table_name_ptr = args[1]->val_str(&table_name)) != nullptr &&
      (engine_name_ptr = args[2]->val_str(&engine_name)) != nullptr &&
      !is_infoschema_db(schema_name_ptr->c_ptr_safe()) && !skip_hidden_table) {
    const dd::Object_id se_private_id = (dd::Object_id)args[3]->val_uint();
    THD *thd = current_thd;

    MYSQL_TIME time;
    bool not_used = true;
    // Convert longlong time to MYSQL_TIME format
    if (my_longlong_to_datetime_with_warn(stat_data, &time, MYF(0))) {
      null_value = true;
      return true;
    }

    // Convert MYSQL_TIME to epoc second according to local time_zone as
    // cached_timestamp value is with local time_zone
    const my_time_t timestamp =
        thd->variables.time_zone->TIME_to_gmt_sec(&time, &not_used);
    // Make sure we have safe string to access.
    schema_name_ptr->c_ptr_safe();
    table_name_ptr->c_ptr_safe();
    engine_name_ptr->c_ptr_safe();

    /*
      The same native function used by I_S.PARTITIONS is used by I_S.TABLES.
      We invoke native function with partition name only with I_S.PARTITIONS
      as a last argument. So, we check for argument count below, before
      reading partition name.
     */
    if (arg_count == 10)
      partition_name_ptr = args[9]->val_str(&partition_name);
    else if (arg_count == 9)
      partition_name_ptr = args[8]->val_str(&partition_name);

    unixtime = thd->lex->m_IS_table_stats.read_stat(
        thd, *schema_name_ptr, *table_name_ptr, *engine_name_ptr,
        (partition_name_ptr ? partition_name_ptr->c_ptr_safe() : nullptr),
        se_private_id,
        (ts_se_private_data_ptr ? ts_se_private_data_ptr->c_ptr_safe()
                                : nullptr),
        nullptr, static_cast<ulonglong>(timestamp), cached_timestamp,
        dd::info_schema::enum_table_stats_type::CHECK_TIME);

    if (unixtime) {
      null_value = false;
      thd->variables.time_zone->gmt_sec_to_TIME(ltime, (my_time_t)unixtime);
      return false;
    }
  }

  null_value = true;
  return true;
}
