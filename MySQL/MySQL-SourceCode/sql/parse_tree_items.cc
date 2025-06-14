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

#include "sql/parse_tree_items.h"

#include <sys/types.h>  // TODO: replace with cstdint

#include "m_string.h"
#include "my_dbug.h"
#include "my_sqlcommand.h"
#include "my_sys.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysql/udf_registration_types.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "nulls.h"
#include "sql/auth/auth_acls.h"
#include "sql/item_cmpfunc.h"  // Item_func_eq
#include "sql/item_create.h"
#include "sql/item_subselect.h"
#include "sql/mysqld.h"  // using_udf_functions
#include "sql/parse_tree_nodes.h"
#include "sql/protocol.h"
#include "sql/sp.h"
#include "sql/sp_head.h"
#include "sql/sp_pcontext.h"  // sp_pcontext
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_show.h"  // append_identifier()
#include "sql/sql_udf.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/trigger_def.h"
#include "sql_string.h"
#include "template_utils.h"

/**
  Apply a truth test to given expression. Either the expression can implement
  it itself, or we create an Item node to implement it by wrapping the
  expression. Expression is possibly an incomplete predicate.

  @param pc   current parse context
  @param expr expression
  @param truth_test  test to apply

  @returns the resulting expression, or NULL if error
*/

static Item *change_truth_value_of_condition(Parse_context *pc, Item *expr,
                                             Item::Bool_test truth_test) {
  switch (truth_test) {
    case Item::BOOL_NEGATED:
    case Item::BOOL_IS_TRUE:
    case Item::BOOL_IS_FALSE:
    case Item::BOOL_NOT_TRUE:
    case Item::BOOL_NOT_FALSE:
      break;
    default:
      assert(false);
  }
  // Ensure that all incomplete predicates are made complete:
  if (!expr->is_bool_func()) {
    expr = make_condition(pc, expr);
    if (expr == nullptr) return nullptr;
  }
  Item *changed = expr->truth_transformer(pc->thd, truth_test);
  if (changed != nullptr) return changed;
  if (truth_test == Item::BOOL_NEGATED)
    return new (pc->mem_root) Item_func_not(expr);
  return new (pc->mem_root) Item_func_truth(expr, truth_test);
}

/**
  Helper to resolve the SQL:2003 Syntax exception 1) in @<in predicate@>.
  See SQL:2003, Part 2, section 8.4 @<in predicate@>, Note 184, page 383.
  This function returns the proper item for the SQL expression
  <code>left [NOT] IN ( expr )</code>
  @param pc the current parse context
  @param left the in predicand
  @param is_negation false for IN predicates, true for NOT IN predicates
  @param expr first and only expression of the in value list
  @return an expression representing the IN predicate.
*/
static Item *handle_sql2003_note184_exception(Parse_context *pc, Item *left,
                                              bool is_negation, Item *expr) {
  /*
    Relevant references for this issue:
    - SQL:2003, Part 2, section 8.4 <in predicate>, page 383,
    - SQL:2003, Part 2, section 7.2 <row value expression>, page 296,
    - SQL:2003, Part 2, section 6.3 <value expression primary>, page 174,
    - SQL:2003, Part 2, section 7.15 <subquery>, page 370,
    - SQL:2003 Feature F561, "Full value expressions".

    The exception in SQL:2003 Note 184 means:
    Item_singlerow_subselect, which corresponds to a <scalar subquery>,
    should be re-interpreted as an Item_in_subselect, which corresponds
    to a <table subquery> when used inside an <in predicate>.

    Our reading of Note 184 is recursive, so that all:
    - IN (( <subquery> ))
    - IN ((( <subquery> )))
    - IN '('^N <subquery> ')'^N
    - etc
    should be interpreted as a <table subquery>, no matter how deep in the
    expression the <subquery> is.
  */

  Item *result;

  DBUG_TRACE;

  if (expr->type() == Item::SUBQUERY_ITEM) {
    Item_subselect *expr2 = (Item_subselect *)expr;

    if (expr2->subquery_type() == Item_subselect::SCALAR_SUBQUERY) {
      Item_singlerow_subselect *expr3 = (Item_singlerow_subselect *)expr2;
      /*
        Implement the mandated change, by altering the semantic tree:
          left IN Item_singlerow_subselect(subselect)
        is modified to
          left IN (subselect)
        which is represented as
          Item_in_subselect(left, subselect)
      */
      Query_block *const qb = expr3->invalidate_and_restore_query_block();
      result = new (pc->mem_root) Item_in_subselect(expr->m_pos, left, qb);

      if (is_negation)
        result =
            change_truth_value_of_condition(pc, result, Item::BOOL_NEGATED);

      return result;
    }
  }

  if (is_negation)
    result = new (pc->mem_root) Item_func_ne(left, expr);
  else
    result = new (pc->mem_root) Item_func_eq(left, expr);

  return result;
}

