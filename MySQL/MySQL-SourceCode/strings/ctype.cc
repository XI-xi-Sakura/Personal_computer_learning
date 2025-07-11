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

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "my_byteorder.h"
#include "my_sys.h"
#include "my_xml.h"
#include "mysql/my_loglevel.h"
#include "mysql/strings/collations.h"
#include "mysql/strings/m_ctype.h"
#include "mysys_err.h"
#include "sql_chars.h"
#include "string_with_len.h"
#include "strings/collations_internal.h"
#include "strings/m_ctype_internals.h"
#include "template_utils.h"

/*

  This files implements routines which parse XML based
  character set and collation description files.

  Unicode collations are encoded according to

    Unicode Technical Standard #35
    Locale Data Markup Language (LDML)
    http://www.unicode.org/reports/tr35/

  and converted into ICU string according to

    Collation Customization
    http://oss.software.ibm.com/icu/userguide/Collate_Customization.html

*/

extern CHARSET_INFO my_charset_ucs2_unicode_ci;
extern CHARSET_INFO my_charset_utf16_unicode_ci;
extern CHARSET_INFO my_charset_utf8mb4_unicode_ci;

static char *mstr(char *str, const char *src, size_t l1, size_t l2) {
  l1 = l1 < l2 ? l1 : l2;
  memcpy(str, src, l1);
  str[l1] = '\0';
  return str;
}

struct my_cs_file_section_st {
  int state;
  const char *str;
};

#define _CS_MISC 1
#define _CS_ID 2
#define _CS_CSNAME 3
#define _CS_FAMILY 4
#define _CS_ORDER 5
#define _CS_COLNAME 6
#define _CS_FLAG 7
#define _CS_CHARSET 8
#define _CS_COLLATION 9
#define _CS_UPPERMAP 10
#define _CS_LOWERMAP 11
#define _CS_UNIMAP 12
#define _CS_COLLMAP 13
#define _CS_CTYPEMAP 14
#define _CS_PRIMARY_ID 15
#define _CS_BINARY_ID 16
#define _CS_CSDESCRIPT 17

/* Special purpose commands */
#define _CS_UCA_VERSION 100
#define _CS_CL_SUPPRESS_CONTRACTIONS 101
#define _CS_CL_OPTIMIZE 102
#define _CS_CL_SHIFT_AFTER_METHOD 103

/* Collation Settings */
#define _CS_ST_SETTINGS 200
#define _CS_ST_STRENGTH 201
#define _CS_ST_ALTERNATE 202
#define _CS_ST_BACKWARDS 203
#define _CS_ST_NORMALIZATION 204
#define _CS_ST_CASE_LEVEL 205
#define _CS_ST_CASE_FIRST 206
#define _CS_ST_HIRAGANA_QUATERNARY 207
#define _CS_ST_NUMERIC 208
#define _CS_ST_VARIABLE_TOP 209
#define _CS_ST_MATCH_BOUNDARIES 210
#define _CS_ST_MATCH_STYLE 211

/* Rules */
#define _CS_RULES 300
#define _CS_RESET 301
#define _CS_DIFF1 302
#define _CS_DIFF2 303
#define _CS_DIFF3 304
#define _CS_DIFF4 305
#define _CS_IDENTICAL 306

/* Rules: Expansions */
#define _CS_EXP_X 320
#define _CS_EXP_EXTEND 321
#define _CS_EXP_DIFF1 322
#define _CS_EXP_DIFF2 323
#define _CS_EXP_DIFF3 324
#define _CS_EXP_DIFF4 325
#define _CS_EXP_IDENTICAL 326

/* Rules: Abbreviating Ordering Specifications */
#define _CS_A_DIFF1 351
#define _CS_A_DIFF2 352
#define _CS_A_DIFF3 353
#define _CS_A_DIFF4 354
#define _CS_A_IDENTICAL 355

/* Rules: previous context */
#define _CS_CONTEXT 370

/* Rules: Placing Characters Before Others*/
#define _CS_RESET_BEFORE 380

/* Rules: Logical Reset Positions */
#define _CS_RESET_FIRST_PRIMARY_IGNORABLE 401
#define _CS_RESET_LAST_PRIMARY_IGNORABLE 402
#define _CS_RESET_FIRST_SECONDARY_IGNORABLE 403
#define _CS_RESET_LAST_SECONDARY_IGNORABLE 404
#define _CS_RESET_FIRST_TERTIARY_IGNORABLE 405
#define _CS_RESET_LAST_TERTIARY_IGNORABLE 406
#define _CS_RESET_FIRST_TRAILING 407
#define _CS_RESET_LAST_TRAILING 408
#define _CS_RESET_FIRST_VARIABLE 409
#define _CS_RESET_LAST_VARIABLE 410
#define _CS_RESET_FIRST_NON_IGNORABLE 411
#define _CS_RESET_LAST_NON_IGNORABLE 412

static struct my_cs_file_section_st sec[] = {
    {_CS_MISC, "xml"},
    {_CS_MISC, "xml/version"},
    {_CS_MISC, "xml/encoding"},
    {_CS_MISC, "charsets"},
    {_CS_MISC, "charsets/max-id"},
    {_CS_MISC, "charsets/copyright"},
    {_CS_MISC, "charsets/description"},
    {_CS_CHARSET, "charsets/charset"},
    {_CS_PRIMARY_ID, "charsets/charset/primary-id"},
    {_CS_BINARY_ID, "charsets/charset/binary-id"},
    {_CS_CSNAME, "charsets/charset/name"},
    {_CS_FAMILY, "charsets/charset/family"},
    {_CS_CSDESCRIPT, "charsets/charset/description"},
    {_CS_MISC, "charsets/charset/alias"},
    {_CS_MISC, "charsets/charset/ctype"},
    {_CS_CTYPEMAP, "charsets/charset/ctype/map"},
    {_CS_MISC, "charsets/charset/upper"},
    {_CS_UPPERMAP, "charsets/charset/upper/map"},
    {_CS_MISC, "charsets/charset/lower"},
    {_CS_LOWERMAP, "charsets/charset/lower/map"},
    {_CS_MISC, "charsets/charset/unicode"},
    {_CS_UNIMAP, "charsets/charset/unicode/map"},
    {_CS_COLLATION, "charsets/charset/collation"},
    {_CS_COLNAME, "charsets/charset/collation/name"},
    {_CS_ID, "charsets/charset/collation/id"},
    {_CS_ORDER, "charsets/charset/collation/order"},
    {_CS_FLAG, "charsets/charset/collation/flag"},
    {_CS_COLLMAP, "charsets/charset/collation/map"},

