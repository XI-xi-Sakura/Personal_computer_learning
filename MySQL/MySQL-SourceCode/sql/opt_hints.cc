/* Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#include "sql/opt_hints.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include "my_table_map.h"
#include "mysql/strings/m_ctype.h"
#include "mysql/udf_registration_types.h"
#include "mysqld_error.h"
#include "sql/derror.h"  // ER_THD
#include "sql/error_handler.h"
#include "sql/item.h"
#include "sql/item_subselect.h"
#include "sql/key.h"
#include "sql/mysqld.h"  // table_alias_charset
#include "sql/nested_join.h"
#include "sql/parse_tree_hints.h"
#include "sql/set_var.h"
#include "sql/sql_class.h"  // THD
#include "sql/sql_const.h"
#include "sql/sql_error.h"      // Sql_condition
#include "sql/sql_optimizer.h"  // JOIN class
#include "sql/sql_select.h"
#include "sql/table.h"
#include "string_with_len.h"

struct MEM_ROOT;

/**
  Information about hints. Should be
  synchronized with opt_hints_enum enum.

  Note: Hint name depends on hint state. 'NO_' prefix is added
  if appropriate hint state bit(see Opt_hints_map::hints) is not
  set. Depending on 'switch_state_arg' argument in 'parse tree
  object' constructors(see parse_tree_hints.[h,cc]) implementor
  can control wishful form of the hint name.
*/

struct st_opt_hint_info opt_hint_info[] = {
    {"BKA", true, true, false},
    {"BNL", true, true, false},
    {"ICP", true, true, false},
    {"MRR", true, true, false},
    {"NO_RANGE_OPTIMIZATION", true, true, false},
    {"MAX_EXECUTION_TIME", false, false, false},
    {"QB_NAME", false, false, false},
    {"SEMIJOIN", false, false, false},
    {"SUBQUERY", false, false, false},
    {"MERGE", true, true, false},
    {"JOIN_PREFIX", false, false, true},
    {"JOIN_SUFFIX", false, false, true},
    {"JOIN_ORDER", false, false, true},
    {"JOIN_FIXED_ORDER", false, true, false},
    {"INDEX_MERGE", false, false, false},
    {"RESOURCE_GROUP", false, false, false},
    {"SKIP_SCAN", false, false, false},
    {"HASH_JOIN", true, true, false},
    {"INDEX", false, false, false},
    {"JOIN_INDEX", false, false, false},
    {"GROUP_INDEX", false, false, false},
    {"ORDER_INDEX", false, false, false},
    {"DERIVED_CONDITION_PUSHDOWN", true, true, false},
    {nullptr, false, false, false}};

/**
  Prefix for system generated query block name.
  Used in information warning in EXPLAIN oputput.
*/

const LEX_CSTRING sys_qb_prefix = {"select#", 7};

/*
  Compare LEX_CSTRING objects.

  @param s     The 1st string
  @param t     The 2nd string
  @param cs    Pointer to character set

  @return  0 if strings are equal
           1 if s is greater
          -1 if t is greater
*/

int cmp_lex_string(const LEX_CSTRING &s, const LEX_CSTRING &t,
                   const CHARSET_INFO *cs) {
  return cs->coll->strnncollsp(cs, pointer_cast<const uchar *>(s.str), s.length,
                               pointer_cast<const uchar *>(t.str), t.length);
}

bool Opt_hints::get_switch(opt_hints_enum type_arg) const {
  if (is_specified(type_arg)) return hints_map.switch_on(type_arg);

  if (opt_hint_info[type_arg].check_upper_lvl)
    return parent->get_switch(type_arg);

  return false;
}

Opt_hints *Opt_hints::find_by_name(const LEX_CSTRING *name_arg,
                                   const CHARSET_INFO *cs) const {
  for (uint i = 0; i < child_array.size(); i++) {
    const LEX_CSTRING *name = child_array[i]->get_print_name();
    if (!cmp_lex_string(*name, *name_arg, cs)) return child_array[i];
  }
  return nullptr;
}

void Opt_hints::print(const THD *thd, String *str, enum_query_type query_type) {
  for (uint i = 0; i < MAX_HINT_ENUM; i++) {
    if (opt_hint_info[i].irregular_hint) continue;
    const opt_hints_enum hint = static_cast<opt_hints_enum>(i);
    /*
       If printing a normalized query, also unresolved hints will be printed.
       (This is needed by query rewrite plugins which request
       normalized form before resolving has been performed.)
    */
    if (is_specified(hint) && !ignore_print(hint) &&
        (is_resolved(hint) || query_type == QT_NORMALIZED_FORMAT)) {
      append_hint_type(str, hint);
      str->append(STRING_WITH_LEN("("));
      append_name(thd, str);
      if (!opt_hint_info[i].switch_hint)
        get_complex_hints(hint)->append_args(thd, str);
      str->append(STRING_WITH_LEN(") "));
    }
  }

  print_irregular_hints(thd, str);

  for (uint i = 0; i < child_array.size(); i++)
    child_array[i]->print(thd, str, query_type);
}

void Opt_hints::append_hint_type(String *str, opt_hints_enum type) {
  const char *hint_name = opt_hint_info[type].hint_name;
  if (!hints_map.switch_on(type)) str->append(STRING_WITH_LEN("NO_"));
  str->append(hint_name);
}

void Opt_hints::print_warn_unresolved(THD *thd) {
  String hint_name_str, hint_type_str;
  append_name(thd, &hint_name_str);

  for (uint i = 0; i < MAX_HINT_ENUM; i++) {
    if (is_specified(static_cast<opt_hints_enum>(i))) {
      hint_type_str.length(0);
      append_hint_type(&hint_type_str, static_cast<opt_hints_enum>(i));
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_UNRESOLVED_HINT_NAME,
          ER_THD(thd, ER_UNRESOLVED_HINT_NAME), hint_name_str.c_ptr_safe(),
          hint_type_str.c_ptr_safe());
      get_parent()->set_unresolved(static_cast<opt_hints_enum>(i));
    }
  }
}

void Opt_hints::check_unresolved(THD *thd) {
  if (!is_resolved(MAX_HINT_ENUM)) print_warn_unresolved(thd);

  if (!is_all_resolved()) {
    for (uint i = 0; i < child_array.size(); i++)
      child_array[i]->check_unresolved(thd);
  }
}

PT_hint *Opt_hints_global::get_complex_hints(opt_hints_enum type) {
  if (type == MAX_EXEC_TIME_HINT_ENUM) return max_exec_time;

  assert(0);
  return nullptr;
}

void Opt_hints_global::print_irregular_hints(const THD *thd, String *str) {
  if (sys_var_hint) sys_var_hint->print(thd, str);
}