bool PTI_comp_op::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || left->itemize(pc, &left) ||
      right->itemize(pc, &right))
    return true;

  *res = (*boolfunc2creator)(false)->create(left, right);
  return *res == nullptr;
}

bool PTI_comp_op_all::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || left->itemize(pc, &left) ||
      subselect->contextualize(pc))
    return true;

  *res = all_any_subquery_creator(pc->thd, m_pos, left, comp_op, is_all,
                                  subselect->value());

  return *res == nullptr;
}

bool PTI_function_call_nonkeyword_sysdate::do_itemize(Parse_context *pc,
                                                      Item **res) {
  if (super::do_itemize(pc, res)) return true;

  /*
    Unlike other time-related functions, SYSDATE() is
    replication-unsafe because it is not affected by the
    TIMESTAMP variable.  It is unsafe even if
    sysdate_is_now=1, because the slave may have
    sysdate_is_now=0.
  */
  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  if (global_system_variables.sysdate_is_now == 0)
    *res = new (pc->mem_root) Item_func_sysdate_local(dec);
  else
    *res = new (pc->mem_root) Item_func_now_local(dec);
  if (*res == nullptr) return true;
  lex->safe_to_cache_query = false;

  return false;
}

bool PTI_udf_expr::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || expr->itemize(pc, &expr)) return true;
  /*
   Use Item::name as a storage for the attribute value of user
   defined function argument. It is safe to use Item::name
   because the syntax will not allow having an explicit name here.
   See WL#1017 re. udf attributes.
  */
  if (select_alias.str) {
    expr->item_name.copy(select_alias.str, select_alias.length,
                         system_charset_info, false);
  }
  /*
     A field has to have its proper name in order for name
     resolution to work, something we are only guaranteed if we
     parse it out. If we hijack the input stream with
     [@1.cpp.start ... @1.cpp.end) we may get quoted or escaped names.
  */
  else if (expr->type() != Item::FIELD_ITEM &&
           expr->type() != Item::REF_ITEM /* For HAVING */)
    expr->item_name.copy(expr_loc.start, expr_loc.length(), pc->thd->charset());
  *res = expr;
  return false;
}

bool PTI_function_call_generic_ident_sys::do_itemize(Parse_context *pc,
                                                     Item **res) {
  if (super::do_itemize(pc, res)) return true;

  THD *thd = pc->thd;
  udf = nullptr;
  if (using_udf_functions && (udf = find_udf(ident.str, ident.length)) &&
      udf->type == UDFTYPE_AGGREGATE) {
    pc->select->in_sum_expr++;
  }

  if (sp_check_name(&ident)) return true;

  /*
    Implementation note:
    names are resolved with the following order:
    - MySQL native functions,
    - User Defined Functions,
    - Stored Functions (assuming the current <use> database)

    This will be revised with WL#2128 (SQL PATH)
  */
  Create_func *builder = find_native_function_builder(ident);
  if (builder)
    *res = builder->create_func(thd, ident, opt_udf_expr_list);
  else {
    if (udf) {
      if (udf->type == UDFTYPE_AGGREGATE) {
        pc->select->in_sum_expr--;
      }

      *res = Create_udf_func::s_singleton.create(thd, udf, opt_udf_expr_list);
    } else {
      builder = find_qualified_function_builder(thd);
      assert(builder);
      *res = builder->create_func(thd, ident, opt_udf_expr_list);
    }
  }
  return *res == nullptr || (*res)->itemize(pc, res);
}

