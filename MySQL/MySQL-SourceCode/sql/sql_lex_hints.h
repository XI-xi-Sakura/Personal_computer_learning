/*
   Copyright (c) 2014, 2025, Oracle and/or its affiliates.

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

/* A lexical scanner for optimizer hints pseudo-commentary syntax */

#ifndef SQL_LEX_HINTS_ICLUDED
#define SQL_LEX_HINTS_ICLUDED

#include <string.h>
#include <sys/types.h>

#include "lex_string.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysql/strings/m_ctype.h"
#include "sql/lex_symbol.h"
#include "sql/lexer_yystype.h"
#include "sql/parser_yystype.h"
#include "sql/sql_class.h"
#include "sql/sql_digest_stream.h"
#include "sql/sql_lex_hash.h"
#include "strings/sql_chars.h"

#include "sql/sql_hints.yy.h"

class PT_hint_list;

/// Lexical scanner for hint comments.
///
/// When the main lexical scanner recognizes the "/*+" delimiter, it calls
/// the hint parser (my_hint_parser_parse) to consume the rest of hint tokens
/// including the */ delimiter. The hint parser uses Hint_scanner as its own
/// lexer to scan hint-specific tokens.
class Hint_scanner {
  THD *thd;
  const CHARSET_INFO *cs;
  const bool is_ansi_quotes;
  size_t lineno;
  const hint_lex_char_classes *char_classes;

  const char *input_buf;
  const char *input_buf_end;

  const char *ptr;

  int prev_token;

  /**
    Digest buffer interface to append tokens.
  */
  sql_digest_state *digest_state;

 public:
  /**
    Current token (yytext) origin in the input_buf
  */
  const char *raw_yytext;
  /**
    Current token pointer (may be converted allocated string outside input_buf
  */
  const char *yytext;
  /**
    Length of the current token (see yytext)
  */
  size_t yyleng;

  bool has_hints;  ///< True if a hint comment is not empty (has any hints).

 public:
  Hint_scanner(THD *thd, size_t lineno_arg, const char *buf, size_t len,
               sql_digest_state *digest_state_arg);
  size_t get_lineno() const { return lineno; }
  const char *get_ptr() const { return ptr; }
  sql_digest_state *get_digest() { return digest_state; }
  void syntax_warning(const char *msg) const;

  int get_next_token() {
    DBUG_TRACE;
    prev_token = scan();
    add_hint_token_digest();
    return prev_token;
  }

 protected:
  int scan();

  template <hint_lex_char_classes Quote>
  int scan_quoted() {
    assert(Quote == HINT_CHR_BACKQUOTE || Quote == HINT_CHR_DOUBLEQUOTE ||
           Quote == HINT_CHR_QUOTE);
    assert(*ptr == '`' || *ptr == '"' || *ptr == '\'');

    const bool is_ident = (Quote == HINT_CHR_BACKQUOTE) ||
                          (is_ansi_quotes && Quote == HINT_CHR_DOUBLEQUOTE);
    const int ret = is_ident ? HINT_ARG_IDENT : HINT_ARG_TEXT;

    skip_byte("\"'`");  // skip opening quote sign
    adjust_token();     // reset yytext & yyleng

    size_t double_separators = 0;

    for (;;) {
      hint_lex_char_classes chr_class = peek_class();
      switch (chr_class) {
        case HINT_CHR_NL:
          skip_newline();
          continue;
        case HINT_CHR_MB:
          if (skip_mb()) return HINT_ERROR;
          continue;
        case HINT_CHR_ASTERISK:
          if (peek_class2() == HINT_CHR_SLASH)
            return HINT_ERROR;  // we don't support "*/" inside quoted
                                // identifiers
          skip_byte('*');
          continue;
        case HINT_CHR_EOF:
          return HINT_ERROR;
        case Quote:
          if (peek_class2() == Quote) {
            skip_byte("\"'`");
            skip_byte("\"'`");
            double_separators++;
            continue;
          } else {
            if (yyleng == 0) return HINT_ERROR;  // empty quoted identifier

            ptr++;  // skip closing quote

            if (thd->charset_is_system_charset && double_separators == 0) {
              yytext = thd->strmake(yytext, yyleng);  // null-terminate it.
              return ret;
            }

            LEX_STRING s;
            if (!thd->charset_is_system_charset) {
              if (thd->convert_string(&s, system_charset_info, yytext, yyleng,
                                      thd->charset()))
                return HINT_ERROR;  // OOM etc.
            } else {
              assert(0 < double_separators && double_separators < yyleng);
              s.length = yyleng - double_separators;
              s.str = static_cast<char *>(thd->alloc(s.length));
              if (s.str == nullptr) return HINT_ERROR;  // OOM
            }
            if (double_separators > 0)
              compact<Quote>(&s, yytext, yyleng, double_separators);

            yytext = s.str;
            yyleng = s.length;
            return ret;
          }
        default:
          skip_byte();
      }
    }
  }