Opt_hints_qb::Opt_hints_qb(Opt_hints *opt_hints_arg, MEM_ROOT *mem_root_arg,
                           uint select_number_arg)
    : Opt_hints(nullptr, opt_hints_arg, mem_root_arg),
      select_number(select_number_arg),
      subquery_hint(nullptr),
      semijoin_hint(nullptr),
      join_order_hints(mem_root_arg),
      join_order_hints_ignored(0) {
  sys_name.str = buff;
  sys_name.length =
      snprintf(buff, sizeof(buff), "%s%x", sys_qb_prefix.str, select_number);
}

PT_hint *Opt_hints_qb::get_complex_hints(opt_hints_enum type) {
  if (type == SEMIJOIN_HINT_ENUM) return semijoin_hint;

  if (type == SUBQUERY_HINT_ENUM) return subquery_hint;

  assert(0);
  return nullptr;
}

Opt_hints_table *Opt_hints_qb::adjust_table_hints(Table_ref *tr) {
  const LEX_CSTRING str = {tr->alias, strlen(tr->alias)};
  Opt_hints_table *tab =
      static_cast<Opt_hints_table *>(find_by_name(&str, table_alias_charset));

  tr->opt_hints_qb = this;

  if (!tab)  // Tables not found
    return nullptr;

  tab->adjust_key_hints(tr);
  return tab;
}

bool Opt_hints_qb::semijoin_enabled(const THD *thd) const {
  if (subquery_hint)  // SUBQUERY hint disables semi-join
    return false;

  if (semijoin_hint) {
    // SEMIJOIN hint will always force semijoin regardless of optimizer_switch
    if (semijoin_hint->switch_on()) return true;

    // NO_SEMIJOIN hint.  If strategy list is empty, do not use SEMIJOIN
    if (semijoin_hint->get_args() == 0) return false;

    // Fall through: NO_SEMIJOIN w/ strategies neither turns SEMIJOIN off nor on
  }

  return thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SEMIJOIN);
}

uint Opt_hints_qb::sj_enabled_strategies(uint opt_switches) const {
  // Hints override switches
  if (semijoin_hint) {
    const uint strategies = semijoin_hint->get_args();
    if (semijoin_hint->switch_on())  // SEMIJOIN hint
      return (strategies == 0) ? opt_switches : strategies;

    // NO_SEMIJOIN hint. Hints and optimizer_switch both affect strategies
    return ~strategies & opt_switches;
  }

  return opt_switches;
}

Subquery_strategy Opt_hints_qb::subquery_strategy() const {
  if (subquery_hint)
    return static_cast<Subquery_strategy>(subquery_hint->get_args());

  return Subquery_strategy::UNSPECIFIED;
}

void Opt_hints_qb::print_irregular_hints(const THD *thd, String *str) {
  /* Print join order hints */
  for (uint i = 0; i < join_order_hints.size(); i++) {
    if (join_order_hints_ignored & (1ULL << i)) continue;
    const PT_qb_level_hint *hint = join_order_hints[i];
    str->append(opt_hint_info[hint->type()].hint_name);
    str->append(STRING_WITH_LEN("("));
    append_name(thd, str);
    str->append(STRING_WITH_LEN(" "));
    hint->append_args(thd, str);
    str->append(STRING_WITH_LEN(") "));
  }
}

/**
  Print warning about unresolved table for join order hints.

  @param thd pointer to THD object
  @param type hint type
  @param hint_table table name
*/

static void print_join_order_warn(THD *thd, opt_hints_enum type,
                                  const Hint_param_table *hint_table) {
  String hint_name_str, hint_type_str;
  hint_type_str.append(opt_hint_info[type].hint_name);
  append_table_name(thd, &hint_name_str, &hint_table->opt_query_block,
                    &hint_table->table);
  push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNRESOLVED_HINT_NAME,
                      ER_THD(thd, ER_UNRESOLVED_HINT_NAME),
                      hint_name_str.c_ptr_safe(), hint_type_str.c_ptr_safe());
}

/**
  Function compares hint table name and Table_ref table name.
  Query block name is taken into account.

  @param hint_table         hint table name
  @param table              pointer to Table_ref object

  @return false if table names are equal, true otherwise.
*/

static bool compare_table_name(const Hint_param_table *hint_table,
                               const Table_ref *table) {
  const LEX_CSTRING *hint_qb_name = &hint_table->opt_query_block;
  const LEX_CSTRING *hint_table_name = &hint_table->table;

  const LEX_CSTRING *table_qb_name =
      table->opt_hints_qb ? table->opt_hints_qb->get_name() : nullptr;
  const LEX_CSTRING table_name = {table->alias, strlen(table->alias)};

  if (table_qb_name && table_qb_name->length > 0 && hint_qb_name->length > 0) {
    if (cmp_lex_string(*hint_qb_name, *table_qb_name, system_charset_info))
      return true;
  }

  if (cmp_lex_string(*hint_table_name, table_name, system_charset_info))
    return true;

  return false;
}

/**
  Function returns dependencies used for updating table dependencies
  depending on hint type.

  @param type          hint type
  @param hint_tab_map  hint table map
  @param table_map     table map

  @return table dependencies.
*/

static table_map get_other_dep(opt_hints_enum type, table_map hint_tab_map,
                               table_map table_map) {
  switch (type) {
    case JOIN_PREFIX_HINT_ENUM:
      if (hint_tab_map & table_map)  // Hint table: No additional dependencies
        return 0;
      // Other tables: depend on all hint tables
      return hint_tab_map;
    case JOIN_SUFFIX_HINT_ENUM:
      if (hint_tab_map & table_map)  // Hint table: depends on all other tables
        return ~hint_tab_map;
      return 0;
    case JOIN_ORDER_HINT_ENUM:
      return 0;  // No additional dependencies
    default:
      assert(0);
      break;
  }
  return 0;
}

/**
  Auxiliary class is used to save/restore table dependencies.
*/

class Join_order_hint_handler {
  JOIN *join;
  table_map *orig_dep_array;  ///< Original table dependencies

 public:
  Join_order_hint_handler(JOIN *join_arg)
      : join(join_arg), orig_dep_array(nullptr) {}

  /**
    Allocates and initializes orig_dep_array.

    @return true if orig_dep_array is allocated, false otherwise.
  */
  bool init() {
    orig_dep_array =
        (table_map *)join->thd->alloc(sizeof(table_map) * join->tables);

    if (orig_dep_array == nullptr) return true;

    for (uint i = 0; i < join->tables; i++) {
      JOIN_TAB *tab = &join->join_tab[i];
      orig_dep_array[i] = tab->dependent;
    }
    return false;
  }

  void no_restore_deps() { orig_dep_array = nullptr; }

  /**
    Restore original dependencies if necessary.
  */