    /* Special purpose commands */
    {_CS_UCA_VERSION, "charsets/charset/collation/version"},
    {_CS_CL_SUPPRESS_CONTRACTIONS,
     "charsets/charset/collation/suppress_contractions"},
    {_CS_CL_OPTIMIZE, "charsets/charset/collation/optimize"},
    {_CS_CL_SHIFT_AFTER_METHOD,
     "charsets/charset/collation/shift-after-method"},

    /* Collation Settings */
    {_CS_ST_SETTINGS, "charsets/charset/collation/settings"},
    {_CS_ST_STRENGTH, "charsets/charset/collation/settings/strength"},
    {_CS_ST_ALTERNATE, "charsets/charset/collation/settings/alternate"},
    {_CS_ST_BACKWARDS, "charsets/charset/collation/settings/backwards"},
    {_CS_ST_NORMALIZATION, "charsets/charset/collation/settings/normalization"},
    {_CS_ST_CASE_LEVEL, "charsets/charset/collation/settings/caseLevel"},
    {_CS_ST_CASE_FIRST, "charsets/charset/collation/settings/caseFirst"},
    {_CS_ST_HIRAGANA_QUATERNARY,
     "charsets/charset/collation/settings/hiraganaQuaternary"},
    {_CS_ST_NUMERIC, "charsets/charset/collation/settings/numeric"},
    {_CS_ST_VARIABLE_TOP, "charsets/charset/collation/settings/variableTop"},
    {_CS_ST_MATCH_BOUNDARIES,
     "charsets/charset/collation/settings/match-boundaries"},
    {_CS_ST_MATCH_STYLE, "charsets/charset/collation/settings/match-style"},

    /* Rules */
    {_CS_RULES, "charsets/charset/collation/rules"},
    {_CS_RESET, "charsets/charset/collation/rules/reset"},
    {_CS_DIFF1, "charsets/charset/collation/rules/p"},
    {_CS_DIFF2, "charsets/charset/collation/rules/s"},
    {_CS_DIFF3, "charsets/charset/collation/rules/t"},
    {_CS_DIFF4, "charsets/charset/collation/rules/q"},
    {_CS_IDENTICAL, "charsets/charset/collation/rules/i"},

    /* Rules: expansions */
    {_CS_EXP_X, "charsets/charset/collation/rules/x"},
    {_CS_EXP_EXTEND, "charsets/charset/collation/rules/x/extend"},
    {_CS_EXP_DIFF1, "charsets/charset/collation/rules/x/p"},
    {_CS_EXP_DIFF2, "charsets/charset/collation/rules/x/s"},
    {_CS_EXP_DIFF3, "charsets/charset/collation/rules/x/t"},
    {_CS_EXP_DIFF4, "charsets/charset/collation/rules/x/q"},
    {_CS_EXP_IDENTICAL, "charsets/charset/collation/rules/x/i"},

    /* Rules: previous context */
    {_CS_CONTEXT, "charsets/charset/collation/rules/x/context"},

    /* Rules: Abbreviating Ordering Specifications */
    {_CS_A_DIFF1, "charsets/charset/collation/rules/pc"},
    {_CS_A_DIFF2, "charsets/charset/collation/rules/sc"},
    {_CS_A_DIFF3, "charsets/charset/collation/rules/tc"},
    {_CS_A_DIFF4, "charsets/charset/collation/rules/qc"},
    {_CS_A_IDENTICAL, "charsets/charset/collation/rules/ic"},

    /* Rules: Placing Characters Before Others*/
    {_CS_RESET_BEFORE, "charsets/charset/collation/rules/reset/before"},

