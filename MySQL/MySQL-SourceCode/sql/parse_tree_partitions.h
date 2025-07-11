/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#ifndef PARSE_TREE_PARTITIONS_INCLUDED
#define PARSE_TREE_PARTITIONS_INCLUDED

#include <sys/types.h>  // TODO: replace with cstdint

#include "lex_string.h"
#include "my_base.h"
#include "my_inttypes.h"  // TODO: replace with cstdint
#include "sql/parse_location.h"
#include "sql/parse_tree_helpers.h"
#include "sql/parse_tree_node_base.h"
#include "sql/partition_element.h"
#include "sql/partition_info.h"

class Item;
class THD;

template <class T>
class List;

template <typename Element_type>
class Mem_root_array;

/**
  Parse context for partitioning-specific parse tree nodes.

  For internal use in the contextualization code.

  @ingroup ptn_partitioning
*/
struct Partition_parse_context : public Parse_context,
                                 public Parser_partition_info {
  Partition_parse_context(THD *const thd, partition_info *part_info,
                          partition_element *current_partition,
                          partition_element *curr_part_elem,
                          bool is_add_or_reorganize_partition);

  Partition_parse_context(THD *thd_arg, partition_info *part_info_arg,
                          bool is_add_or_reorganize_partition)
      : Partition_parse_context(thd_arg, part_info_arg, nullptr, nullptr,
                                is_add_or_reorganize_partition) {}

  /**
    True for "ALTER TABLE ADD PARTITION" and "ALTER TABLE REORGANIZE PARTITION"
    statements, otherwise false.
  */
  const bool is_add_or_reorganize_partition;
};

/**
  Base class for all Partition_parse_context-dependent nodes

  @ingroup ptn_partitioning
*/
typedef Parse_tree_node_tmpl<Partition_parse_context> Parse_tree_part_node;

/**
  Base class for all partition options

  @ingroup ptn_part_options
*/
class PT_partition_option : public Parse_tree_part_node {
 protected:
  explicit PT_partition_option(const POS &pos) : Parse_tree_part_node(pos) {}
};

/**
  Node for the @SQL{COMMENT [=] @<string@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_comment : public PT_partition_option {
  typedef PT_partition_option super;

  char *comment;

 public:
  explicit PT_partition_comment(const POS &pos, char *comment)
      : super(pos), comment(comment) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->part_comment = comment;
    return false;
  }
};

/**
  Node for the @SQL{INDEX DIRECTORY [=] @<string@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_index_directory : public PT_partition_option {
  typedef PT_partition_option super;

  const char *index_directory;

 public:
  explicit PT_partition_index_directory(const POS &pos,
                                        const char *index_directory)
      : super(pos), index_directory(index_directory) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->index_file_name = index_directory;
    return false;
  }
};

/**
  Node for the @SQL{DATA DIRECTORY [=] @<string@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_data_directory : public PT_partition_option {
  typedef PT_partition_option super;

  const char *data_directory;

 public:
  explicit PT_partition_data_directory(const POS &pos,
                                       const char *data_directory)
      : super(pos), data_directory(data_directory) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->data_file_name = data_directory;
    return false;
  }
};

/**
  Node for the @SQL{MIN_ROWS [=] @<integer@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_min_rows : public PT_partition_option {
  typedef PT_partition_option super;

  ha_rows min_rows;

 public:
  explicit PT_partition_min_rows(const POS &pos, ha_rows min_rows)
      : super(pos), min_rows(min_rows) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->part_min_rows = min_rows;
    return false;
  }
};

/**
  Node for the @SQL{MAX_ROWS [=] @<integer@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_max_rows : public PT_partition_option {
  typedef PT_partition_option super;

  ha_rows max_rows;

 public:
  explicit PT_partition_max_rows(const POS &pos, ha_rows max_rows)
      : super(pos), max_rows(max_rows) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->part_max_rows = max_rows;
    return false;
  }
};

/**
  Node for the @SQL{NODEGROUP [=] @<integer@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_nodegroup : public PT_partition_option {
  typedef PT_partition_option super;

  uint16 nodegroup;

 public:
  explicit PT_partition_nodegroup(const POS &pos, uint16 nodegroup)
      : super(pos), nodegroup(nodegroup) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->nodegroup_id = nodegroup;
    return false;
  }
};

/**
  Node for the @SQL{[STORAGE] ENGINE [=] @<identifier|string@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_engine : public PT_partition_option {
  typedef PT_partition_option super;

 public:
  const LEX_CSTRING name;

  explicit PT_partition_engine(const POS &pos, const LEX_CSTRING &name)
      : super(pos), name(name) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    return resolve_engine(pc->thd, name, false,  // partition can't be temporary
                          false, &pc->curr_part_elem->engine_type);
  }
};

/**
  Node for the @SQL{TABLESPACE [=] @<identifier@>} partition option

  @ingroup ptn_part_options
*/
class PT_partition_tablespace : public PT_partition_option {
  typedef PT_partition_option super;

  const char *tablespace;

 public:
  explicit PT_partition_tablespace(const POS &pos, const char *tablespace)
      : super(pos), tablespace(tablespace) {}