  ~Join_order_hint_handler() {
    if (orig_dep_array == nullptr) return;

    for (uint i = 0; i < join->tables; i++) {
      JOIN_TAB *tab = &join->join_tab[i];
      tab->dependent = orig_dep_array[i];
    }
  }
};

/**
  Function updates dependencies for nested joins. If table
  specified in the hint belongs to nested join, we need
  to update dependencies of all tables of the nested join
  with the same dependency as for the hint table. It is also
  necessary to update all tables of the nested joins this table
  is part of.

  @param join             pointer to JOIN object
  @param hint_tab         pointer to JOIN_TAB object
  @param hint_tab_map     map of the tables, specified in the hint
*/

static void update_nested_join_deps(JOIN *join, const JOIN_TAB *hint_tab,
                                    table_map hint_tab_map) {
  const Table_ref *table = hint_tab->table_ref;
  if (table->embedding) {
    for (uint i = 0; i < join->tables; i++) {
      JOIN_TAB *tab = &join->join_tab[i];
      if (tab->table_ref->embedding) {
        const NESTED_JOIN *const nested_join =
            tab->table_ref->embedding->nested_join;
        if (hint_tab->embedding_map & nested_join->nj_map)
          tab->dependent |= (hint_tab_map & ~nested_join->used_tables);
      }
    }
  }
}

/**
  Function resolves hint tables, checks and sets table dependencies
  according to the hint. If the hint is ignored due to circular table
  dependencies, original dependencies are restored.

  @param join             pointer to JOIN object
  @param hint_table_list  hint table list
  @param type             hint type

  @return false if hint is applied, true otherwise.
*/

static bool set_join_hint_deps(JOIN *join,
                               const Hint_param_table_list *hint_table_list,
                               opt_hints_enum type) {
  /*
    Make a copy of the original table dependencies.
    If an error occurs when applying the hint dependencies,
    the original dependencies will be restored by the destructor for this
    object.
  */
  Join_order_hint_handler hint_handler(join);
  // Map of the tables, specified in the hint
  table_map hint_tab_map = 0;

  if (hint_handler.init()) return true;

  for (const Hint_param_table *hint_table = hint_table_list->begin();
       hint_table < hint_table_list->end(); hint_table++) {
    bool hint_table_found = false;
    for (uint i = 0; i < join->tables; i++) {
      const Table_ref *table = join->join_tab[i].table_ref;
      if (!compare_table_name(hint_table, table)) {
        hint_table_found = true;
        /*
          Const tables are excluded from the process of dependency setting
          since they are always first in the table order. Note that it
          does not prevent the hint from being applied to the non-const
          tables of the hint.
        */
        if (join->const_table_map & table->map()) break;

        JOIN_TAB *tab = &join->join_tab[i];
        // Hint tables are always dependent on preceding tables
        tab->dependent |= hint_tab_map;
        update_nested_join_deps(join, tab, hint_tab_map);
        hint_tab_map |= tab->table_ref->map();
        break;
      }
    }

    if (!hint_table_found) {
      print_join_order_warn(join->thd, type, hint_table);
      return true;
    }
  }

  // Add dependencies that are related to non-hint tables
  for (uint i = 0; i < join->tables; i++) {
    JOIN_TAB *tab = &join->join_tab[i];
    const table_map dependent_tables =
        get_other_dep(type, hint_tab_map, tab->table_ref->map());
    update_nested_join_deps(join, tab, dependent_tables);
    tab->dependent |= dependent_tables;
  }

  if (join->propagate_dependencies()) return true;

  hint_handler.no_restore_deps();
  return false;
}

/**
  Checks if any join order hints have been specified in query.

  @return true if join order hints present, false otherwise.
*/
bool Opt_hints_qb::has_join_order_hints() const {
  return join_order_hints.size() > 0;
}

/**
  Deletes all the join order hints.
*/
void Opt_hints_qb::clear_join_order_hints() { join_order_hints.clear(); }

/**
 Check if a join table matches a hinted table. If the join table is an outer
 join, semijoin or antijoin, check all its nested tables for a match with the
 hinted table and return the matching table.

 @param hint_table Table specified in hint
 @param table      Table from join list

 @return Join table which matches hinted table
*/
static Table_ref *find_hinted_table(const Hint_param_table *hint_table,
                                    const Table_ref *table) {
  if (!table->nested_join) {
    if (!compare_table_name(hint_table, table))
      return const_cast<Table_ref *>(table);
    return nullptr;
  }
  for (auto nest_it = table->nested_join->m_tables.rbegin();
       nest_it != table->nested_join->m_tables.rend();
       ++nest_it) {  // The list goes backwards.
    Table_ref *nest_tl = *nest_it;
    if (nest_tl->nested_join) {
      Table_ref *found_tab = find_hinted_table(hint_table, nest_tl);
      if (found_tab != nullptr) return found_tab;
    } else {
      if (!compare_table_name(hint_table, nest_tl)) return nest_tl;
    }
  }
  return nullptr;
}

/**
 Return tablemap of tables present within the Table_ref. If the table is an
 outer join, semijoin or antijoin, return the bitmap of all nested tables. If
 not, return the bitmap of the table.

 @param table Table for which bitmap(s) requested

 @return bitmap of all tables within this Table_ref
*/
static table_map get_table_map(const Table_ref *table) {
  return table->nested_join ? table->nested_join->used_tables : table->map();
}
/**
 Add a table to the specified list. If an order_list is specified, tables must
 be added maintaining the relative order in the order list.

 @param table             Table to be added to list
 @param exclude_table_map Bitmap of tables to be excluded from list
 @param order_table_map   Bitmap of tables in order-list
 @param order_list        List of required relative orders
 @param list              List to add table to
*/
static void add_table_to_list(Table_ref *table, table_map exclude_table_map,
                              table_map order_table_map,
                              mem_root_deque<Table_ref *> *order_list,
                              mem_root_deque<Table_ref *> *list) {
  table_map curr_table_map = get_table_map(table);
  if (Overlaps(curr_table_map, order_table_map)) {
    // if any JOIN_ORDER_HINT table is found, replace it with the
    // next table in the JOIN_ORDER_HINT order. This will ensure that
    // all the JOIN_ORDER_HINT tables are present in relative order.
    Table_ref *next_table_in_order = nullptr;
    if (!order_list->empty()) next_table_in_order = order_list->back();

    while (next_table_in_order &&
           Overlaps(get_table_map(next_table_in_order), exclude_table_map)) {
      order_list->pop_back();
      if (order_list->empty()) {
        next_table_in_order = nullptr;
      } else {
        next_table_in_order = order_list->back();
      }
    }
    if (next_table_in_order != nullptr) {
      order_list->pop_back();
      list->push_front(next_table_in_order);
    }
  } else if (!Overlaps(curr_table_map, exclude_table_map)) {
    list->push_front(table);
  }
  return;
}