  int scan_ident() {
    for (;;) {
      const hint_lex_char_classes chr_class = peek_class();
      switch (chr_class) {
        case HINT_CHR_IDENT:
        case HINT_CHR_DIGIT:
          skip_byte();
          continue;
        case HINT_CHR_MB:
          if (skip_mb()) return HINT_ERROR;
          continue;
        case HINT_CHR_EOF:
        default:
          return HINT_ARG_IDENT;
      }
    }
  }

  int scan_multiplier_or_ident() {
    assert(peek_class() == HINT_CHR_IDENT);
    switch (peek_byte()) {
      case 'K':
      case 'M':
      case 'G':
        break;
      default:
        return scan_ident();
    }
    skip_byte();

    switch (peek_class()) {
      case HINT_CHR_IDENT:
      case HINT_CHR_DIGIT:
        return scan_ident();
      default:
        return HINT_IDENT_OR_NUMBER_WITH_SCALE;
    }
  }

  int scan_query_block_name() {
    skip_byte('@');
    start_token();

    switch (peek_class()) {
      case HINT_CHR_IDENT:
      case HINT_CHR_DIGIT:
      case HINT_CHR_MB:
        if (scan_ident() == HINT_ARG_IDENT) {
          yytext = thd->strmake(yytext, yyleng);
          return HINT_ARG_QB_NAME;
        } else
          return HINT_ERROR;
      case HINT_CHR_BACKQUOTE:
        return scan_quoted<HINT_CHR_BACKQUOTE>() == HINT_ARG_IDENT
                   ? HINT_ARG_QB_NAME
                   : HINT_ERROR;
      case HINT_CHR_DOUBLEQUOTE:
        return scan_quoted<HINT_CHR_DOUBLEQUOTE>() == HINT_ARG_IDENT
                   ? HINT_ARG_QB_NAME
                   : HINT_ERROR;
      default:
        return HINT_ERROR;
    }
  }

  int scan_ident_or_keyword() {
    for (;;) {
      switch (peek_class()) {
        case HINT_CHR_IDENT:
        case HINT_CHR_DIGIT:
          skip_byte();
          continue;
        case HINT_CHR_MB:
          return scan_ident();
        case HINT_CHR_EOF:
        default:
          const SYMBOL *symbol =
              Lex_hash::hint_keywords.get_hash_symbol(yytext, yyleng);
          if (symbol)  // keyword
          {
            /*
              Override the yytext pointer to the short-living buffer with a
              long-living pointer to the same text (don't need to allocate a
              keyword string since symbol array is a global constant).
            */
            yytext = symbol->name;
            assert(yyleng == symbol->length);

            return symbol->tok;
          }

          yytext = thd->strmake(yytext, yyleng);
          return HINT_ARG_IDENT;
      }
    }
  }

  int scan_number_or_multiplier_or_ident() {
    assert(peek_class() == HINT_CHR_DIGIT);
    skip_byte();

    for (;;) {
      switch (peek_class()) {
        case HINT_CHR_DIGIT:
          skip_byte();
          continue;
        case HINT_CHR_DOT:
          return scan_fraction_digits();
        case HINT_CHR_IDENT:
          return scan_multiplier_or_ident();
        case HINT_CHR_MB:
          return scan_ident();
        case HINT_CHR_EOF:
        default:
          return HINT_ARG_NUMBER;
      }
    }
  }

  int scan_fraction_digits() {
    skip_byte('.');

    if (peek_class() == HINT_CHR_DIGIT)
      skip_byte();
    else
      return HINT_ERROR;

    for (;;) {
      switch (peek_class()) {
        case HINT_CHR_DIGIT:
          skip_byte();
          continue;
        case HINT_CHR_IDENT:
        case HINT_CHR_MB:
          return HINT_ERROR;
        default:
          return HINT_ARG_FLOATING_POINT_NUMBER;
      }
    }
  }

  bool eof() const {
    assert(ptr <= input_buf_end);
    return ptr >= input_buf_end;
  }