void PTI_function_call_generic_2d::add_json_info(Json_object *obj) {
  String func_str;

  if (db.length > 0) {
    append_identifier_with_backtick(&func_str, db.str, db.length);
    func_str.append('.');
  }
  append_identifier_with_backtick(&func_str, func.str, func.length);
  obj->add_alias("func_name", create_dom_ptr<Json_string>(func_str.ptr(),
                                                          func_str.length()));
}

bool PTI_function_call_generic_2d::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  /*
    The following in practice calls:
    <code>Create_sp_func::create()</code>
    and builds a stored function.

    However, it's important to maintain the interface between the
    parser and the implementation in item_create.cc clean,
    since this will change with WL#2128 (SQL PATH):
    - INFORMATION_SCHEMA.version() is the SQL 99 syntax for the native
    function version(),
    - MySQL.version() is the SQL 2003 syntax for the native function
    version() (a vendor can specify any schema).
  */

  if (!db.str ||
      (check_and_convert_db_name(&db, false) != Ident_name_check::OK))
    return true;
  if (sp_check_name(&func)) return true;

  Create_qfunc *builder = find_qualified_function_builder(pc->thd);
  assert(builder);
  *res = builder->create(pc->thd, db, func, true, opt_expr_list);
  return *res == nullptr || (*res)->itemize(pc, res);
}

bool PTI_text_literal_nchar_string::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  uint repertoire = is_7bit ? MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  assert(my_charset_is_ascii_based(national_charset_info));
  init(literal.str, literal.length, national_charset_info, DERIVATION_COERCIBLE,
       repertoire);
  return false;
}

bool PTI_singlerow_subselect::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || subselect->contextualize(pc)) return true;
  *res = new (pc->mem_root) Item_singlerow_subselect(m_pos, subselect->value());
  if (*res == nullptr) return true;

  down_cast<Item_subselect *>(*res)->set_contextualized();
  pc->thd->add_item(*res);
  pc->select->n_scalar_subqueries++;

  return false;
}

bool PTI_exists_subselect::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || subselect->contextualize(pc)) return true;
  *res = new (pc->mem_root) Item_exists_subselect(m_pos, subselect->value());
  if (*res == nullptr) return true;

  Item_exists_subselect *exists = down_cast<Item_exists_subselect *>(*res);
  exists->set_contextualized();

  pc->thd->add_item(*res);

  return false;
}

bool PTI_handle_sql2003_note184_exception::do_itemize(Parse_context *pc,
                                                      Item **res) {
  if (super::do_itemize(pc, res) || left->itemize(pc, &left) ||
      right->itemize(pc, &right))
    return true;
  *res = handle_sql2003_note184_exception(pc, left, is_negation, right);
  return *res == nullptr;
}

bool PTI_expr_with_alias::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || expr->itemize(pc, &expr)) return true;

  if (alias.str) {
    if (pc->thd->lex->sql_command == SQLCOM_CREATE_VIEW &&
        check_column_name(alias)) {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), alias.str);
      return true;
    }
    expr->item_name.copy(alias.str, alias.length, system_charset_info, false);
  } else if (!expr->item_name.is_set()) {
    expr->item_name.copy(expr_loc.start, (uint)(expr_loc.end - expr_loc.start),
                         pc->thd->charset());
  }
  *res = expr;
  return false;
}

void PTI_expr_with_alias::add_json_info(Json_object *obj) {
  if (alias.str != nullptr)
    obj->add_alias("alias",
                   create_dom_ptr<Json_string>(alias.str, alias.length));
}