/**
  Sorts tables from the join list to create a new join list which contains the
  tables in an order which complies with join order hints.

  @param thd           Thread handle.
  @param join_list     Deque of tables in join
  @param toplevel      False for subqueries, true otherwise

  @return Deque of tables sorted in reverse hinted order.
*/
const mem_root_deque<Table_ref *> *Opt_hints_qb::sort_tables_in_join_order(
    THD *thd, const mem_root_deque<Table_ref *> &join_list, bool toplevel) {
  assert(has_join_order_hints());

  if (join_list.size() <= 1) {
    return &join_list;
  }

  MEM_ROOT *mem_root_arg = thd->mem_root;
  mem_root_deque<Table_ref *> *new_join_list =
      new (mem_root_arg) mem_root_deque<Table_ref *>(mem_root_arg);

  mem_root_deque<Table_ref *> prefix_list(mem_root_arg),
      suffix_list(mem_root_arg), order_list(mem_root_arg);
  table_map prefix_table_map = 0, suffix_table_map = 0, order_table_map = 0;
  // Iterate through the hint table list for each hint and place the tables
  // into 3 lists: prefix_list, suffix_list and order_list
  for (uint hint_idx = 0; hint_idx < join_order_hints.size(); hint_idx++) {
    PT_qb_level_hint *hint = join_order_hints[hint_idx];
    Hint_param_table_list *hint_table_list = hint->get_table_list();
    if (hint_table_list->size() == 0) continue;

    mem_root_deque<Table_ref *> curr_order_list(mem_root_arg);
    table_map curr_order_table_map = 0;
    bool conflicting_hint = false;

    // Push all prefix/suffix tables backwards into join table list
    for (uint hint_tab_idx = 0; hint_tab_idx < hint_table_list->size();
         hint_tab_idx++) {
      if (conflicting_hint) break;

      const Hint_param_table &hint_table = (*hint_table_list)[hint_tab_idx];
      Table_ref *match = nullptr, *curr_tab = nullptr;
      for (auto it = join_list.rbegin(); it != join_list.rend(); ++it) {
        curr_tab = *it;
        match = find_hinted_table(&hint_table, curr_tab);
        if (match != nullptr) break;
      }
      if (match == nullptr) {
        if (toplevel) {
          // unknown table
          print_join_order_warn(thd, hint->type(), &hint_table);
          conflicting_hint = true;
        }
        continue;
      }
      table_map match_map = match->map();
      if (curr_tab->nested_join) {
        table_map already_sorted = prefix_table_map | suffix_table_map |
                                   order_table_map | curr_order_table_map;
        if (!Overlaps(already_sorted, curr_tab->nested_join->used_tables) &&
            (curr_tab->nested_join->m_tables.size() > 1)) {
          // sort inner table list according to hints
          const mem_root_deque<Table_ref *> *nested_table_list =
              sort_tables_in_join_order(thd, curr_tab->nested_join->m_tables);
          if (nested_table_list != &curr_tab->nested_join->m_tables) {
            curr_tab->nested_join->m_tables.clear();
            curr_tab->nested_join->m_tables = *nested_table_list;
          }
        }
        match = curr_tab;
        match_map = curr_tab->nested_join->used_tables;

        if (hint->type() == JOIN_PREFIX_HINT_ENUM && prefix_list.empty()) {
          conflicting_hint = true;
          push_warning_printf(thd, Sql_condition::SL_WARNING,
                              ER_WARN_UNSUPPORTED_HINT,
                              ER_THD(thd, ER_WARN_UNSUPPORTED_HINT),
                              "hypergraph: first table in join order cannot be "
                              "semijoin/antijoin nest");
          continue;
        }
      }
      table_map hinted_table_map = prefix_table_map | suffix_table_map;
      if (Overlaps(hinted_table_map, match_map)) {
        // table already hinted, ignore and add remaining hinted tables
        // for best-effort matching of multiple hints
        continue;
      }
      mem_root_deque<Table_ref *> *selected_list = nullptr;
      table_map *selected_map = nullptr;
      switch (hint->type()) {
        case JOIN_PREFIX_HINT_ENUM:
          if (Overlaps(order_table_map, match_map)) {
            Table_ref *prefix_tab = nullptr, *order_tab = *order_list.rbegin();
            auto order_it = order_list.rbegin();
            // Find first table in order list which does not match a prefix
            // table
            for (auto prefix_it = prefix_list.rbegin();
                 order_it != order_list.rend() &&
                 prefix_it != prefix_list.rend();
                 ++order_it, ++prefix_it) {
              order_tab = *order_it;
              prefix_tab = *prefix_it;
              if (order_tab != prefix_tab) break;
            }

            if (*order_it != match) {
              conflicting_hint = true;
              continue;
            }
          }
          selected_list = &prefix_list;
          selected_map = &prefix_table_map;
          break;
        case JOIN_SUFFIX_HINT_ENUM:
          if (Overlaps(order_table_map, match_map)) {
            if (hint_tab_idx == (hint_table_list->size() - 1)) {
              Table_ref *last_table_in_order = *order_list.begin();
              if (last_table_in_order != match) {
                conflicting_hint = true;
                continue;
              }
            } else {
              auto order_it = order_list.rbegin();
              for (; order_it != order_list.rend(); ++order_it) {
                Table_ref *order_tab = *order_it;
                if (order_tab == match) break;
              }
              for (; order_it != order_list.rend(); ++order_it) {
                Table_ref *order_tab = *order_it;
                for (auto suffix_it = suffix_list.rbegin();
                     suffix_it != suffix_list.rend(); ++suffix_it) {
                  // if any table in order_list *after* match matches an
                  // existing suffix table, then the suffix order is the
                  // reverse of the existing hinted join order
                  if (order_tab == *suffix_it) {
                    conflicting_hint = true;
                    break;
                  }
                }
                if (conflicting_hint) break;
              }
              if (conflicting_hint) continue;
            }
          }
          selected_list = &suffix_list;
          selected_map = &suffix_table_map;
          break;
        case JOIN_ORDER_HINT_ENUM:
          if (Overlaps(match_map, curr_order_table_map)) {
            // table already hinted, continue
            continue;
          }
          selected_list = &curr_order_list;
          selected_map = &curr_order_table_map;
          break;
        default:
          assert(false);
          break;
      }
      selected_list->push_front(match);
      (*selected_map) |= match_map;
    }
    if (conflicting_hint) {
      join_order_hints_ignored |= 1ULL << hint_idx;
      continue;
    }

    mem_root_deque<Table_ref *> merged_order_list(mem_root_arg);
    auto curr_it = curr_order_list.rbegin();
    // A query may have multiple JOIN_ORDER_HINTs. These individual orders
    // need to be combined if possible to create an order which fulfils all
    // the JOIN_ORDER_HINTs. For each JOIN_ORDER hint, a hint-specific table
    // list called curr_order_list is created. After adding all the tables in
    // the hint, the curr_order_list is merged with the combined order_list to
    // create a new order_list which reflects the orders of all the JOIN_ORDER
    // hints processed so far, including the current hint.
    if (Overlaps(order_table_map, curr_order_table_map)) {
      // multiple JOIN_ORDER hints may contain one or more
      // common tables.
      for (auto it = order_list.rbegin(); it != order_list.rend(); ++it) {
        if (conflicting_hint) break;
        Table_ref *tl = *it;
        if (!Overlaps(get_table_map(tl), curr_order_table_map))
          merged_order_list.push_front(tl);
        else {
          for (; curr_it != curr_order_list.rend(); ++curr_it) {
            Table_ref *curr_tl = *curr_it;
            if (Overlaps(get_table_map(curr_tl), order_table_map) &&
                !Overlaps(get_table_map(curr_tl), get_table_map(tl))) {
              // same tables as previous hint, but in different order
              // ignore this hint
              merged_order_list = order_list;
              conflicting_hint = true;
              break;
            }
            merged_order_list.push_front(curr_tl);
            if (Overlaps(get_table_map(curr_tl), get_table_map(tl))) {
              ++curr_it;
              break;
            }
          }
        }
      }
      if (conflicting_hint) {
        join_order_hints_ignored |= 1ULL << hint_idx;
        continue;
      }
    } else {
      // No overlapping tables
      merged_order_list = order_list;
    }
    // Append the current order list to the merged list.
    for (; curr_it != curr_order_list.rend(); ++curr_it) {
      Table_ref *curr_tl = *curr_it;
      merged_order_list.push_front(curr_tl);
    }
    order_list = merged_order_list;
    order_table_map |= curr_order_table_map;
  }

  // Now that the lists are in place, the new join order is constructed
  // 1. Add all prefix tables.
  // 2. Add all non-prefix and non-suffix tables while handling relative
  //    table orders specified in order_list.
  // 3. Add all suffix tables.
  for (auto it = prefix_list.rbegin(); it != prefix_list.rend(); ++it) {
    Table_ref *tl = *it;
    new_join_list->push_front(tl);
  }
  table_map exclude_table_map = prefix_table_map | suffix_table_map;
  for (auto it = join_list.rbegin(); it != join_list.rend(); ++it) {
    Table_ref *tl = *it;
    add_table_to_list(tl, exclude_table_map, order_table_map, &order_list,
                      new_join_list);
  }
  for (auto it = suffix_list.rbegin(); it != suffix_list.rend(); ++it) {
    Table_ref *tl = *it;
    new_join_list->push_front(tl);
  }
  table_map prior_map = 0;
  for (auto it = new_join_list->rbegin(); it != new_join_list->rend(); ++it) {
    Table_ref *tl = *it;
    if (tl->straight || tl->outer_join) {
      if (!IsSubset(tl->dep_tables, prior_map)) {
        // hinted order conflicts with OUTER JOIN directive, silently ignore
        // hints by reverting to original join order.
        return &join_list;
      }
    } else if (tl->is_sj_or_aj_nest()) {
      table_map outer_table_map = tl->nested_join->sj_depends_on;
      if (!IsSubset(outer_table_map, prior_map)) {
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_WARN_UNSUPPORTED_HINT,
                            ER_THD(thd, ER_WARN_UNSUPPORTED_HINT),
                            "hypergraph: semijoin/antijoin table precedes a"
                            " table which it depends on");
        return &join_list;
      }
    }
    prior_map |= get_table_map(tl);
  }
  return new_join_list;
}