  char peek_byte() const {
    assert(!eof());
    return *ptr;
  }

  hint_lex_char_classes peek_class() const {
    return eof() ? HINT_CHR_EOF : char_classes[static_cast<uchar>(peek_byte())];
  }

  hint_lex_char_classes peek_class2() const {
    assert(ptr + 1 <= input_buf_end);
    return ptr + 1 >= input_buf_end ? HINT_CHR_EOF
                                    : char_classes[static_cast<uchar>(ptr[1])];
  }

  void skip_newline() {
    assert(!eof() && peek_byte() == '\n');
    skip_byte();
    lineno++;
  }

  uchar get_byte() {
    assert(!eof());
    const char ret = *ptr;
    yyleng++;
    ptr++;
    return ret;
  }

  void skip_byte() {
    assert(!eof());
    yyleng++;
    ptr++;
  }

  /**
    Skips the next byte. In the debug mode, abort if it's not found in @p byte.

    @param byte         A byte to compare with the byte we skip.
                        Unused in non-debug builds.
  */
  void skip_byte(char byte [[maybe_unused]]) {
    assert(peek_byte() == byte);
    skip_byte();
  }

  /**
    Skips the next byte. In the debug mode, abort if it's not found in @p str.

    @param str          A string of characters to compare with the next byte.
                        Unused in non-debug builds.
  */
  void skip_byte(const char *str [[maybe_unused]]) {
    assert(strchr(str, peek_byte()));
    skip_byte();
  }

  bool skip_mb() {
    const size_t len = my_ismbchar(cs, ptr, input_buf_end);
    if (len == 0) {
      ptr++;
      yyleng++;
      return true;
    }
    ptr += len;
    yyleng += len;
    return false;
  }

  void adjust_token() {
    yytext = ptr;
    yyleng = 0;
  }

  void start_token() {
    adjust_token();
    raw_yytext = ptr;
  }

  template <hint_lex_char_classes Separator>
  void compact(LEX_STRING *to, const char *from, size_t len, size_t doubles) {
    assert(doubles > 0);

    size_t d = doubles;
    char *t = to->str;
    for (const char *s = from, *end = from + len; s < end;) {
      switch (char_classes[(uchar)*s]) {
        case HINT_CHR_MB: {
          size_t hint_len = my_ismbchar(cs, s, end);
          assert(hint_len > 1);
          memcpy(t, s, hint_len);
          t += hint_len;
          s += hint_len;
        }
          continue;
        case Separator:
          assert(char_classes[(uchar)*s] == Separator);
          *t++ = *s++;
          s++;  // skip the 2nd separator
          d--;
          if (d == 0) {
            memcpy(t, s, end - s);
            to->length = len - doubles;
            return;
          }
          continue;
        case HINT_CHR_EOF:
          assert(0);
          to->length = 0;
          return;
        default:
          *t++ = *s++;
      }
    }
    assert(0);
    to->length = 0;
    return;
  }

  void add_hint_token_digest();

 private:
  /**
    Helper function to check digest buffer for overflow before adding tokens.

    @param token        A token number to add.
  */
  void add_digest(uint token) {
    if (digest_state == nullptr) return;  // Digest buffer is full.

    Lexer_yystype fake_yylvalue;
    /*
      YYSTYPE::LEX_STRING is designed to accept non-constant "char *": there is
      a consideration, that the lexer returns MEM_ROOT-allocated string values
      there, and the rest of server is welcome to modify that strings inplace
      (ind it does that in a few rare cases).
      The digest calculator never modify YYSTYPE::LEX_STRING::str data, so
      it is not practical to add extra memory allocation there: const_cast is
      enough.
    */
    fake_yylvalue.lex_str.str = const_cast<char *>(yytext);
    fake_yylvalue.lex_str.length = yyleng;

    digest_state = digest_add_token(digest_state, token, &fake_yylvalue);
  }
};

inline int my_hint_parser_lex(MY_HINT_PARSER_STYPE *yacc_yylval,
                              Hint_scanner *scanner) {
  auto yylval = reinterpret_cast<Lexer_yystype *>(yacc_yylval);
  const int ret = scanner->get_next_token();
  yylval->hint_string.str = scanner->yytext;
  yylval->hint_string.length = scanner->yyleng;
  return ret;
}

void my_hint_parser_error(THD *, Hint_scanner *, PT_hint_list **,
                          const char *msg);

#endif /* SQL_LEX_HINTS_ICLUDED */