bool PTI_simple_ident_ident::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
  sp_variable *spv;

  if (pctx && (spv = pctx->find_variable(ident.str, ident.length, false))) {
    sp_head *sp = lex->sphead;

    assert(sp);

    /* We're compiling a stored procedure and found a variable */
    if (!lex->parsing_options.allows_variable) {
      my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
      return true;
    }

    *res = create_item_for_sp_var(
        thd, ident, spv, sp->m_parser_data.get_current_stmt_start_ptr(),
        raw.start, raw.end);
    lex->safe_to_cache_query = false;
  } else {
    if ((pc->select->parsing_place == CTX_HAVING &&
         pc->select->get_in_sum_expr() == 0u) ||
        (pc->select->parsing_place == CTX_QUALIFY &&
         pc->select->in_window_expr == 0u)) {
      *res = new (pc->mem_root) Item_ref(POS(), NullS, NullS, ident.str);
    } else {
      *res = new (pc->mem_root) Item_field(POS(), NullS, NullS, ident.str);
    }
    if (*res == nullptr || (*res)->itemize(pc, res)) return true;
  }
  return *res == nullptr;
}

bool PTI_simple_ident_q_3d::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  THD *thd = pc->thd;
  const char *schema =
      thd->get_protocol()->has_client_capability(CLIENT_NO_SCHEMA) ? nullptr
                                                                   : db;
  if (pc->select->no_table_names_allowed) {
    my_error(ER_TABLENAME_NOT_ALLOWED_HERE, MYF(0), table, thd->where);
    return true;
  }
  if ((pc->select->parsing_place != CTX_HAVING) ||
      (pc->select->get_in_sum_expr() > 0)) {
    *res = new (pc->mem_root) Item_field(POS(), schema, table, field);
  } else {
    *res = new (pc->mem_root) Item_ref(POS(), schema, table, field);
  }
  if (*res == nullptr) return true;
  return (*res)->itemize(pc, res);
}

bool PTI_simple_ident_q_2d::do_itemize(Parse_context *pc, Item **res) {
  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_head *sp = lex->sphead;

  /*
    References with OLD and NEW designators can be used in expressions in
    triggers. Semantic checks must ensure they are not used in invalid
    contexts, such as assignment targets.
  */
  if (sp && sp->m_type == enum_sp_type::TRIGGER &&
      (!my_strcasecmp(system_charset_info, table, "NEW") ||
       !my_strcasecmp(system_charset_info, table, "OLD"))) {
    if (Parse_tree_item::do_itemize(pc, res)) return true;

    bool new_row = (table[0] == 'N' || table[0] == 'n');

    if (sp->m_trg_chistics.event == TRG_EVENT_INSERT && !new_row) {
      my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "OLD", "on INSERT");
      return true;
    }

    if (sp->m_trg_chistics.event == TRG_EVENT_DELETE && new_row) {
      my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "NEW", "on DELETE");
      return true;
    }

    assert(!new_row || (sp->m_trg_chistics.event == TRG_EVENT_INSERT ||
                        sp->m_trg_chistics.event == TRG_EVENT_UPDATE));
    const bool read_only =
        !(new_row && sp->m_trg_chistics.action_time == TRG_ACTION_BEFORE);
    Item_trigger_field *trg_fld = new (pc->mem_root)
        Item_trigger_field(POS(), new_row ? TRG_NEW_ROW : TRG_OLD_ROW, field,
                           SELECT_ACL, read_only);
    if (trg_fld == nullptr || trg_fld->itemize(pc, (Item **)&trg_fld))
      return true;
    assert(trg_fld->type() == TRIGGER_FIELD_ITEM);

    /*
      Let us add this item to list of all Item_trigger_field objects
      in trigger.
    */
    lex->sphead->m_cur_instr_trig_field_items.link_in_list(
        trg_fld, &trg_fld->next_trg_field);

    *res = trg_fld;
  } else {
    if (super::do_itemize(pc, res)) return true;
  }
  return false;
}

bool PTI_simple_ident_nospvar_ident::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  if ((pc->select->parsing_place != CTX_HAVING) ||
      (pc->select->get_in_sum_expr() > 0)) {
    *res = new (pc->mem_root) Item_field(POS(), nullptr, nullptr, ident.str);
  } else {
    *res = new (pc->mem_root) Item_ref(POS(), nullptr, nullptr, ident.str);
  }
  if (*res == nullptr) return true;
  return (*res)->itemize(pc, res);
}

bool PTI_truth_transform::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || expr->itemize(pc, &expr)) return true;

  *res = change_truth_value_of_condition(pc, expr, truth_test);
  return *res == nullptr;
}

