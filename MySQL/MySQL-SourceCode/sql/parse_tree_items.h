/* Copyright (c) 2013, 2025, Oracle and/or its affiliates.

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

#ifndef PARSE_TREE_ITEMS_INCLUDED
#define PARSE_TREE_ITEMS_INCLUDED

#include "field_types.h"  // enum_field_types
#include "lex_string.h"
#include "my_inttypes.h"  // TODO: replace with cstdint
#include "mysql/strings/m_ctype.h"
#include "sql/comp_creator.h"
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"       // Item_sum_count
#include "sql/item_timefunc.h"  // Item_func_now_local
#include "sql/parse_location.h"
#include "sql/parse_tree_helpers.h"  // Parse_tree_item
#include "sql/parse_tree_node_base.h"
#include "sql/set_var.h"

class PT_subquery;
class PT_window;
struct udf_func;

class PTI_truth_transform : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *expr;
  Bool_test truth_test;

 public:
  PTI_truth_transform(const POS &pos, Item *expr_arg, Bool_test truth_test)
      : super(pos), expr(expr_arg), truth_test(truth_test) {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 protected:
  void add_json_info(Json_object *obj) override;
};

class PTI_comp_op : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *left;
  chooser_compare_func_creator boolfunc2creator;
  Item *right;

 public:
  PTI_comp_op(const POS &pos, Item *left_arg,
              chooser_compare_func_creator boolfunc2creator_arg,
              Item *right_arg)
      : super(pos),
        left(left_arg),
        boolfunc2creator(boolfunc2creator_arg),
        right(right_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 protected:
  void add_json_info(Json_object *obj) override {
    obj->add_alias("operator", create_dom_ptr<Json_string>(
                                   (*boolfunc2creator)(false)->symbol(false)));
  }
};

class PTI_comp_op_all : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *left;
  chooser_compare_func_creator comp_op;
  bool is_all;
  PT_subquery *subselect;

 protected:
  void add_json_info(Json_object *obj) override {
    obj->add_alias("is_all", create_dom_ptr<Json_boolean>(is_all));
    obj->add_alias("operator", create_dom_ptr<Json_string>(
                                   (*comp_op)(false)->symbol(false)));
  }

 public:
  PTI_comp_op_all(const POS &pos, Item *left_arg,
                  chooser_compare_func_creator comp_op_arg, bool is_all_arg,
                  PT_subquery *subselect_arg)
      : super(pos),
        left(left_arg),
        comp_op(comp_op_arg),
        is_all(is_all_arg),
        subselect(subselect_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_simple_ident_ident : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_CSTRING ident;
  Symbol_location raw;

 public:
  PTI_simple_ident_ident(const POS &pos, const LEX_CSTRING &ident_arg)
      : super(pos), ident(ident_arg), raw(pos.raw) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

/**
  Parse tree Item wrapper for 3-dimentional simple_ident-s
*/
class PTI_simple_ident_q_3d : public Parse_tree_item {
  typedef Parse_tree_item super;

 protected:
  const char *db;
  const char *table;
  const char *field;