/**
  Checks if a combination of left and right RelationalExpressions satisfy
  one or more join order hints. If the proposed join does not satisfy a hint,
  the remaining hints are ignored.

  @param left          Left side of proposed join
  @param right         Right side of proposed join
  @param join_list     Deque of tables in join

  @return true if at least one hint matches, false otherwise
*/
bool Opt_hints_qb::check_join_order_hints(
    RelationalExpression *left, RelationalExpression *right,
    const mem_root_deque<Table_ref *> *join_list) {
  if (!has_join_order_hints()) return true;
  if (right->type != RelationalExpression::TABLE)
    return false;  // right-deep or bushy plan

  uint matched_hints = 0;
  for (uint hint_idx = 0; hint_idx < join_order_hints.size(); hint_idx++) {
    if (join_order_hints_ignored & (1ULL << hint_idx)) continue;
    PT_qb_level_hint *hint = join_order_hints[hint_idx];
    if (hinted_join_order(hint, left, right, join_list)) matched_hints++;
  }
  return matched_hints > 0;
}

/**
  Checks if a combination of left and right RelationalExpressions satisfy
  a hint.

  @param hint          Hint info
  @param left          Left side of proposed join
  @param right         Right side of proposed join
  @param join_list     Deque of tables in join

  @return true if at least one hint matches, false otherwise
*/
bool Opt_hints_qb::hinted_join_order(
    PT_qb_level_hint *hint, RelationalExpression *left,
    RelationalExpression *right, const mem_root_deque<Table_ref *> *join_list) {
  if (right->type != RelationalExpression::TABLE)
    return false;  // right-deep or bushy plan

  Hint_param_table_list *hint_table_list = hint->get_table_list();
  if (hint_table_list->size() == 0) return false;

  // Special case where both 'left' and 'right' are TABLEs
  if (left->type == RelationalExpression::TABLE) {
    uint hint_tbl_idx = 0;
    const bool left_matched =
        hint_table_list->size() > hint_tbl_idx &&
        !compare_table_name(&(*hint_table_list)[hint_tbl_idx], left->table);
    hint_tbl_idx += (left_matched == true);
    const bool right_matched =
        hint_table_list->size() > hint_tbl_idx &&
        !compare_table_name(&(*hint_table_list)[hint_tbl_idx], right->table);
    switch (hint->type()) {
      case JOIN_PREFIX_HINT_ENUM:
        return left_matched && (right_matched || hint_table_list->size() == 1);
      case JOIN_SUFFIX_HINT_ENUM:
        return right_matched;
      case JOIN_ORDER_HINT_ENUM:
        return left_matched && right_matched;
      default:
        assert(false);
        break;
    }
    return false;
  }
  table_map hint_tab_map = 0;
  uint last_hint_tab_in_left = 0;
  for (uint hint_tbl_idx = 0; hint_tbl_idx < hint_table_list->size();
       hint_tbl_idx++) {
    const Hint_param_table &hint_table = (*hint_table_list)[hint_tbl_idx];
    for (auto it = join_list->rbegin(); it != join_list->rend();
         ++it) {  // The list goes backwards.
      const Table_ref *tl = *it;
      Table_ref *match = find_hinted_table(&hint_table, tl);
      if (match != nullptr) {
        table_map match_map = get_table_map(tl);
        hint_tab_map |= match_map;
        if (Overlaps(left->tables_in_subtree, match_map))
          last_hint_tab_in_left = hint_tbl_idx;
        break;
      }
    }
  }
  bool left_hinted =
      hinted_join_order(hint, left->left, left->right, join_list);
  switch (hint->type()) {
    case JOIN_PREFIX_HINT_ENUM: {
      if (!left_hinted) return false;
      if (IsSubset(hint_tab_map, left->tables_in_subtree)) {
        // Hint matches if prefix fully contained in left expr. Non-prefix
        // tables can be present after prefix tables.
        // E.g. left = {a, b, c, d}, prefix = {a, b, c}
        return true;
      } else if (Overlaps(left->tables_in_subtree, hint_tab_map) &&
                 Overlaps(right->table->map(), hint_tab_map)) {
        if (!left_hinted) return false;
        // hint split across left and right expressions
        uint next_prefix_tab = last_hint_tab_in_left + 1;
        if (next_prefix_tab >= hint_table_list->size()) {
          return false;
        }
        if (!compare_table_name(&((*hint_table_list)[next_prefix_tab]),
                                right->table)) {
          return true;
        }
      }
      return false;
      break;
    }
    case JOIN_SUFFIX_HINT_ENUM: {
      if (!left_hinted) {
        if (!compare_table_name(&((*hint_table_list)[0]), right->table)) {
          // right expr contains first suffix table
          return true;
        } else if (!Overlaps(right->table->map(), hint_tab_map)) {
          // Hint broken if right table not present
          return false;
        }
      } else if (Overlaps(left->tables_in_subtree, hint_tab_map) &&
                 Overlaps(right->table->map(), hint_tab_map)) {
        // hint split across left and right expressions
        uint next_suffix_tab = last_hint_tab_in_left + 1;
        if (next_suffix_tab >= hint_table_list->size()) {
          return false;
        }
        if (!compare_table_name(&((*hint_table_list)[next_suffix_tab]),
                                right->table)) {
          return true;
        }
      }
      return false;
      break;
    }
    case JOIN_ORDER_HINT_ENUM: {
      if (!Overlaps(left->tables_in_subtree, hint_tab_map) &&
          !Overlaps(right->table->map(), hint_tab_map)) {
        // JOIN_ORDER tables not present in either left or right
        return false;
      } else if (left_hinted &&
                 IsSubset(hint_tab_map, left->tables_in_subtree)) {
        // All join order tables present in order in left expr, right expr is
        // not part of hint
        return false;
      } else if (Overlaps(left->tables_in_subtree, hint_tab_map) &&
                 Overlaps(right->table->map(), hint_tab_map)) {
        // hint split across left and right expressions - get all tables which
        // should precede right->table in hinted join order
        table_map prior_map = 0;
        for (uint hint_tbl_idx = 0; hint_tbl_idx <= last_hint_tab_in_left;
             hint_tbl_idx++) {
          const Hint_param_table &hint_table = (*hint_table_list)[hint_tbl_idx];
          for (auto it = join_list->rbegin(); it != join_list->rend();
               ++it) {  // The list goes backwards.
            const Table_ref *tl = *it;
            if (find_hinted_table(&hint_table, tl)) {
              prior_map |= get_table_map(tl);
              break;
            }
          }
        }
        // check that right expr does not contain a table which should
        // precede any of the left expr tables in the hinted order
        if (Overlaps(right->table->map(), prior_map)) return false;
        return true;
      }
      break;
    }
    default:
      break;
  }
  return false;
}