void PTI_truth_transform::add_json_info(Json_object *obj) {
  const char *truth_str = nullptr;
  switch (truth_test) {
    case BOOL_NEGATED:
      truth_str = "NOT";
      break;
    case BOOL_IS_TRUE:
      truth_str = "IS TRUE";
      break;
    case BOOL_NOT_TRUE:
      truth_str = "IS NOT TRUE";
      break;
    case BOOL_IS_FALSE:
      truth_str = "IS FALSE";
      break;
    case BOOL_NOT_FALSE:
      truth_str = "IS NOT FALSE";
      break;
    default:
      break;
  }
  if (truth_str != nullptr)
    obj->add_alias("truth_test", create_dom_ptr<Json_string>(truth_str));
}

bool PTI_function_call_nonkeyword_now::do_itemize(Parse_context *pc,
                                                  Item **res) {
  if (super::do_itemize(pc, res)) return true;

  pc->thd->lex->safe_to_cache_query = false;
  return false;
}

bool PTI_text_literal_text_string::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  THD *thd = pc->thd;
  LEX_STRING tmp;
  const CHARSET_INFO *cs_con = thd->variables.collation_connection;
  const CHARSET_INFO *cs_cli = thd->variables.character_set_client;
  uint repertoire = is_7bit && my_charset_is_ascii_based(cs_cli)
                        ? MY_REPERTOIRE_ASCII
                        : MY_REPERTOIRE_UNICODE30;
  if (thd->charset_is_collation_connection ||
      (repertoire == MY_REPERTOIRE_ASCII && my_charset_is_ascii_based(cs_con)))
    tmp = literal;
  else {
    if (thd->convert_string(&tmp, cs_con, literal.str, literal.length, cs_cli))
      return true;
  }
  init(tmp.str, tmp.length, cs_con, DERIVATION_COERCIBLE, repertoire);
  return false;
}

bool PTI_text_literal_concat::do_itemize(Parse_context *pc, Item **res) {
  Item *tmp_head;
  if (super::do_itemize(pc, res) || head->itemize(pc, &tmp_head)) return true;

  assert(tmp_head->type() == STRING_ITEM);
  Item_string *head_str = down_cast<Item_string *>(tmp_head);
  head_str->append(literal.str, literal.length);
  if ((head_str->collation.repertoire & MY_REPERTOIRE_EXTENDED) == 0) {
    // If the string has been pure ASCII so far, check the new part.
    const CHARSET_INFO *cs = pc->thd->variables.collation_connection;
    head_str->collation.repertoire |=
        my_string_repertoire(cs, literal.str, literal.length);
  }
  *res = head_str;
  return false;
}

bool PTI_temporal_literal::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  *res = create_temporal_literal(pc->thd, literal.str, literal.length, cs,
                                 field_type, true);
  return *res == nullptr;
}

bool PTI_variable_aux_set_var::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->parsing_options.allows_variable) {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return true;
  }
  lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return lex->set_var_list.push_back(this);
}

bool PTI_user_variable::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->parsing_options.allows_variable) {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return true;
  }
  lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return false;
}

bool PTI_get_system_variable::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->parsing_options.allows_variable) {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return true;
  }

  if (m_opt_prefix.str == nullptr &&
      (is_identifier(m_name.str, "warning_count") ||
       is_identifier(m_name.str, "error_count"))) {
    /*
      "Diagnostics variable" used in a non-diagnostics statement.
      Save the information we need for the former, but clear the
      rest of the diagnostics area on account of the latter.
      See reset_condition_info().
    */
    lex->keep_diagnostics = DA_KEEP_COUNTS;
  }
  *res = get_system_variable(pc, m_scope, m_opt_prefix, m_name, true);
  return *res == nullptr;
}

bool PTI_count_sym::do_itemize(Parse_context *pc, Item **res) {
  args[0] = new (pc->mem_root) Item_int(int32{0}, 1);
  if (args[0] == nullptr) return true;
  return super::do_itemize(pc, res);
}