 public:
  PTI_simple_ident_q_3d(const POS &pos, const char *db_arg,
                        const char *table_arg, const char *field_arg)
      : super(pos), db(db_arg), table(table_arg), field(field_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

/**
  Parse tree Item wrapper for 3-dimentional simple_ident-s
*/
class PTI_simple_ident_q_2d : public PTI_simple_ident_q_3d {
  typedef PTI_simple_ident_q_3d super;

 public:
  PTI_simple_ident_q_2d(const POS &pos, const char *table_arg,
                        const char *field_arg)
      : super(pos, nullptr, table_arg, field_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_simple_ident_nospvar_ident : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_STRING ident;

 public:
  PTI_simple_ident_nospvar_ident(const POS &pos, const LEX_STRING &ident_arg)
      : super(pos), ident(ident_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_function_call_nonkeyword_now final : public Item_func_now_local {
  typedef Item_func_now_local super;

 public:
  PTI_function_call_nonkeyword_now(const POS &pos, uint8 dec_arg)
      : super(pos, dec_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_function_call_nonkeyword_sysdate : public Parse_tree_item {
  typedef Parse_tree_item super;

  uint8 dec;

 public:
  explicit PTI_function_call_nonkeyword_sysdate(const POS &pos, uint8 dec_arg)
      : super(pos), dec(dec_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_udf_expr : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *expr;
  LEX_STRING select_alias;
  Symbol_location expr_loc;

 public:
  PTI_udf_expr(const POS &pos, Item *expr_arg,
               const LEX_STRING &select_alias_arg,
               const Symbol_location &expr_loc_arg)
      : super(pos),
        expr(expr_arg),
        select_alias(select_alias_arg),
        expr_loc(expr_loc_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_function_call_generic_ident_sys : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_STRING ident;
  PT_item_list *opt_udf_expr_list;

  udf_func *udf;

 public:
  PTI_function_call_generic_ident_sys(const POS &pos,
                                      const LEX_STRING &ident_arg,
                                      PT_item_list *opt_udf_expr_list_arg)
      : super(pos),
        ident(ident_arg),
        opt_udf_expr_list(opt_udf_expr_list_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 protected:
  void add_json_info(Json_object *obj) override {
    obj->add_alias("func_name",
                   create_dom_ptr<Json_string>(ident.str, ident.length));
  }
};

/**
  Parse tree Item wrapper for 2-dimentional functional names (ex.: db.func_name)
*/
class PTI_function_call_generic_2d : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_STRING db;
  LEX_STRING func;
  PT_item_list *opt_expr_list;

 public:
  PTI_function_call_generic_2d(const POS &pos, const LEX_STRING &db_arg,
                               const LEX_STRING &func_arg,
                               PT_item_list *opt_expr_list_arg)
      : super(pos),
        db(db_arg),
        func(func_arg),
        opt_expr_list(opt_expr_list_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 protected:
  void add_json_info(Json_object *obj) override;
};

class PTI_text_literal : public Item_string {
  typedef Item_string super;

 protected:
  bool is_7bit;
  LEX_STRING literal;

  PTI_text_literal(const POS &pos, bool is_7bit_arg,
                   const LEX_STRING &literal_arg)
      : super(pos), is_7bit(is_7bit_arg), literal(literal_arg) {}
};

class PTI_text_literal_text_string : public PTI_text_literal {
  typedef PTI_text_literal super;

 public:
  PTI_text_literal_text_string(const POS &pos, bool is_7bit_arg,
                               const LEX_STRING &literal_arg)
      : super(pos, is_7bit_arg, literal_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_text_literal_nchar_string : public PTI_text_literal {
  typedef PTI_text_literal super;

 public:
  PTI_text_literal_nchar_string(const POS &pos, bool is_7bit_arg,
                                const LEX_STRING &literal_arg)
      : super(pos, is_7bit_arg, literal_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_text_literal_underscore_charset : public PTI_text_literal {
  typedef PTI_text_literal super;

  const CHARSET_INFO *cs;

 public:
  PTI_text_literal_underscore_charset(const POS &pos, bool is_7bit_arg,
                                      const CHARSET_INFO *cs_arg,
                                      const LEX_STRING &literal_arg)
      : super(pos, is_7bit_arg, literal_arg), cs(cs_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override {
    if (super::do_itemize(pc, res)) return true;

    init(literal.str, literal.length, cs, DERIVATION_COERCIBLE,
         MY_REPERTOIRE_UNICODE30);
    set_repertoire_from_value();
    set_cs_specified(true);
    return false;
  }
};

class PTI_text_literal_concat : public PTI_text_literal {
  typedef PTI_text_literal super;

  PTI_text_literal *head;

 public:
  PTI_text_literal_concat(const POS &pos, bool is_7bit_arg,
                          PTI_text_literal *head_arg, const LEX_STRING &tail)
      : super(pos, is_7bit_arg, tail), head(head_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_temporal_literal : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_STRING literal;
  enum_field_types field_type;
  const CHARSET_INFO *cs;

 public:
  PTI_temporal_literal(const POS &pos, const LEX_STRING &literal_arg,
                       enum_field_types field_type_arg,
                       const CHARSET_INFO *cs_arg)
      : super(pos),
        literal(literal_arg),
        field_type(field_type_arg),
        cs(cs_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_literal_underscore_charset_hex_num : public Item_string {
  typedef Item_string super;

 public:
  PTI_literal_underscore_charset_hex_num(const POS &pos,
                                         const CHARSET_INFO *charset,
                                         const LEX_STRING &literal)
      : super(pos, null_name_string,
              Item_hex_string::make_hex_str(literal.str, literal.length),
              charset) {}

  bool do_itemize(Parse_context *pc, Item **res) override {
    if (super::do_itemize(pc, res)) return true;

    set_repertoire_from_value();
    set_cs_specified(true);
    return check_well_formed_result(&str_value, true, true) == nullptr;
  }
};

class PTI_literal_underscore_charset_bin_num : public Item_string {
  typedef Item_string super;

 public:
  PTI_literal_underscore_charset_bin_num(const POS &pos,
                                         const CHARSET_INFO *charset,
                                         const LEX_STRING &literal)
      : super(pos, null_name_string,
              Item_bin_string::make_bin_str(literal.str, literal.length),
              charset) {}

  bool do_itemize(Parse_context *pc, Item **res) override {
    if (super::do_itemize(pc, res)) return true;

    set_cs_specified(true);
    return check_well_formed_result(&str_value, true, true) == nullptr;
  }
};

class PTI_variable_aux_set_var final : public Item_func_set_user_var {
  typedef Item_func_set_user_var super;

 public:
  PTI_variable_aux_set_var(const POS &pos, const LEX_STRING &var, Item *expr)
      : super(pos, var, expr) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_user_variable final : public Item_func_get_user_var {
  typedef Item_func_get_user_var super;

 public:
  PTI_user_variable(const POS &pos, const LEX_STRING &var) : super(pos, var) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

/**
  Parse tree Item wrapper for 3-dimentional variable names

  Example: \@global.default.x
*/
class PTI_get_system_variable : public Parse_tree_item {
  typedef Parse_tree_item super;

 public:
  PTI_get_system_variable(const POS &pos, enum_var_type scope,
                          const POS &name_pos, const LEX_CSTRING &opt_prefix,
                          const LEX_CSTRING &name)
      : super{pos},
        m_scope{scope},
        m_name_pos{name_pos},
        m_opt_prefix{opt_prefix},
        m_name{name} {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 private:
  const enum_var_type m_scope;
  const POS m_name_pos;
  const LEX_CSTRING m_opt_prefix;
  const LEX_CSTRING m_name;
};

class PTI_count_sym : public Item_sum_count {
  typedef Item_sum_count super;

 public:
  PTI_count_sym(const POS &pos, PT_window *w)
      : super(pos, (Item *)nullptr, w) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_in_sum_expr : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *expr;

 public:
  PTI_in_sum_expr(const POS &pos, Item *expr_arg)
      : super(pos), expr(expr_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_singlerow_subselect : public Parse_tree_item {
  typedef Parse_tree_item super;

  PT_subquery *subselect;

 public:
  PTI_singlerow_subselect(const POS &pos, PT_subquery *subselect_arg)
      : super(pos), subselect(subselect_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_exists_subselect : public Parse_tree_item {
  typedef Parse_tree_item super;

  PT_subquery *subselect;

 public:
  PTI_exists_subselect(const POS &pos, PT_subquery *subselect_arg)
      : super(pos), subselect(subselect_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_odbc_date : public Parse_tree_item {
  typedef Parse_tree_item super;

  LEX_STRING ident;
  Item *expr;

 protected:
  void add_json_info(Json_object *obj) override {
    obj->add_alias("date_type",
                   create_dom_ptr<Json_string>(ident.str, ident.length));
  }

 public:
  PTI_odbc_date(const POS &pos, const LEX_STRING &ident_arg, Item *expr_arg)
      : super(pos), ident(ident_arg), expr(expr_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_handle_sql2003_note184_exception : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *left;
  bool is_negation;
  Item *right;

 public:
  PTI_handle_sql2003_note184_exception(const POS &pos, Item *left_arg,
                                       bool is_negation_arg, Item *right_arg)
      : super(pos),
        left(left_arg),
        is_negation(is_negation_arg),
        right(right_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
  void add_json_info(Json_object *obj) override {
    obj->add_alias("negated", create_dom_ptr<Json_boolean>(is_negation));
  }
};

class PTI_expr_with_alias : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *expr;
  Symbol_location expr_loc;
  LEX_CSTRING alias;

 public:
  PTI_expr_with_alias(const POS &pos, Item *expr_arg,
                      const Symbol_location &expr_loc_arg,
                      const LEX_CSTRING &alias_arg)
      : super(pos), expr(expr_arg), expr_loc(expr_loc_arg), alias(alias_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 protected:
  void add_json_info(Json_object *obj) override;
};

class PTI_int_splocal : public Parse_tree_item {
  using super = Parse_tree_item;

 public:
  PTI_int_splocal(const POS &pos, const LEX_CSTRING &name)
      : super(pos), m_location{pos}, m_name{name} {}

  bool do_itemize(Parse_context *pc, Item **res) override;

 private:
  /// Location of the variable name.
  const POS m_location;

  /// Same data as in PTI_in_sum_expr#m_location but 0-terminated "for free".
  const LEX_CSTRING m_name;
};

class PTI_limit_option_ident : public PTI_int_splocal {
  using super = PTI_int_splocal;

 public:
  PTI_limit_option_ident(const POS &pos, const LEX_CSTRING &name)
      : super{pos, name} {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_limit_option_param_marker : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item_param *param_marker;

 public:
  explicit PTI_limit_option_param_marker(const POS &pos,
                                         Item_param *param_marker_arg)
      : super(pos), param_marker(param_marker_arg) {}

  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_context : public Parse_tree_item {
  typedef Parse_tree_item super;
  Item *expr;
  const enum_parsing_context m_parsing_place;

 protected:
  PTI_context(const POS &pos, Item *expr_arg, enum_parsing_context place)
      : super(pos), expr(expr_arg), m_parsing_place(place) {}

 public:
  bool do_itemize(Parse_context *pc, Item **res) override;
};

class PTI_where final : public PTI_context {
 public:
  PTI_where(const POS &pos, Item *expr_arg)
      : PTI_context(pos, expr_arg, CTX_WHERE) {}
};

class PTI_having final : public PTI_context {
 public:
  PTI_having(const POS &pos, Item *expr_arg)
      : PTI_context(pos, expr_arg, CTX_HAVING) {}
};

class PTI_qualify final : public PTI_context {
 public:
  PTI_qualify(const POS &pos, Item *expr_arg)
      : PTI_context(pos, expr_arg, CTX_QUALIFY) {}
};

#endif /* PARSE_TREE_ITEMS_INCLUDED */