  bool do_contextualize(Partition_parse_context *pc) override {
    if (super::do_contextualize(pc)) return true;

    pc->curr_part_elem->tablespace_name = tablespace;
    return false;
  }
};

/**
  Node for the @SQL{SUBRAPTITION} clause of @SQL{CREATE/ALTER TABLE}

  @ingroup ptn_partitioning
*/
class PT_subpartition : public Parse_tree_part_node {
  const POS m_errpos;
  const char *name;
  const Mem_root_array<PT_partition_option *> *options;

 public:
  PT_subpartition(const POS &pos, const POS &errpos, const char *name,
                  Mem_root_array<PT_partition_option *> *options)
      : Parse_tree_part_node(pos),
        m_errpos(errpos),
        name(name),
        options(options) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Base class for partition value nodes: @SQL{MAX_VALUE} values or expressions

  @ingroup ptn_partitioning
*/
class PT_part_value_item : public Parse_tree_part_node {
 protected:
  explicit PT_part_value_item(const POS &pos) : Parse_tree_part_node(pos) {}
};

/**
  Node for the @SQL{MAX_VALUE} partition value in @SQL{CREATE/ALTER TABLE}

  @ingroup ptn_partitioning
*/
class PT_part_value_item_max : public PT_part_value_item {
  typedef PT_part_value_item super;

 public:
  explicit PT_part_value_item_max(const POS &pos) : super(pos) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the partitioning expression in @SQL{CREATE/ALTER TABLE}

  @ingroup ptn_partitioning
*/
class PT_part_value_item_expr : public PT_part_value_item {
  typedef PT_part_value_item super;

  Item *expr;

 public:
  explicit PT_part_value_item_expr(const POS &pos, Item *expr)
      : super(pos), expr(expr) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Base class for @SQL{VALUES} partitioning clauses

  @ingroup ptn_partitioning
*/
class PT_part_values : public Parse_tree_part_node {
 protected:
  explicit PT_part_values(const POS &pos) : Parse_tree_part_node(pos) {}
};

/**
  Node for a list of partitioning values in @SQL{VALUES} clauses

  @ingroup ptn_partitioning
*/
class PT_part_value_item_list_paren : public PT_part_values {
  typedef PT_part_values super;

  Mem_root_array<PT_part_value_item *> *values;
  const POS paren_pos;

 public:
  explicit PT_part_value_item_list_paren(
      const POS &pos, Mem_root_array<PT_part_value_item *> *values,
      const POS &paren_pos)
      : super(pos), values(values), paren_pos(paren_pos) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for a list of partitioning values in the @SQL{VALUES IN} clause

  @ingroup ptn_partitioning
*/
class PT_part_values_in_item : public PT_part_values {
  typedef PT_part_values super;

  const POS m_errpos;
  PT_part_value_item_list_paren *item;

 public:
  explicit PT_part_values_in_item(const POS &pos, const POS &errpos,
                                  PT_part_value_item_list_paren *item)
      : super(pos), m_errpos(errpos), item(item) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for a list of partitioning values in the @SQL{VALUES IN} clause

  @ingroup ptn_partitioning
*/
class PT_part_values_in_list : public PT_part_values {
  typedef PT_part_values super;

  const POS m_errpos;
  Mem_root_array<PT_part_value_item_list_paren *> *list;

 public:
  explicit PT_part_values_in_list(
      const POS &pos, const POS &errpos,
      Mem_root_array<PT_part_value_item_list_paren *> *list)
      : super(pos), m_errpos(errpos), list(list) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION} clause of CREATE/ALTER TABLE

  @ingroup ptn_partitioning
*/
class PT_part_definition : public Parse_tree_part_node {
  typedef Parse_tree_part_node super;

  const POS m_errpos;
  const LEX_STRING name;
  partition_type type;
  PT_part_values *const opt_part_values;
  const POS values_pos;
  Mem_root_array<PT_partition_option *> *opt_part_options;
  Mem_root_array<PT_subpartition *> *opt_sub_partitions;
  const POS sub_partitions_pos;

 public:
  PT_part_definition(const POS &pos, const POS &errpos, const LEX_STRING &name,
                     partition_type type, PT_part_values *const opt_part_values,
                     const POS &values_pos,
                     Mem_root_array<PT_partition_option *> *opt_part_options,
                     Mem_root_array<PT_subpartition *> *opt_sub_partitions,
                     const POS &sub_partitions_pos)
      : super(pos),
        m_errpos(errpos),
        name(name),
        type(type),
        opt_part_values(opt_part_values),
        values_pos(values_pos),
        opt_part_options(opt_part_options),
        opt_sub_partitions(opt_sub_partitions),
        sub_partitions_pos(sub_partitions_pos) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Base class for all subpartitioning clause nodes

  @ingroup ptn_partitioning
*/
class PT_sub_partition : public Parse_tree_part_node {
 protected:
  explicit PT_sub_partition(const POS &pos) : Parse_tree_part_node(pos) {}
};

/**
  Node for the @SQL{SUBRAPTITION BY HASH} definition clause

  @ingroup ptn_partitioning
*/
class PT_sub_partition_by_hash : public PT_sub_partition {
  typedef PT_sub_partition super;