bool PTI_in_sum_expr::do_itemize(Parse_context *pc, Item **res) {
  pc->select->in_sum_expr++;
  if (super::do_itemize(pc, res) || expr->itemize(pc, &expr)) return true;
  pc->select->in_sum_expr--;
  *res = expr;
  return false;
}

bool PTI_odbc_date::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res) || expr->itemize(pc, &expr)) return true;

  *res = nullptr;
  /*
    If "expr" is reasonably short pure ASCII string literal,
    try to parse known ODBC style date, time or timestamp literals,
    e.g:
    SELECT {d'2001-01-01'};
    SELECT {t'10:20:30'};
    SELECT {ts'2001-01-01 10:20:30'};
  */
  if (expr->type() == Item::STRING_ITEM &&
      expr->collation.repertoire == MY_REPERTOIRE_ASCII) {
    String buf;
    String *tmp_str = expr->val_str(&buf);
    if (tmp_str->length() < MAX_DATE_STRING_REP_LENGTH * 4) {
      enum_field_types type = MYSQL_TYPE_STRING;
      ErrConvString str(tmp_str);
      LEX_STRING *ls = &ident;
      if (ls->length == 1) {
        if (ls->str[0] == 'd') /* {d'2001-01-01'} */
          type = MYSQL_TYPE_DATE;
        else if (ls->str[0] == 't') /* {t'10:20:30'} */
          type = MYSQL_TYPE_TIME;
      } else if (ls->length == 2) /* {ts'2001-01-01 10:20:30'} */
      {
        if (ls->str[0] == 't' && ls->str[1] == 's') type = MYSQL_TYPE_DATETIME;
      }
      if (type != MYSQL_TYPE_STRING)
        *res = create_temporal_literal(pc->thd, str.ptr(), str.length(),
                                       system_charset_info, type, false);
    }
  }
  if (*res == nullptr) *res = expr;
  return false;
}

bool PTI_int_splocal::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;  // OOM

  LEX *const lex = pc->thd->lex;
  sp_head *const sp = lex->sphead;
  const char *query_start_ptr =
      sp ? sp->m_parser_data.get_current_stmt_start_ptr() : nullptr;

  Item_splocal *v =
      create_item_for_sp_var(pc->thd, m_name, nullptr, query_start_ptr,
                             m_location.raw.start, m_location.raw.end);
  if (v == nullptr) {
    return true;  // undefined variable or OOM
  }
  // Data type for a routine field is resolved during parsing, so this is OK:
  if (v->type() == Item::ROUTINE_FIELD_ITEM &&
      !is_integer_type(v->data_type())) {
    pc->thd->syntax_error_at(m_location, ER_SPVAR_NONINTEGER_TYPE, m_name.str);
    return true;
  }

  lex->safe_to_cache_query = false;
  *res = v;
  return false;
}

bool PTI_limit_option_ident::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;
  auto *v = down_cast<Item_splocal *>(*res);
  v->limit_clause_param = true;
  return false;
}

bool PTI_limit_option_param_marker::do_itemize(Parse_context *pc, Item **res) {
  Item *tmp_param;
  if (super::do_itemize(pc, res) || param_marker->itemize(pc, &tmp_param))
    return true;

  /*
    The Item_param::type() function may return various values, so we can't
    simply compare tmp_param->type() with some constant, cast tmp_param
    to Item_param* and assign the result back to param_marker.
    OTOH we ensure that Item_param::itemize() always substitute the output
    parameter with "this" pointer of Item_param object, so we can skip
    the check and the assignment.
  */
  assert(tmp_param == param_marker);

  *res = param_marker;
  return false;
}

bool PTI_context::do_itemize(Parse_context *pc, Item **res) {
  if (super::do_itemize(pc, res)) return true;

  pc->select->parsing_place = m_parsing_place;

  if (expr->itemize(pc, &expr)) return true;

  if (!expr->is_bool_func()) {
    expr = make_condition(pc, expr);
    if (expr == nullptr) return true;
  }

  // Ensure we're resetting parsing place of the right select
  assert(pc->select->parsing_place == m_parsing_place);
  pc->select->parsing_place = CTX_NONE;
  assert(expr != nullptr);
  expr->apply_is_true();

  *res = expr;
  return false;
}