    /* Rules: Logical Reset Positions */
    {_CS_RESET_FIRST_NON_IGNORABLE,
     "charsets/charset/collation/rules/reset/first_non_ignorable"},
    {_CS_RESET_LAST_NON_IGNORABLE,
     "charsets/charset/collation/rules/reset/last_non_ignorable"},
    {_CS_RESET_FIRST_PRIMARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/first_primary_ignorable"},
    {_CS_RESET_LAST_PRIMARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/last_primary_ignorable"},
    {_CS_RESET_FIRST_SECONDARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/first_secondary_ignorable"},
    {_CS_RESET_LAST_SECONDARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/last_secondary_ignorable"},
    {_CS_RESET_FIRST_TERTIARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/first_tertiary_ignorable"},
    {_CS_RESET_LAST_TERTIARY_IGNORABLE,
     "charsets/charset/collation/rules/reset/last_tertiary_ignorable"},
    {_CS_RESET_FIRST_TRAILING,
     "charsets/charset/collation/rules/reset/first_trailing"},
    {_CS_RESET_LAST_TRAILING,
     "charsets/charset/collation/rules/reset/last_trailing"},
    {_CS_RESET_FIRST_VARIABLE,
     "charsets/charset/collation/rules/reset/first_variable"},
    {_CS_RESET_LAST_VARIABLE,
     "charsets/charset/collation/rules/reset/last_variable"},

    {0, nullptr}};

static struct my_cs_file_section_st *cs_file_sec(const char *attr, size_t len) {
  struct my_cs_file_section_st *s;
  for (s = sec; s->str; s++) {
    if (!strncmp(attr, s->str, len) && s->str[len] == 0) return s;
  }
  return nullptr;
}

#define MY_CS_CSDESCR_SIZE 64
#define MY_CS_CONTEXT_SIZE 64

typedef struct my_cs_file_info {
  char csname[MY_CS_NAME_SIZE];
  char name[MY_CS_NAME_SIZE];
  uint8_t ctype[MY_CS_CTYPE_TABLE_SIZE];
  uint8_t to_lower[MY_CS_TO_LOWER_TABLE_SIZE];
  uint8_t to_upper[MY_CS_TO_UPPER_TABLE_SIZE];
  uint8_t sort_order[MY_CS_SORT_ORDER_TABLE_SIZE];
  uint16_t tab_to_uni[MY_CS_TO_UNI_TABLE_SIZE];
  char comment[MY_CS_CSDESCR_SIZE];
  char *tailoring;
  size_t tailoring_length;
  size_t tailoring_alloced_length;
  char context[MY_CS_CONTEXT_SIZE];
  CHARSET_INFO cs;
  MY_CHARSET_LOADER *loader;
} MY_CHARSET_FILE;

static void my_charset_file_reset_charset(MY_CHARSET_FILE *i) {
  memset(&i->cs, 0, sizeof(i->cs));
}

static void my_charset_file_reset_collation(MY_CHARSET_FILE *i) {
  i->tailoring_length = 0;
  i->context[0] = '\0';
}

static void my_charset_file_init(MY_CHARSET_FILE *i) {
  my_charset_file_reset_charset(i);
  my_charset_file_reset_collation(i);
  i->tailoring = nullptr;
  i->tailoring_alloced_length = 0;
}

static void my_charset_file_free(MY_CHARSET_FILE *i) { free(i->tailoring); }

static int my_charset_file_tailoring_realloc(MY_CHARSET_FILE *i,
                                             size_t newlen) {
  if (i->tailoring_alloced_length > newlen ||
      (i->tailoring = static_cast<char *>(
           realloc(i->tailoring,
                   (i->tailoring_alloced_length = (newlen + 32U * 1024U)))))) {
    return MY_XML_OK;
  }
  return MY_XML_ERROR;
}

static int fill_uchar(uint8_t *a, unsigned size, const char *str, size_t len) {
  unsigned i = 0;
  const char *s, *b, *e = str + len;

  for (s = str; s < e; i++) {
    for (; (s < e) && strchr(" \t\r\n", s[0]); s++)
      ;
    b = s;
    for (; (s < e) && !strchr(" \t\r\n", s[0]); s++)
      ;
    if (s == b || i > size) break;
    a[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  return 0;
}

static int fill_uint16(uint16_t *a, unsigned size, const char *str,
                       size_t len) {
  unsigned i = 0;

  const char *s, *b, *e = str + len;
  for (s = str; s < e; i++) {
    for (; (s < e) && strchr(" \t\r\n", s[0]); s++)
      ;
    b = s;
    for (; (s < e) && !strchr(" \t\r\n", s[0]); s++)
      ;
    if (s == b || i > size) break;
    a[i] = (uint16_t)strtol(b, nullptr, 16);
  }
  return 0;
}

static int tailoring_append(MY_XML_PARSER *st, const char *fmt, size_t len,
                            const char *attr) {
  auto *i = (struct my_cs_file_info *)st->user_data;
  size_t const newlen = i->tailoring_length + len + 64; /* 64 for format */
  if (MY_XML_OK == my_charset_file_tailoring_realloc(i, newlen)) {
    char *dst = i->tailoring + i->tailoring_length;
    sprintf(dst, fmt, (int)len, attr);
    i->tailoring_length += strlen(dst);
    return MY_XML_OK;
  }
  return MY_XML_ERROR;
}

static int tailoring_append2(MY_XML_PARSER *st, const char *fmt, size_t len1,
                             const char *attr1, size_t len2,
                             const char *attr2) {
  auto *i = (struct my_cs_file_info *)st->user_data;
  size_t const newlen =
      i->tailoring_length + len1 + len2 + 64; /* 64 for format */
  if (MY_XML_OK == my_charset_file_tailoring_realloc(i, newlen)) {
    char *dst = i->tailoring + i->tailoring_length;
    sprintf(dst, fmt, (int)len1, attr1, (int)len2, attr2);
    i->tailoring_length += strlen(dst);
    return MY_XML_OK;
  }
  return MY_XML_ERROR;
}

static size_t scan_one_character(const char *s, const char *e, my_wc_t *wc) {
  CHARSET_INFO *cs = &my_charset_utf8mb3_general_ci;
  if (s >= e) return 0;

  /* Escape sequence: \uXXXX */
  if (s[0] == '\\' && s + 2 < e && s[1] == 'u' && my_isxdigit(cs, s[2])) {
    size_t len = 3; /* We have at least one digit */
    for (s += 3; s < e && my_isxdigit(cs, s[0]); s++, len++) {
    }
    wc[0] = 0;
    return len;
  }
  if ((s[0] & 0x80) == 0) /* 7-bit character */
  {
    wc[0] = 0;
    return 1;
  } else /* Non-escaped character */
  {
    int const rc = cs->cset->mb_wc(cs, wc, pointer_cast<const uint8_t *>(s),
                                   pointer_cast<const uint8_t *>(e));
    if (rc > 0) return (size_t)rc;
  }
  return 0;
}

static int tailoring_append_abbreviation(MY_XML_PARSER *st, const char *fmt,
                                         size_t len, const char *attr) {
  size_t clen;
  const char *attrend = attr + len;
  my_wc_t wc;

  for (; (clen = scan_one_character(attr, attrend, &wc)) > 0; attr += clen) {
    assert(attr < attrend);
    if (tailoring_append(st, fmt, clen, attr) != MY_XML_OK) return MY_XML_ERROR;
  }
  return MY_XML_OK;
}

extern "C" {
static int cs_enter(MY_XML_PARSER *st, const char *attr, size_t len) {
  auto *i = (struct my_cs_file_info *)st->user_data;
  struct my_cs_file_section_st *s = cs_file_sec(attr, len);
  int const state = s ? s->state : 0;

  switch (state) {
    case 0:
      i->loader->reporter(WARNING_LEVEL, EE_UNKNOWN_LDML_TAG, (int)len, attr);
      break;

    case _CS_CHARSET:
      my_charset_file_reset_charset(i);
      break;

    case _CS_COLLATION:
      my_charset_file_reset_collation(i);
      break;

    case _CS_RESET:
      return tailoring_append(st, " &", 0, nullptr);

    default:
      break;
  }
  return MY_XML_OK;
}

static int cs_leave(MY_XML_PARSER *st, const char *attr, size_t len) {
  auto *i = (struct my_cs_file_info *)st->user_data;
  struct my_cs_file_section_st *s = cs_file_sec(attr, len);
  int const state = s ? s->state : 0;
  int rc;

  switch (state) {
    case _CS_COLLATION:
      if (i->tailoring_length) i->cs.tailoring = i->tailoring;
      rc = i->loader->add_collation(&i->cs);
      break;

    /* Rules: Logical Reset Positions */
    case _CS_RESET_FIRST_NON_IGNORABLE:
      rc = tailoring_append(st, "[first non-ignorable]", 0, nullptr);
      break;

    case _CS_RESET_LAST_NON_IGNORABLE:
      rc = tailoring_append(st, "[last non-ignorable]", 0, nullptr);
      break;

    case _CS_RESET_FIRST_PRIMARY_IGNORABLE:
      rc = tailoring_append(st, "[first primary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_LAST_PRIMARY_IGNORABLE:
      rc = tailoring_append(st, "[last primary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_FIRST_SECONDARY_IGNORABLE:
      rc = tailoring_append(st, "[first secondary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_LAST_SECONDARY_IGNORABLE:
      rc = tailoring_append(st, "[last secondary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_FIRST_TERTIARY_IGNORABLE:
      rc = tailoring_append(st, "[first tertiary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_LAST_TERTIARY_IGNORABLE:
      rc = tailoring_append(st, "[last tertiary ignorable]", 0, nullptr);
      break;

    case _CS_RESET_FIRST_TRAILING:
      rc = tailoring_append(st, "[first trailing]", 0, nullptr);
      break;

    case _CS_RESET_LAST_TRAILING:
      rc = tailoring_append(st, "[last trailing]", 0, nullptr);
      break;

    case _CS_RESET_FIRST_VARIABLE:
      rc = tailoring_append(st, "[first variable]", 0, nullptr);
      break;

    case _CS_RESET_LAST_VARIABLE:
      rc = tailoring_append(st, "[last variable]", 0, nullptr);
      break;

    default:
      rc = MY_XML_OK;
  }
  return rc;
}
}  // extern "C"

static const char *diff_fmt[5] = {"<%.*s", "<<%.*s", "<<<%.*s", "<<<<%.*s",
                                  "=%.*s"};

static const char *context_diff_fmt[5] = {
    "<%.*s|%.*s", "<<%.*s|%.*s", "<<<%.*s|%.*s", "<<<<%.*s|%.*s", "=%.*s|%.*s"};

extern "C" {
static int cs_value(MY_XML_PARSER *st, const char *attr, size_t len) {
  auto *i = (struct my_cs_file_info *)st->user_data;
  struct my_cs_file_section_st *s;
  int const state =
      (int)((s = cs_file_sec(st->attr.start, st->attr.end - st->attr.start))
                ? s->state
                : 0);
  int rc = MY_XML_OK;

  switch (state) {
    case _CS_MISC:
    case _CS_FAMILY:
    case _CS_ORDER:
      break;
    case _CS_ID:
      i->cs.number = strtol(attr, (char **)nullptr, 10);
      break;
    case _CS_BINARY_ID:
      i->cs.binary_number = strtol(attr, (char **)nullptr, 10);
      break;
    case _CS_PRIMARY_ID:
      i->cs.primary_number = strtol(attr, (char **)nullptr, 10);
      break;
    case _CS_COLNAME: {
      // Replace "utf8_" with "utf8mb3_" for external character sets.
      // Convert to lowercase first.
      mysql::collation::Name normalized_name(attr, len);
      if (normalized_name.to_string_view().starts_with("utf8_")) {
        std::string collation_name_string =
            std::string(normalized_name.to_string_view());
        // insert "mb3" to get "utf8mb3_xxx"
        collation_name_string.insert(4, "mb3");
        i->cs.m_coll_name =
            mstr(i->name, collation_name_string.c_str(),
                 collation_name_string.length(), MY_CS_NAME_SIZE - 1);
      } else {
        i->cs.m_coll_name = mstr(i->name, attr, len, MY_CS_NAME_SIZE - 1);
      }
    } break;
    case _CS_CSNAME: {
      // Replace "utf8" with "utf8mb3" for external character sets.
      // Convert to lowercase first.
      mysql::collation::Name normalized_name(attr, len);
      if (normalized_name.to_string_view() == "utf8") {
        i->cs.csname =
            mstr(i->csname, STRING_WITH_LEN("utf8mb3"), MY_CS_NAME_SIZE - 1);
      } else {
        i->cs.csname = mstr(i->csname, attr, len, MY_CS_NAME_SIZE - 1);
      }
      assert(0 != strcmp(i->cs.csname, "utf8"));
    } break;
    case _CS_CSDESCRIPT:
      i->cs.comment = mstr(i->comment, attr, len, MY_CS_CSDESCR_SIZE - 1);
      break;
    case _CS_FLAG:
      if (!strncmp("primary", attr, len))
        i->cs.state |= MY_CS_PRIMARY;
      else if (!strncmp("binary", attr, len))
        i->cs.state |= MY_CS_BINSORT;
      else if (!strncmp("compiled", attr, len))
        i->cs.state |= MY_CS_COMPILED;
      break;
    case _CS_UPPERMAP:
      fill_uchar(i->to_upper, MY_CS_TO_UPPER_TABLE_SIZE, attr, len);
      i->cs.to_upper = i->to_upper;
      break;
    case _CS_LOWERMAP:
      fill_uchar(i->to_lower, MY_CS_TO_LOWER_TABLE_SIZE, attr, len);
      i->cs.to_lower = i->to_lower;
      break;
    case _CS_UNIMAP:
      fill_uint16(i->tab_to_uni, MY_CS_TO_UNI_TABLE_SIZE, attr, len);
      i->cs.tab_to_uni = i->tab_to_uni;
      break;
    case _CS_COLLMAP:
      fill_uchar(i->sort_order, MY_CS_SORT_ORDER_TABLE_SIZE, attr, len);
      i->cs.sort_order = i->sort_order;
      break;
    case _CS_CTYPEMAP:
      fill_uchar(i->ctype, MY_CS_CTYPE_TABLE_SIZE, attr, len);
      i->cs.ctype = i->ctype;
      break;

    /* Special purpose commands */
    case _CS_UCA_VERSION:
      rc = tailoring_append(st, "[version %.*s]", len, attr);
      break;

    case _CS_CL_SUPPRESS_CONTRACTIONS:
      rc = tailoring_append(st, "[suppress contractions %.*s]", len, attr);
      break;

    case _CS_CL_OPTIMIZE:
      rc = tailoring_append(st, "[optimize %.*s]", len, attr);
      break;

    case _CS_CL_SHIFT_AFTER_METHOD:
      rc = tailoring_append(st, "[shift-after-method %.*s]", len, attr);
      break;

    /* Collation Settings */
    case _CS_ST_STRENGTH:
      /* 1, 2, 3, 4, 5, or primary, secondary, tertiary, quaternary, identical
       */
      rc = tailoring_append(st, "[strength %.*s]", len, attr);
      break;

    case _CS_ST_ALTERNATE:
      /* non-ignorable, shifted */
      rc = tailoring_append(st, "[alternate %.*s]", len, attr);
      break;

    case _CS_ST_BACKWARDS:
      /* on, off, 2 */
      rc = tailoring_append(st, "[backwards %.*s]", len, attr);
      break;

    case _CS_ST_NORMALIZATION:
      /*
        TODO for WL#896: check collations for normalization: vi.xml
        We want precomposed characters work well at this point.
      */
      /* on, off */
      rc = tailoring_append(st, "[normalization %.*s]", len, attr);
      break;

    case _CS_ST_CASE_LEVEL:
      /* on, off */
      rc = tailoring_append(st, "[caseLevel %.*s]", len, attr);
      break;

    case _CS_ST_CASE_FIRST:
      /* upper, lower, off */
      rc = tailoring_append(st, "[caseFirst %.*s]", len, attr);
      break;

    case _CS_ST_HIRAGANA_QUATERNARY:
      /* on, off */
      rc = tailoring_append(st, "[hiraganaQ %.*s]", len, attr);
      break;

    case _CS_ST_NUMERIC:
      /* on, off */
      rc = tailoring_append(st, "[numeric %.*s]", len, attr);
      break;

    case _CS_ST_VARIABLE_TOP:
      /* TODO for WL#896: check value format */
      rc = tailoring_append(st, "[variableTop %.*s]", len, attr);
      break;

    case _CS_ST_MATCH_BOUNDARIES:
      /* none, whole-character, whole-word */
      rc = tailoring_append(st, "[match-boundaries %.*s]", len, attr);
      break;

    case _CS_ST_MATCH_STYLE:
      /* minimal, medial, maximal */
      rc = tailoring_append(st, "[match-style %.*s]", len, attr);
      break;

    /* Rules */
    case _CS_RESET:
      rc = tailoring_append(st, "%.*s", len, attr);
      break;

    case _CS_DIFF1:
    case _CS_DIFF2:
    case _CS_DIFF3:
    case _CS_DIFF4:
    case _CS_IDENTICAL:
      rc = tailoring_append(st, diff_fmt[state - _CS_DIFF1], len, attr);
      break;

    /* Rules: Expansion */
    case _CS_EXP_EXTEND:
      rc = tailoring_append(st, " / %.*s", len, attr);
      break;

    case _CS_EXP_DIFF1:
    case _CS_EXP_DIFF2:
    case _CS_EXP_DIFF3:
    case _CS_EXP_DIFF4:
    case _CS_EXP_IDENTICAL:
      if (i->context[0]) {
        rc = tailoring_append2(st, context_diff_fmt[state - _CS_EXP_DIFF1],
                               strlen(i->context), i->context, len, attr);
        i->context[0] = 0;
      } else
        rc = tailoring_append(st, diff_fmt[state - _CS_EXP_DIFF1], len, attr);
      break;

    /* Rules: Context */
    case _CS_CONTEXT:
      if (len < sizeof(i->context)) {
        memcpy(i->context, attr, len);
        i->context[len] = '\0';
      }
      break;

    /* Rules: Abbreviating Ordering Specifications */
    case _CS_A_DIFF1:
    case _CS_A_DIFF2:
    case _CS_A_DIFF3:
    case _CS_A_DIFF4:
    case _CS_A_IDENTICAL:
      rc = tailoring_append_abbreviation(st, diff_fmt[state - _CS_A_DIFF1], len,
                                         attr);
      break;

    /* Rules: Placing Characters Before Others */
    case _CS_RESET_BEFORE:
      /*
        TODO for WL#896: Add this check into text customization parser:
        It is an error if the strength of the before relation is not identical
        to the relation after the reset. We'll need this for WL#896.
      */
      rc = tailoring_append(st, "[before %.*s]", len, attr);
      break;

    default:
      break;
  }

  return rc;
}
}  // extern "C"

bool my_parse_charset_xml(MY_CHARSET_LOADER *loader, const char *buf,
                          size_t len, MY_CHARSET_ERRMSG *errmsg) {
  MY_XML_PARSER p;
  struct my_cs_file_info info;
  bool rc;

  my_charset_file_init(&info);
  my_xml_parser_create(&p);
  my_xml_set_enter_handler(&p, cs_enter);
  my_xml_set_value_handler(&p, cs_value);
  my_xml_set_leave_handler(&p, cs_leave);
  info.loader = loader;
  my_xml_set_user_data(&p, (void *)&info);
  rc = my_xml_parse(&p, buf, len) != MY_XML_OK;
  my_xml_parser_free(&p);
  my_charset_file_free(&info);
  if (rc != MY_XML_OK) {
    const char *errstr = my_xml_error_string(&p);
    if (sizeof(errmsg->errarg) > 32 + strlen(errstr)) {
      sprintf(errmsg->errarg, "at line %d pos %d: %s",
              my_xml_error_lineno(&p) + 1, (int)my_xml_error_pos(&p),
              my_xml_error_string(&p));
    }
  }
  return rc;
}

/*
  Check repertoire: detect pure ascii strings
*/
unsigned my_string_repertoire(const CHARSET_INFO *cs, const char *str,
                              size_t length) {
  const char *strend = str + length;
  if (cs->mbminlen == 1) {
    for (; str < strend; str++) {
      if (((uint8_t)*str) > 0x7F) return MY_REPERTOIRE_UNICODE30;
    }
  } else {
    my_wc_t wc;
    int chlen;
    for (; (chlen = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(str),
                                    pointer_cast<const uint8_t *>(strend))) > 0;
         str += chlen) {
      if (wc > 0x7F) return MY_REPERTOIRE_UNICODE30;
    }
  }
  return MY_REPERTOIRE_ASCII;
}

/*
  Returns repertoire for charset
*/
unsigned my_charset_repertoire(const CHARSET_INFO *cs) {
  return cs->state & MY_CS_PUREASCII ? MY_REPERTOIRE_ASCII
                                     : MY_REPERTOIRE_UNICODE30;
}

/*
  Detect if a character set is 8bit,
  and it is pure ascii, i.e. doesn't have
  characters outside U+0000..U+007F
  This functions is shared between "conf_to_src"
  and dynamic charsets loader in "mysqld".
*/
bool my_charset_is_8bit_pure_ascii(const CHARSET_INFO *cs) {
  size_t code;
  if (!cs->tab_to_uni) return false;
  for (code = 0; code < 256; code++) {
    if (cs->tab_to_uni[code] > 0x7F) return false;
  }
  return true;
}

/*
  Shared function between conf_to_src and mysys.
  Check if a 8bit character set is compatible with
  ascii on the range 0x00..0x7F.
*/
bool my_charset_is_ascii_compatible(const CHARSET_INFO *cs) {
  if (!cs->tab_to_uni) return true;
  for (unsigned i = 0; i < 128; i++) {
    if (cs->tab_to_uni[i] != i) return false;
  }
  return true;
}

/**
  Convert a string between two character sets.
  'to' must be large enough to store (form_length * to_cs->mbmaxlen) bytes.

  @param [out] to       Store result here
  @param  to_length     Size of "to" buffer
  @param  to_cs         Character set of result string
  @param  from          Copy from here
  @param  from_length   Length of the "from" string
  @param  from_cs       Character set of the "from" string
  @param [out] errors   Number of conversion errors

  @return Number of bytes copied to 'to' string
*/

static size_t my_convert_internal(char *to, size_t to_length,
                                  const CHARSET_INFO *to_cs, const char *from,
                                  size_t from_length,
                                  const CHARSET_INFO *from_cs,
                                  unsigned *errors) {
  int cnvres;
  my_wc_t wc;
  const uint8_t *from_end = pointer_cast<const uint8_t *>(from) + from_length;
  char *to_start = to;
  uint8_t *to_end = pointer_cast<uint8_t *>(to) + to_length;
  my_charset_conv_mb_wc mb_wc = from_cs->cset->mb_wc;
  my_charset_conv_wc_mb wc_mb = to_cs->cset->wc_mb;
  unsigned error_count = 0;

  while (true) {
    if ((cnvres = (*mb_wc)(from_cs, &wc, pointer_cast<const uint8_t *>(from),
                           from_end)) > 0)
      from += cnvres;
    else if (cnvres == MY_CS_ILSEQ) {
      error_count++;
      from++;
      wc = '?';
    } else if (cnvres > MY_CS_TOOSMALL) {
      /*
        A correct multibyte sequence detected
        But it doesn't have Unicode mapping.
      */
      error_count++;
      from += (-cnvres);
      wc = '?';
    } else
      break;  // Not enough characters

  outp:
    if ((cnvres = (*wc_mb)(to_cs, wc, pointer_cast<uint8_t *>(to), to_end)) > 0)
      to += cnvres;
    else if (cnvres == MY_CS_ILUNI && wc != '?') {
      error_count++;
      wc = '?';
      goto outp;
    } else
      break;
  }
  *errors = error_count;
  return (uint32_t)(to - to_start);
}

/**
  Convert a string between two character sets.
   Optimized for quick copying of ASCII characters in the range 0x00..0x7F.
  'to' must be large enough to store (form_length * to_cs->mbmaxlen) bytes.

  @param [out] to       Store result here
  @param  to_length     Size of "to" buffer
  @param  to_cs         Character set of result string
  @param  from          Copy from here
  @param  from_length   Length of the "from" string
  @param  from_cs       Character set of the "from" string
  @param [out] errors   Number of conversion errors

  @return Number of bytes copied to 'to' string
*/

size_t my_convert(char *to, size_t to_length, const CHARSET_INFO *to_cs,
                  const char *from, size_t from_length,
                  const CHARSET_INFO *from_cs, unsigned *errors) {
  size_t length = 0;
  size_t length2 = 0;
  /*
    If any of the character sets is not ASCII compatible,
    immediately switch to slow mb_wc->wc_mb method.
  */
  if ((to_cs->state | from_cs->state) & MY_CS_NONASCII)
    return my_convert_internal(to, to_length, to_cs, from, from_length, from_cs,
                               errors);

  length = length2 = std::min(to_length, from_length);

  /*
    We no longer read non-aligned memory in the korr/store functions,
    but this is still faster than reading byte-by-byte.
  */
  for (; length >= 8; length -= 8, from += 8, to += 8) {
    if (uint8korr(from) & 0x8080808080808080) {
      break;
    }
    int8store(to, uint8korr(from));
  }

  while (length > 0 && static_cast<uint8_t>(*from) < 0x80) {
    *to++ = *from++;
    --length;
  }

  if (length == 0) {
    *errors = 0;
    return length2;
  }

  size_t const copied_length = length2 - length;
  to_length -= copied_length;
  from_length -= copied_length;
  return copied_length + my_convert_internal(to, to_length, to_cs, from,
                                             from_length, from_cs, errors);
}

/**
  Get the length of the first code in given sequence of chars.
  This func is introduced because we can't determine the length by
  checking the first byte only for gb18030, so we first try my_mbcharlen,
  and then my_mbcharlen_2 if necessary to get the length

  @param[in]  cs   charset_info
  @param[in]  s    start of the char sequence
  @param[in]  e    end of the char sequence
  @return     The length of the first code, or 0 for invalid code
*/
unsigned my_mbcharlen_ptr(const CHARSET_INFO *cs, const char *s,
                          const char *e) {
  unsigned len = my_mbcharlen(cs, (uint8_t)*s);
  if (len == 0 && my_mbmaxlenlen(cs) == 2 && s + 1 < e) {
    len = my_mbcharlen_2(cs, (uint8_t)*s, (uint8_t) * (s + 1));
    /* It could be either a valid multi-byte GB18030 code, or invalid
    gb18030 code if return value is 0 */
    assert(len == 0 || len == 2 || len == 4);
  }

  return len;
}

/**
   Identify whether given like pattern looks like a prefix pattern, which can
   become candidate for index only scan on prefix indexes.
   @param cs               Character set and collation pointer
   @param wildstr          Pointer to LIKE pattern.
   @param wildend          Pointer to end of LIKE pattern.
   @param escape           Escape character pattern,  typically '\'.
   @param w_many           'Many characters' pattern, typically '%'.
   @param[out] prefix_len  Length of LIKE pattern.

   @return Optimization status.
   @retval TRUE if LIKE pattern can be used for prefix index only scan.
   @retval FALSE else.
*/

bool my_is_prefixidx_cand(const CHARSET_INFO *cs, const char *wildstr,
                          const char *wildend, int escape, int w_many,
                          size_t *prefix_len) {
  my_wc_t wc;
  *prefix_len = 0;

  /* Find first occurrence of w_many pattern. */
  while (wildstr < wildend) {
    int res;
    if ((res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(wildstr),
                               pointer_cast<const uint8_t *>(wildend))) <= 0) {
      if (res == MY_CS_ILSEQ) /* Bad sequence */
        return false;
      return true; /* End of the string */
    }
    wildstr += res;

    if (wc == (my_wc_t)w_many) break;

    if (wc == (my_wc_t)escape) {
      if ((res =
               cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(wildstr),
                               pointer_cast<const uint8_t *>(wildend))) <= 0) {
        if (res == MY_CS_ILSEQ) /* Bad sequence */
          return false;
        (*prefix_len)++;
        return true; /* End of the string, last character is escape */
      }
      wildstr += res;
    }
    (*prefix_len)++;
  }

  /* If further char is not w_many then not a candidate prefix sequence. */
  while (wildstr < wildend) {
    int res;
    if ((res = cs->cset->mb_wc(cs, &wc, pointer_cast<const uint8_t *>(wildstr),
                               pointer_cast<const uint8_t *>(wildend))) <= 0) {
      if (res == MY_CS_ILSEQ) /* Bad sequence */
        return false;
      return true; /* End of the string */
    }

    if (wc != (my_wc_t)w_many) return false;
    wildstr += res;
  }

  return true;
}

static void *once_memdup(MY_CHARSET_LOADER *loader, const void *from,
                         size_t size) {
  void *ret = loader->once_alloc(size);
  if (ret == nullptr) return nullptr;
  memcpy(ret, from, size);
  return ret;
}

static const char *once_strdup(MY_CHARSET_LOADER *loader, const char *from) {
  return static_cast<char *>(
      once_memdup(loader, static_cast<const char *>(from), strlen(from) + 1));
}

static void simple_cs_init_functions(CHARSET_INFO *cs) {
  if (cs->state & MY_CS_BINSORT)
    cs->coll = &my_collation_8bit_bin_handler;
  else
    cs->coll = &my_collation_8bit_simple_ci_handler;

  cs->cset = &my_charset_8bit_handler;
}

static bool cs_copy_data(MY_CHARSET_LOADER *loader, CHARSET_INFO *to,
                         CHARSET_INFO *from) {
  to->number = from->number ? from->number : to->number;

  if (from->csname) {
    to->csname = once_strdup(loader, from->csname);
    if (to->csname == nullptr) return true;
  }

  if (from->m_coll_name) {
    to->m_coll_name = once_strdup(loader, from->m_coll_name);
    if (to->m_coll_name == nullptr) return true;
  }

  if (from->comment) {
    to->comment = once_strdup(loader, from->comment);
    if (to->comment == nullptr) return true;
  }

  if (from->ctype) {
    to->ctype = static_cast<uint8_t *>(
        once_memdup(loader, from->ctype, MY_CS_CTYPE_TABLE_SIZE));
    if (to->ctype == nullptr) return true;
  }

  if (from->to_lower) {
    to->to_lower = static_cast<uint8_t *>(
        once_memdup(loader, from->to_lower, MY_CS_TO_LOWER_TABLE_SIZE));
    if (to->to_lower == nullptr) return true;
  }

  if (from->to_upper) {
    to->to_upper = static_cast<uint8_t *>(
        once_memdup(loader, from->to_upper, MY_CS_TO_UPPER_TABLE_SIZE));
    if (to->to_upper == nullptr) return true;
  }

  if (from->sort_order) {
    to->sort_order = static_cast<uint8_t *>(
        once_memdup(loader, from->sort_order, MY_CS_SORT_ORDER_TABLE_SIZE));
    if (to->sort_order == nullptr) return true;
  }

  if (from->tab_to_uni) {
    size_t const sz = MY_CS_TO_UNI_TABLE_SIZE * sizeof(uint16_t);
    to->tab_to_uni =
        static_cast<uint16_t *>(once_memdup(loader, from->tab_to_uni, sz));
    if (to->tab_to_uni == nullptr) return true;
  }

  if (from->tailoring) {
    to->tailoring = once_strdup(loader, from->tailoring);
    if (to->tailoring == nullptr) return true;
  }

  return false;
}

static bool simple_cs_is_full(CHARSET_INFO *cs) {
  return ((cs->csname && cs->tab_to_uni && cs->ctype && cs->to_upper &&
           cs->to_lower) &&
          (cs->number && cs->m_coll_name &&
           (cs->sort_order || (cs->state & MY_CS_BINSORT))));
}

static void copy_uca_collation(CHARSET_INFO *to, CHARSET_INFO *from) {
  to->cset = from->cset;
  to->coll = from->coll;
  to->strxfrm_multiply = from->strxfrm_multiply;
  to->min_sort_char = from->min_sort_char;
  to->max_sort_char = from->max_sort_char;
  to->mbminlen = from->mbminlen;
  to->mbmaxlen = from->mbmaxlen;
  to->caseup_multiply = from->caseup_multiply;
  to->casedn_multiply = from->casedn_multiply;
  to->state |= MY_CS_STRNXFRM | MY_CS_UNICODE;
}

static void clear_cs_info(CHARSET_INFO *cs) {
  cs->number = 0;
  cs->primary_number = 0;
  cs->binary_number = 0;
  cs->m_coll_name = nullptr;
  cs->state = 0;
  cs->sort_order = nullptr;
}

MY_CHARSET_LOADER::~MY_CHARSET_LOADER() {
  for (void *x : m_delete_list) {
    free(x);
  }
}

int MY_CHARSET_LOADER::add_collation(CHARSET_INFO *cs) {
  if (cs->m_coll_name == nullptr) {
    assert(false);
    return MY_XML_OK;  // TODO: "MY_XML_ERROR" & error message?
  }
  if (cs->number >= MY_ALL_CHARSETS_SIZE) {
    // e.g: collation name="utf8_hugeid_ci" id="2047000000"
    char buff[1024];
    snprintf(buff, sizeof(buff), "Too big collation id: %u", cs->number);
    reporter(ERROR_LEVEL, EE_COLLATION_PARSER_ERROR, buff);
    return MY_XML_ERROR;
  }
  CHARSET_INFO *dst = mysql::collation_internals::entry->find_by_name_unsafe(
      mysql::collation::Name{cs->m_coll_name});
  if (dst == nullptr) {
    if (cs->number == 0) {
      assert(false);
      return MY_XML_OK;  // TODO: "MY_XML_ERROR" & error message?
    }
  } else if (dst->number != cs->number && cs->number != 0) {
    assert(false);
    return MY_XML_OK;  // TODO: "MY_XML_ERROR" & error message?
  }
  const unsigned cs_number = dst ? dst->number : cs->number;
  if (dst == nullptr) {
    dst = static_cast<CHARSET_INFO *>(once_alloc(sizeof(CHARSET_INFO)));
    if (dst == nullptr) return MY_XML_ERROR;  // OOM
    memset(dst, 0, sizeof(*dst));
  } else if (dst->state & MY_CS_COMPILED) {
    // Disallow overwriting compiled character sets
    clear_cs_info(cs);
    return MY_XML_OK;  // Just ignore it.
  }

  dst->number = cs_number;
  dst->state = cs->state;
  if (cs->primary_number == cs_number) {
    dst->state |= MY_CS_PRIMARY;
  }
  if (cs->binary_number == cs_number) {
    dst->state |= MY_CS_BINSORT;
  }

  if (cs_copy_data(this, dst, cs)) return MY_XML_ERROR;

  dst->caseup_multiply = dst->casedn_multiply = 1;
  dst->levels_for_compare = 1;

  if (!strcmp(cs->csname, "ucs2")) {
    copy_uca_collation(dst, &my_charset_ucs2_unicode_ci);
    dst->state |= MY_CS_LOADED | MY_CS_NONASCII;
  } else if (!strcmp(cs->csname, "utf8") || !strcmp(cs->csname, "utf8mb3")) {
    copy_uca_collation(dst, &my_charset_utf8mb3_unicode_ci);
    dst->ctype = my_charset_utf8mb3_unicode_ci.ctype;
    dst->state |= MY_CS_LOADED;
  } else if (!strcmp(cs->csname, "utf8mb4")) {
    copy_uca_collation(dst, &my_charset_utf8mb4_unicode_ci);
    dst->ctype = my_charset_utf8mb4_unicode_ci.ctype;
    dst->state |= MY_CS_LOADED;
  } else if (!strcmp(cs->csname, "utf16")) {
    copy_uca_collation(dst, &my_charset_utf16_unicode_ci);
    dst->state |= MY_CS_LOADED | MY_CS_NONASCII;
  } else if (!strcmp(cs->csname, "utf32")) {
    copy_uca_collation(dst, &my_charset_utf32_unicode_ci);
    dst->state |= MY_CS_LOADED | MY_CS_NONASCII;
  } else {
    const uint8_t *sort_order = dst->sort_order;
    simple_cs_init_functions(dst);
    dst->mbminlen = 1;
    dst->mbmaxlen = 1;
    if (simple_cs_is_full(dst)) {
      dst->state |= MY_CS_LOADED;
    }

    /*
      Check if case sensitive sort order: A < a < B.
      We need MY_CS_FLAG for regex library, and for
      case sensitivity flag for 5.0 client protocol,
      to support isCaseSensitive() method in JDBC driver
    */
    if (sort_order &&
        sort_order[static_cast<int>('A')] < sort_order[static_cast<int>('a')] &&
        sort_order[static_cast<int>('a')] < sort_order[static_cast<int>('B')])
      dst->state |= MY_CS_CSSORT;

    if (my_charset_is_8bit_pure_ascii(dst)) {
      dst->state |= MY_CS_PUREASCII;
    }
    if (!my_charset_is_ascii_compatible(cs)) {
      dst->state |= MY_CS_NONASCII;
    }
  }
  if (dst->ctype && is_supported_parser_charset(dst) &&
      init_state_maps(this, dst)) {
    return MY_XML_ERROR;
  }
  dst->state |= MY_CS_AVAILABLE;

  clear_cs_info(cs);
  if (mysql::collation_internals::entry->add_internal_collation(dst)) {
    return MY_XML_ERROR;  // TODO: error message on duplicates?
  }
  return MY_XML_OK;
}

void *MY_CHARSET_LOADER::once_alloc(size_t size) {
  void *ret = malloc(size);
  if (ret != nullptr) {
    m_delete_list.push_back(ret);
  }
  return ret;
}