void Opt_hints_qb::apply_join_order_hints(JOIN *join) {
  for (uint hint_idx = 0; hint_idx < join_order_hints.size(); hint_idx++) {
    PT_qb_level_hint *hint = join_order_hints[hint_idx];
    Hint_param_table_list *hint_table_list = hint->get_table_list();
    if (set_join_hint_deps(join, hint_table_list, hint->type()))
      //  Skip hint printing in EXPLAIN message.
      join_order_hints_ignored |= 1ULL << hint_idx;
  }
}

void Opt_hints_table::adjust_key_hints(Table_ref *tr) {
  set_resolved();
  if (child_array_ptr()->size() == 0)  // No key level hints
  {
    get_parent()->incr_resolved_children();
    return;
  }

  /*
    Make sure that adjustment is done only once.
    Table has already been processed if keyinfo_array is not empty.
  */
  if (keyinfo_array.size()) return;

  // Names of keys are not known for
  // derived/internal temp/table_function tables.
  if (!tr->is_base_table()) return;

  TABLE *table = tr->table;
  if (keyinfo_array.resize(table->s->keys, nullptr)) {
    return;
  }

  for (Opt_hints **hint = child_array_ptr()->begin();
       hint < child_array_ptr()->end(); ++hint) {
    KEY *key_info = table->key_info;
    for (uint j = 0; j < table->s->keys; j++, key_info++) {
      const LEX_CSTRING key_name = {key_info->name, strlen(key_info->name)};
      if (!cmp_lex_string(*(*hint)->get_name(), key_name,
                          system_charset_info)) {
        (*hint)->set_resolved();
        keyinfo_array[j] = static_cast<Opt_hints_key *>(*hint);
        incr_resolved_children();
        set_compound_key_hint_map(*hint, j);
      }
    }
  }

  /*
   Do not increase number of resolved tables
   if there are unresolved key objects. It's
   important for check_unresolved() function.
  */
  if (is_all_resolved()) get_parent()->incr_resolved_children();
}

bool is_compound_hint(opt_hints_enum type_arg) {
  return (
      type_arg == INDEX_MERGE_HINT_ENUM || type_arg == SKIP_SCAN_HINT_ENUM ||
      type_arg == INDEX_HINT_ENUM || type_arg == JOIN_INDEX_HINT_ENUM ||
      type_arg == GROUP_INDEX_HINT_ENUM || type_arg == ORDER_INDEX_HINT_ENUM);
}

PT_hint *Opt_hints_table::get_complex_hints(opt_hints_enum type) {
  assert(is_compound_hint(type));
  return get_compound_key_hint(type)->get_pt_hint();
}

bool Opt_hints_table::is_hint_conflicting(Opt_hints_key *key_hint,
                                          opt_hints_enum type) {
  if ((key_hint == nullptr) && is_specified(type)) return true;
  return (key_hint && key_hint->is_specified(type));
}

/**
  Function updates key_to_use key map depending on index hint state.

  @param keys_to_use            key to use
  @param available_keys_to_use  available keys to use
  @param type_arg               hint type
*/