  const bool is_linear;
  const POS hash_pos;
  Item *hash;
  const uint opt_num_subparts;

 public:
  PT_sub_partition_by_hash(const POS &pos, bool is_linear, const POS &hash_pos,
                           Item *hash, uint opt_num_subparts)
      : super(pos),
        is_linear(is_linear),
        hash_pos(hash_pos),
        hash(hash),
        opt_num_subparts(opt_num_subparts) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{SUBRAPTITION BY KEY} definition clause

  @ingroup ptn_partitioning
*/
class PT_sub_partition_by_key : public PT_sub_partition {
  typedef PT_sub_partition super;

  const bool is_linear;
  enum_key_algorithm key_algo;
  List<char> *field_names;
  const uint opt_num_subparts;

 public:
  PT_sub_partition_by_key(const POS &pos, bool is_linear,
                          enum_key_algorithm key_algo, List<char> *field_names,
                          const uint opt_num_subparts)
      : super(pos),
        is_linear(is_linear),
        key_algo(key_algo),
        field_names(field_names),
        opt_num_subparts(opt_num_subparts) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

class PT_part_type_def : public Parse_tree_part_node {
 protected:
  explicit PT_part_type_def(const POS &pos) : Parse_tree_part_node(pos) {}

  bool set_part_field_list(Partition_parse_context *pc, List<char> *list);

  bool itemize_part_expr(Partition_parse_context *pc, const POS &pos,
                         Item **item);
};

/**
  Node for the @SQL{PARTITION BY [LINEAR] KEY} type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_key : public PT_part_type_def {
  typedef PT_part_type_def super;

  const bool is_linear;
  const enum_key_algorithm key_algo;
  List<char> *const opt_columns;

 public:
  PT_part_type_def_key(const POS &pos, bool is_linear,
                       enum_key_algorithm key_algo, List<char> *opt_columns)
      : super(pos),
        is_linear(is_linear),
        key_algo(key_algo),
        opt_columns(opt_columns) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION BY [LINEAR] HASH} type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_hash : public PT_part_type_def {
  typedef PT_part_type_def super;

  const bool is_linear;
  const POS expr_pos;
  Item *expr;

 public:
  PT_part_type_def_hash(const POS &pos, bool is_linear, const POS &expr_pos,
                        Item *expr)
      : super(pos), is_linear(is_linear), expr_pos(expr_pos), expr(expr) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION BY RANGE (@<expression@>) } type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_range_expr : public PT_part_type_def {
  typedef PT_part_type_def super;

  const POS expr_pos;
  Item *expr;

 public:
  PT_part_type_def_range_expr(const POS &pos, const POS &expr_pos, Item *expr)
      : super(pos), expr_pos(expr_pos), expr(expr) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION BY RANGE COLUMNS (@<ident list@>) } type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_range_columns : public PT_part_type_def {
  typedef PT_part_type_def super;

  List<char> *const columns;

 public:
  explicit PT_part_type_def_range_columns(const POS &pos, List<char> *columns)
      : super(pos), columns(columns) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION BY LIST (@<expression@>) } type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_list_expr : public PT_part_type_def {
  typedef PT_part_type_def super;

  const POS expr_pos;
  Item *expr;

 public:
  PT_part_type_def_list_expr(const POS &pos, const POS &expr_pos, Item *expr)
      : super(pos), expr_pos(expr_pos), expr(expr) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION BY LIST COLUMNS (@<ident list@>) } type clause

  @ingroup ptn_partitioning
*/
class PT_part_type_def_list_columns : public PT_part_type_def {
  typedef PT_part_type_def super;

  List<char> *const columns;

 public:
  explicit PT_part_type_def_list_columns(const POS &pos, List<char> *columns)
      : super(pos), columns(columns) {}

  bool do_contextualize(Partition_parse_context *pc) override;
};

/**
  Node for the @SQL{PARTITION} definition clause

  @ingroup ptn_partitioning
*/
class PT_partition : public Parse_tree_node {
  typedef Parse_tree_node super;

  PT_part_type_def *const part_type_def;
  const uint opt_num_parts;
  PT_sub_partition *const opt_sub_part;
  const POS part_defs_pos;
  Mem_root_array<PT_part_definition *> *part_defs;

 public:
  partition_info part_info;

 public:
  PT_partition(const POS &pos, PT_part_type_def *part_type_def,
               uint opt_num_parts, PT_sub_partition *opt_sub_part,
               const POS &part_defs_pos,
               Mem_root_array<PT_part_definition *> *part_defs)
      : super(pos),
        part_type_def(part_type_def),
        opt_num_parts(opt_num_parts),
        opt_sub_part(opt_sub_part),
        part_defs_pos(part_defs_pos),
        part_defs(part_defs) {}

  bool do_contextualize(Parse_context *pc) override;
};

#endif /* PARSE_TREE_PARTITIONS_INCLUDED */