void Opt_hints_table::update_index_hint_map(Key_map *keys_to_use,
                                            Key_map *available_keys_to_use,
                                            opt_hints_enum type_arg) {
  // Check if hint is resolved.
  if (is_resolved(type_arg)) {
    Key_map *keys_specified_in_hint =
        get_compound_key_hint(type_arg)->get_key_map();
    if (get_switch(type_arg)) {
      // If the hint is on and no keys are specified in the hint,
      // then set "keys_to_use" to all the available keys.
      if (keys_specified_in_hint->is_clear_all())
        keys_to_use->merge(*available_keys_to_use);
      // If hint is on and there are keys specified in the hint, then add
      // the specified keys to "keys_to_use" taking care of the disabled keys
      // (available_keys_to_use).
      else {
        keys_to_use->merge(*keys_specified_in_hint);
        keys_to_use->intersect(*available_keys_to_use);
      }
    } else {
      // If hint is off and there are no keys specified in the hint, then
      // we clear "keys_to_use".
      if (keys_specified_in_hint->is_clear_all()) keys_to_use->clear_all();
      // If hint is off and some keys are specified in the hint, then remove
      // the specified keys from "keys_to_use.
      else
        keys_to_use->subtract(*keys_specified_in_hint);
    }
  }
}

/**
  Function updates keys_in_use_for_query, keys_in_use_for_group_by,
  keys_in_use_for_order_by depending on INDEX, JOIN_INDEX, GROUP_INDEX,
  ORDER_INDEX hints.

  @param thd            pointer to THD object
  @param tbl            pointer to TABLE object

  @return false if no index hint is specified, true otherwise.
*/

bool Opt_hints_table::update_index_hint_maps(THD *thd, TABLE *tbl) {
  if (!is_resolved(INDEX_HINT_ENUM) && !is_resolved(JOIN_INDEX_HINT_ENUM) &&
      !is_resolved(GROUP_INDEX_HINT_ENUM) &&
      !is_resolved(ORDER_INDEX_HINT_ENUM))
    return false;  // No index hint is specified

  Key_map usable_index_map(tbl->s->usable_indexes(thd));
  tbl->keys_in_use_for_query = tbl->keys_in_use_for_group_by =
      tbl->keys_in_use_for_order_by = usable_index_map;

  const bool force_index = is_force_index_hint(INDEX_HINT_ENUM);
  tbl->force_index = (force_index || is_force_index_hint(JOIN_INDEX_HINT_ENUM));
  tbl->force_index_group =
      (force_index || is_force_index_hint(GROUP_INDEX_HINT_ENUM));
  tbl->force_index_order =
      (force_index || is_force_index_hint(ORDER_INDEX_HINT_ENUM));

  if (tbl->force_index || tbl->force_index_group || tbl->force_index_order) {
    tbl->keys_in_use_for_query.clear_all();
    tbl->keys_in_use_for_group_by.clear_all();
    tbl->keys_in_use_for_order_by.clear_all();
  }

  update_index_hint_map(&tbl->keys_in_use_for_query, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_group_by, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_order_by, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_query, &usable_index_map,
                        JOIN_INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_group_by, &usable_index_map,
                        GROUP_INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_order_by, &usable_index_map,
                        ORDER_INDEX_HINT_ENUM);
  /* Make sure "covering_keys" does not include indexes disabled with a hint */
  Key_map covering_keys(tbl->keys_in_use_for_query);
  covering_keys.merge(tbl->keys_in_use_for_group_by);
  covering_keys.merge(tbl->keys_in_use_for_order_by);
  tbl->covering_keys.intersect(covering_keys);
  return true;
}

/**
  Function checks if INDEX hint is conflicting with
  already specified JOIN_INDEX, GROUP_INDEX, ORDER_INDEX
  hints.

  @param table_hint         pointer to table hint
  @param key_hint           pointer to key hint

  @return false if no conflict, true otherwise.
*/

bool Glob_index_key_hint::is_hint_conflicting(Opt_hints_table *table_hint,
                                              Opt_hints_key *key_hint) {
  return (table_hint->is_hint_conflicting(key_hint, JOIN_INDEX_HINT_ENUM) ||
          table_hint->is_hint_conflicting(key_hint, GROUP_INDEX_HINT_ENUM) ||
          table_hint->is_hint_conflicting(key_hint, ORDER_INDEX_HINT_ENUM));
}

/**
  Function checks if JOIN_INDEX|GROUP_INDEX|ORDER_INDEX
  hint is conflicting with already specified INDEX hint.

  @param table_hint         pointer to table hint
  @param key_hint           pointer to key hint

  @return false if no conflict, true otherwise.
*/

bool Index_key_hint::is_hint_conflicting(Opt_hints_table *table_hint,
                                         Opt_hints_key *key_hint) {
  return table_hint->is_hint_conflicting(key_hint, INDEX_HINT_ENUM);
}

/**
  Function prints hint using the info from set_var variable.

  @param thd            Thread handle
  @param str            Pointer to string object
  @param var            Pointer to set_var object
*/

static void print_hint_from_var(const THD *thd, String *str, set_var *var) {
  str->append(STRING_WITH_LEN("SET_VAR("));
  var->print_short(thd, str);
  str->append(STRING_WITH_LEN(") "));
}

/**
  Function prints hint as it is specified.

  @param str            Pointer to string object
  @param sys_var_name   Variable name
  @param sys_var_value  Variable value
*/

static void print_hint_specified(String *str, const std::string &sys_var_name,
                                 Item *sys_var_value) {
  str->append(STRING_WITH_LEN("SET_VAR("));
  str->append(sys_var_name);
  str->append(STRING_WITH_LEN("="));
  char buff[STRING_BUFFER_USUAL_SIZE];
  String str_buff(buff, sizeof(buff), system_charset_info), *str_res;
  str_res = sys_var_value->val_str(&str_buff);
  if (sys_var_value->result_type() == STRING_RESULT) {
    str->append(STRING_WITH_LEN("'"));
    str->append(str_res->ptr(), str_res->length());
    str->append(STRING_WITH_LEN("'"));
  } else if (sys_var_value->result_type() == INT_RESULT)
    str->append(str_res->ptr(), str_res->length());
  str->append(STRING_WITH_LEN(") "));
}

bool Sys_var_hint::add_var(THD *thd, const System_variable_tracker &var_tracker,
                           Item *sys_var_value) {
  for (uint i = 0; i < var_list.size(); i++) {
    const Hint_set_var *hint_var = var_list[i];
    set_var *var = hint_var->var;
    std::string existent_name{var->m_var_tracker.get_var_name()};
    std::string new_name{var_tracker.get_var_name()};
    /*
      Issue a warning if system variable is already present in hint list.
    */
    if (!cmp_lex_string(
            LEX_CSTRING{existent_name.c_str(), existent_name.size()},
            LEX_CSTRING{new_name.c_str(), new_name.size()},
            system_charset_info)) {
      String str;
      print_hint_specified(&str, existent_name, sys_var_value);
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_WARN_CONFLICTING_HINT,
          ER_THD(thd, ER_WARN_CONFLICTING_HINT), str.c_ptr_safe());
      return false;
    }
  }

  set_var *var =
      new (thd->mem_root) set_var(OPT_SESSION, var_tracker, sys_var_value);
  if (!var) return true;

  Hint_set_var *hint_var = new (thd->mem_root) Hint_set_var(var);
  if (!hint_var) return true;

  return var_list.push_back(hint_var);
}

void Sys_var_hint::update_vars(THD *thd) {
  // Skip SET_VAR hint applying on the slave.
  if (thd->slave_thread) return;

  Set_var_error_handler error_handler(false);
  for (uint i = 0; i < var_list.size(); i++) {
    thd->push_internal_handler(&error_handler);
    Hint_set_var *hint_var = var_list[i];
    set_var *var = hint_var->var;
    if (!var->resolve(thd) && !var->check(thd)) {
      auto f = [thd](const System_variable_tracker &, sys_var *v) -> Item * {
        return v->copy_value(thd);
      };
      Item *save_value =
          var->m_var_tracker.access_system_variable<Item *>(thd, f).value_or(
              nullptr);
      if (!var->update(thd)) hint_var->save_value = save_value;
    }
    thd->pop_internal_handler();
    error_handler.reset_state();
  }
}

void Sys_var_hint::restore_vars(THD *thd) {
  Set_var_error_handler error_handler(true);
  thd->push_internal_handler(&error_handler);
  for (uint i = 0; i < var_list.size(); i++) {
    Hint_set_var *hint_var = var_list[i];
    set_var *var = hint_var->var;
    if (hint_var->save_value) {
      /* Restore original value for update */
      std::swap(var->value, hint_var->save_value);
      /*
        There should be no error since original value is restored.
      */
#ifndef NDEBUG
      assert(!var->check(thd));
      assert(!var->update(thd));
#else
      (void)var->check(thd);
      (void)var->update(thd);
#endif
      /* Restore hint value for further executions */
      std::swap(var->value, hint_var->save_value);
    }
  }
  thd->pop_internal_handler();
}

void Sys_var_hint::print(const THD *thd, String *str) {
  for (uint i = 0; i < var_list.size(); i++) {
    Hint_set_var *hint_var = var_list[i];
    if (hint_var->save_value) print_hint_from_var(thd, str, hint_var->var);
  }
}

/**
  Function returns hint value depending on
  the specified hint level. If hint is specified
  on current level, current level hint value is
  returned, otherwise parent level hint is checked.

  @param hint              Pointer to the hint object
  @param parent_hint       Pointer to the parent hint object,
                           should never be NULL
  @param type_arg          hint type
  @param [out] ret_val     hint value depending on
                           what hint level is used

  @return true if hint is specified, false otherwise
*/

static bool get_hint_state(Opt_hints *hint, Opt_hints *parent_hint,
                           opt_hints_enum type_arg, bool *ret_val) {
  assert(parent_hint);

  if (opt_hint_info[type_arg].switch_hint) {
    if (hint && hint->is_specified(type_arg)) {
      *ret_val = hint->get_switch(type_arg);
      return true;
    }
    if (opt_hint_info[type_arg].check_upper_lvl &&
        parent_hint->is_specified(type_arg)) {
      *ret_val = parent_hint->get_switch(type_arg);
      return true;
    }
  } else {
    if (hint && hint->is_specified(type_arg) &&
        /*
          This check is necessary because function idx_merge_key_enabled()
          can operate only with resolved hint. For unresolved hint function,
          idx_merge_key_enabled() always returns 'true' to emulate absence
          of the hint.
        */
        hint->is_resolved(type_arg)) {
      *ret_val = hint->get_switch(type_arg);
      return true;
    }
  }
  return false;
}

bool hint_key_state(const THD *thd, const Table_ref *table, uint keyno,
                    opt_hints_enum type_arg, uint optimizer_switch) {
  Opt_hints_table *table_hints = table->opt_hints_table;

  /* Parent should always be initialized */
  if (table_hints && keyno != MAX_KEY) {
    Opt_hints_key *key_hints = table_hints->keyinfo_array.size() > 0
                                   ? table_hints->keyinfo_array[keyno]
                                   : nullptr;
    bool ret_val = false;
    if (get_hint_state(key_hints, table_hints, type_arg, &ret_val))
      return ret_val;
  }

  return thd->optimizer_switch_flag(optimizer_switch);
}

bool hint_table_state(const THD *thd, const Table_ref *table_list,
                      opt_hints_enum type_arg, uint optimizer_switch) {
  if (table_list->opt_hints_qb) {
    bool ret_val = false;
    if (get_hint_state(table_list->opt_hints_table, table_list->opt_hints_qb,
                       type_arg, &ret_val))
      return ret_val;
  }

  return thd->optimizer_switch_flag(optimizer_switch);
}

void append_table_name(const THD *thd, String *str, const LEX_CSTRING *qb_name,
                       const LEX_CSTRING *table_name) {
  /* Append table name */
  append_identifier(thd, str, table_name->str, table_name->length);

  /* Append QB name */
  if (qb_name && qb_name->length > 0) {
    str->append(STRING_WITH_LEN("@"));
    append_identifier(thd, str, qb_name->str, qb_name->length);
  }
}

bool compound_hint_key_enabled(const TABLE *table, uint keyno,
                               opt_hints_enum type_arg) {
  Opt_hints_table *table_hints = table->pos_in_table_list->opt_hints_table;

  if (table_hints && table_hints->is_resolved(type_arg)) {
    if (table_hints->get_compound_key_hint(type_arg)->is_key_map_clear_all())
      return table_hints->get_compound_key_hint(type_arg)
          ->get_pt_hint()
          ->switch_on();

    return table_hints->get_compound_key_hint(type_arg)->is_set_key_map(
               keyno) == table_hints->get_compound_key_hint(type_arg)
                             ->get_pt_hint()
                             ->switch_on();
  }
  return true;
}

bool idx_merge_hint_state(THD *thd, const TABLE *table,
                          bool *use_cheapest_index_merge) {
  const bool force_index_merge =
      hint_table_state(thd, table->pos_in_table_list, INDEX_MERGE_HINT_ENUM, 0);
  if (force_index_merge) {
    assert(table->pos_in_table_list->opt_hints_table);
    Opt_hints_table *table_hints = table->pos_in_table_list->opt_hints_table;
    /*
      If INDEX_MERGE hint is used without only specified index,
      cheapest index merge should be used.
    */
    *use_cheapest_index_merge = table_hints->index_merge.is_key_map_clear_all();
  }

  return force_index_merge;
}
