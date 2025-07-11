/* Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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

#include "sql/join_optimizer/access_path.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

#include "mem_root_deque.h"
#include "my_base.h"
#include "my_dbug.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "prealloced_array.h"
#include "sql/field.h"
#include "sql/filesort.h"
#include "sql/handler.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_subselect.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/bka_iterator.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/delete_rows_iterator.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/row_iterator.h"
#include "sql/iterators/sorting_iterator.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/iterators/window_iterators.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/estimate_selectivity.h"
#include "sql/join_optimizer/overflow_bitset.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/mem_root_array.h"
#include "sql/pack_rows.h"
#include "sql/range_optimizer/geometry_index_range_scan.h"
#include "sql/range_optimizer/group_index_skip_scan.h"
#include "sql/range_optimizer/group_index_skip_scan_plan.h"
#include "sql/range_optimizer/index_merge.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/index_skip_scan.h"
#include "sql/range_optimizer/index_skip_scan_plan.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/range_optimizer/reverse_index_range_scan.h"
#include "sql/range_optimizer/rowid_ordered_retrieval.h"
#include "sql/sql_array.h"
#include "sql/sql_const.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_update.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/visible_fields.h"
#include "template_utils.h"

using pack_rows::TableCollection;
using std::all_of;
using std::vector;

AccessPath *NewSortAccessPath(THD *thd, AccessPath *child, Filesort *filesort,
                              ORDER *order, bool count_examined_rows) {
  assert(child != nullptr);
  assert(filesort != nullptr);
  assert(order != nullptr);

  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::SORT;
  path->count_examined_rows = count_examined_rows;
  path->sort().child = child;
  path->sort().filesort = filesort;
  path->sort().order = order;
  path->sort().remove_duplicates = filesort->m_remove_duplicates;
  path->sort().unwrap_rollup = false;
  path->sort().limit = filesort->limit;
  path->sort().force_sort_rowids = !filesort->using_addon_fields();

  if (filesort->using_addon_fields()) {
    path->sort().tables_to_get_rowid_for = 0;
  } else {
    if (filesort->tables.size() == 1 &&
        filesort->tables[0]->pos_in_table_list == nullptr) {
      // This can happen if we sort a single temporary table
      // which is not in the table list (e.g., one that was
      // specifically created for us). Filesort has special-casing
      // to always get the row ID in this case.
      path->sort().tables_to_get_rowid_for = 0;
    } else {
      FindTablesToGetRowidFor(path);
    }
  }
  path->has_group_skip_scan = child->has_group_skip_scan;
  return path;
}

AccessPath *NewDeleteRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map delete_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, delete_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::DELETE_ROWS;
  path->delete_rows().child = child;
  path->delete_rows().tables_to_delete_from = delete_tables;
  path->delete_rows().immediate_tables = immediate_tables;
  return path;
}

AccessPath *NewUpdateRowsAccessPath(THD *thd, AccessPath *child,
                                    table_map update_tables,
                                    table_map immediate_tables) {
  assert(IsSubset(immediate_tables, update_tables));
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::UPDATE_ROWS;
  path->update_rows().child = child;
  path->update_rows().tables_to_update = update_tables;
  path->update_rows().immediate_tables = immediate_tables;
  return path;
}

static Mem_root_array<Item_values_column *> *GetTableValueConstructorOutputRefs(
    MEM_ROOT *mem_root, const JOIN *join) {
  // If the table value constructor has a single row, the values are contained
  // directly in join->fields, and there are no Item_values_column output refs.
  if (join->query_block->row_value_list->size() == 1) {
    return nullptr;
  }

  auto columns = new (mem_root) Mem_root_array<Item_values_column *>(mem_root);
  if (columns == nullptr) return nullptr;

  for (Item *column : VisibleFields(*join->fields)) {
    if (columns->push_back(down_cast<Item_values_column *>(column))) {
      return nullptr;
    }
  }

  return columns;
}

AccessPath *NewTableValueConstructorAccessPath(const THD *thd,
                                               const JOIN *join) {
  AccessPath *path = new (thd->mem_root) AccessPath;
  path->type = AccessPath::TABLE_VALUE_CONSTRUCTOR;
  // The iterator keeps track of which row it is at in examined_rows,
  // so we always need to give it the pointer.
  path->count_examined_rows = true;
  path->table_value_constructor().output_refs =
      GetTableValueConstructorOutputRefs(thd->mem_root, join);
  return path;
}

static AccessPath *FindSingleAccessPathOfType(AccessPath *path,
                                              AccessPath::Type type) {
  AccessPath *found_path = nullptr;

  auto func = [type, &found_path](AccessPath *subpath, const JOIN *) {
#ifdef NDEBUG
    constexpr bool fast_exit = true;
#else
    constexpr bool fast_exit = false;
#endif
    if (subpath->type == type) {
      assert(found_path == nullptr);
      found_path = subpath;
      // If not in debug mode, stop as soon as we find the first one.
      if (fast_exit) {
        return true;
      }
    }
    return false;
  };
  // Our users generally want to stop at STREAM or MATERIALIZE nodes,
  // since they are table-oriented and those nodes have their own tables.
  WalkAccessPaths(path, /*join=*/nullptr,
                  WalkAccessPathPolicy::STOP_AT_MATERIALIZATION, func);
  return found_path;
}

static RowIterator *FindSingleIteratorOfType(AccessPath *path,
                                             AccessPath::Type type) {
  AccessPath *found_path = FindSingleAccessPathOfType(path, type);
  if (found_path == nullptr) {
    return nullptr;
  } else {
    return found_path->iterator->real_iterator();
  }
}

TABLE *GetBasicTable(const AccessPath *path) {
  switch (path->type) {
    // Basic access paths (those with no children, at least nominally).
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table;
    case AccessPath::SAMPLE_SCAN:
      return path->sample_scan().table;
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table;
    case AccessPath::INDEX_DISTANCE_SCAN:
      return path->index_distance_scan().table;
    case AccessPath::REF:
      return path->ref().table;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().table;
    case AccessPath::EQ_REF:
      return path->eq_ref().table;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().table;
    case AccessPath::FULL_TEXT_SEARCH:
      return path->full_text_search().table;
    case AccessPath::CONST_TABLE:
      return path->const_table().table;
    case AccessPath::MRR:
      return path->mrr().table;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table;
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table;
    case AccessPath::INDEX_MERGE:
      return path->index_merge().table;
    case AccessPath::ROWID_INTERSECTION:
      return path->rowid_intersection().table;
    case AccessPath::ROWID_UNION:
      return path->rowid_union().table;
    case AccessPath::INDEX_SKIP_SCAN:
      return path->index_skip_scan().table;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return path->group_index_skip_scan().table;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return path->dynamic_index_range_scan().table;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
    case AccessPath::UNQUALIFIED_COUNT:

    // Note, some other AccessPaths may use its own temporary (derived) table.
    // We intentionally do not return such TABLEs.
    default:
      return nullptr;
  }
}

table_map GetUsedTableMap(const AccessPath *path, bool include_pruned_tables) {
  table_map tmap = 0;
  WalkTablesUnderAccessPath(
      const_cast<AccessPath *>(path),
      [&tmap](TABLE *table) {
        if (table->pos_in_table_list == nullptr) {
          // Materialization within a JOIN (e.g., for sorting). The table won't
          // have a map, so the caller will need to find the table manually.
          tmap |= RAND_TABLE_BIT;
        } else {
          tmap |= table->pos_in_table_list->map();
        }
        return false;
      },
      include_pruned_tables);
  return tmap;
}

static Prealloced_array<TABLE *, 4> GetUsedTables(AccessPath *child,
                                                  bool include_pruned_tables) {
  Prealloced_array<TABLE *, 4> tables{PSI_NOT_INSTRUMENTED};
  WalkTablesUnderAccessPath(
      child,
      [&tables](TABLE *table) {
        tables.push_back(table);
        return false;
      },
      include_pruned_tables);
  return tables;
}

Mem_root_array<TABLE *> CollectTables(THD *thd, AccessPath *root_path) {
  Mem_root_array<TABLE *> tables(thd->mem_root);
  WalkTablesUnderAccessPath(
      root_path, [&tables](TABLE *table) { return tables.push_back(table); },
      /*include_pruned_tables=*/true);
  return tables;
}

namespace {

/**
  Collect all the single-row index lookups that are located below the given path
  with no intermediate materialization step in between, and which cache the
  result of the index lookup.

  These are used by iterators that may overwrite the contents of
  table->record[0] in a way that disturbs EQRefIterator's cache, and which
  therefore need to mark the cache as invalid to force the next read from the
  EQRefIterator to read again from the index. Examples of iterators that may
  disturb EQRefIterator's cache include AggregateIterator, SortingIterator,
  HashJoinIterator and BKAIterator.
 */
std::span<AccessPath *> CollectSingleRowIndexLookups(THD *thd,
                                                     AccessPath *root) {
  Mem_root_array<AccessPath *> lookups(thd->mem_root);
  WalkAccessPaths(root, /*join=*/nullptr,
                  WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                  [&lookups](AccessPath *path, const JOIN *) {
                    if (path->type == AccessPath::EQ_REF &&
                        !path->eq_ref().ref->disable_cache) {
                      return lookups.push_back(path);
                    }
                    return false;
                  });
  return {lookups};
}

// Mirrors QEP_TAB::pfs_batch_update(), with one addition:
// If there is more than one table, batch mode will be handled by the join
// iterators on the probe side, so joins will return false.
bool ShouldEnableBatchMode(AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_DISTANCE_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:
      if (path->filter().condition->has_subquery()) {
        return false;
      } else {
        return ShouldEnableBatchMode(path->filter().child);
      }
    case AccessPath::SORT:
      return ShouldEnableBatchMode(path->sort().child);
    case AccessPath::EQ_REF:
    case AccessPath::CONST_TABLE:
      // These can read only one row per scan, so batch mode will never be a
      // win (fall through).
    default:
      // All others, in particular joins.
      return false;
  }
}

// Check if a subquery present in a condition has forced materialization.
bool IsForcedMaterialization(THD *thd, Item *cond) {
  bool force_materialization = false;
  WalkItem(cond, enum_walk::POSTFIX | enum_walk::SUBQUERY,
           [&force_materialization, thd](Item *item) {
             if (item->type() == Item::SUBQUERY_ITEM) {
               if (!is_quantified_comp_predicate(item)) return false;
               Item_in_subselect *item_subs =
                   down_cast<Item_in_subselect *>(item);
               const Query_expression *query_expr = item_subs->query_expr();
               Query_block *qb = query_expr->first_query_block();
               // Sometimes a query block is marked for materialization
               // during resolving. However, because of an always false
               // condition detected elsewhere in the query during
               // optimization, this query block may not be optimized.
               // So, check that before forcing materialization.
               if (query_expr->is_optimized() &&
                   qb->subquery_strategy(thd) ==
                       Subquery_strategy::SUBQ_MATERIALIZATION) {
                 force_materialization = true;
                 return true;
               }
             }
             return false;
           });
  return force_materialization;
}

/**
  If the path is a FILTER path marked that subqueries are to be materialized,
  do so. If not, do nothing.

  It is important that this is not called until the entire plan is ready;
  not just when planning a single query block. The reason is that a query
  block A with materializable subqueries may itself be part of a materializable
  subquery B, so if one calls this when planning A, the subqueries in A will
  irrevocably be materialized, even if that is not the optimal plan given B.
  Thus, this is done when creating iterators.
 */
bool FinalizeMaterializedSubqueries(THD *thd, JOIN *join, AccessPath *path) {
  if (path->type != AccessPath::FILTER ||
      !(path->filter().materialize_subqueries ||
        IsForcedMaterialization(thd, path->filter().condition))) {
    return false;
  }
  return WalkItem(
      path->filter().condition, enum_walk::POSTFIX | enum_walk::SUBQUERY,
      [thd, join](Item *item) {
        if (!is_quantified_comp_predicate(item)) {
          return false;
        }
        Item_in_subselect *item_subs = down_cast<Item_in_subselect *>(item);
        if (item_subs->strategy == Subquery_strategy::SUBQ_MATERIALIZATION) {
          // This subquery is already set up for materialization.
          return false;
        }
        const Query_expression *query_expr = item_subs->query_expr();
        // The subquery is eliminated. Do not materialize.
        if (!query_expr->is_optimized()) {
          return false;
        }
        // If IN-TO-EXISTS is forced, don't materialize.
        Query_block *qb = query_expr->first_query_block();
        if (qb->subquery_strategy(thd) == Subquery_strategy::SUBQ_EXISTS) {
          return false;
        }
        if (!item_subs->subquery_allows_materialization(thd, qb,
                                                        join->query_block)) {
          return false;
        }
        if (item_subs->finalize_materialization_transform(thd, qb->join)) {
          return true;
        }
        item_subs->create_iterators(thd);
        return false;
      });
}

struct IteratorToBeCreated {
  AccessPath *path;
  JOIN *join;
  bool eligible_for_batch_mode;
  unique_ptr_destroy_only<RowIterator> *destination;
  Bounds_checked_array<unique_ptr_destroy_only<RowIterator>> children;

  void AllocChildren(MEM_ROOT *mem_root, int num_children) {
    children =
        Bounds_checked_array<unique_ptr_destroy_only<RowIterator>>::Alloc(
            mem_root, num_children);
  }
};

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *child, JOIN *join,
                          bool eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the child, and we'll return to this job later.
  job->AllocChildren(mem_root, 1);
  todo->push_back(*job);
  todo->push_back(
      {child, join, eligible_for_batch_mode, &job->children[0], {}});
}

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *outer,
                          AccessPath *inner, JOIN *join,
                          bool inner_eligible_for_batch_mode,
                          IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the children, and we'll return to this job later.
  // Note that we push the inner before the outer job, so that we get
  // left created before right (invalidators in materialization access paths,
  // used in the old join optimizer, depend on this).
  job->AllocChildren(mem_root, 2);
  todo->push_back(*job);
  todo->push_back(
      {inner, join, inner_eligible_for_batch_mode, &job->children[1], {}});
  todo->push_back({outer, join, false, &job->children[0], {}});
}

}  // namespace

const Mem_root_array<Item *> *GetExtraHashJoinConditions(
    MEM_ROOT *mem_root, bool using_hypergraph_optimizer,
    const vector<HashJoinCondition> &equijoin_conditions,
    const Mem_root_array<Item *> &other_conditions) {
  if (!using_hypergraph_optimizer) {
    // The old optimizer has already collected the necessary conditions in
    // other_conditions or in a filter on top of the hash join.
    return &other_conditions;
  }

  if (all_of(equijoin_conditions.begin(), equijoin_conditions.end(),
             [](const HashJoinCondition &condition) {
               return condition.store_full_sort_key();
             })) {
    // When we have no partially stored hash keys, there are no more conditions
    // to add.
    return &other_conditions;
  }

  // If we have at least one part of the hash key that cannot be stored fully in
  // the hash join buffer, we need to add the corresponding equijoin condition
  // as an extra condition to evaluate after the hash join. Append it to the
  // non-equijoin predicates that we already have.
  Mem_root_array<Item *> *extra_conditions =
      new (mem_root) Mem_root_array<Item *>(mem_root, other_conditions);
  if (extra_conditions == nullptr) return nullptr;

  for (const HashJoinCondition &condition : equijoin_conditions) {
    if (!condition.store_full_sort_key()) {
      if (extra_conditions->push_back(condition.join_condition())) {
        return nullptr;
      }
    }
  }

  return extra_conditions;
}

unique_ptr_destroy_only<RowIterator> CreateIteratorFromAccessPath(
    THD *thd, MEM_ROOT *mem_root, AccessPath *top_path, JOIN *top_join,
    bool top_eligible_for_batch_mode) {
  assert(IteratorsAreNeeded(thd, top_path));

  unique_ptr_destroy_only<RowIterator> ret;
  Mem_root_array<IteratorToBeCreated> todo(mem_root);
  todo.push_back({top_path, top_join, top_eligible_for_batch_mode, &ret, {}});

  // The access path trees can be pretty deep, and the stack frames can be big
  // on certain compilers/setups, so instead of explicit recursion, we push jobs
  // onto a MEM_ROOT-backed stack. This uses a little more RAM (the MEM_ROOT
  // typically lives to the end of the query), but reduces the stack usage
  // greatly.
  //
  // The general rule is that if an iterator requires any children, it will push
  // jobs for their access paths at the end of the stack and then re-push
  // itself. When the children are instantiated and we get back to the original
  // iterator, we'll actually instantiate it. (We distinguish between the two
  // cases on basis of whether job.children has been allocated or not; the child
  // iterator's destination will point into this array. The child list needs
  // to be allocated in a way that doesn't move around if the TODO job list
  // is reallocated, which we do by means of allocating it directly on the
  // MEM_ROOT.)
  while (!todo.empty()) {
    IteratorToBeCreated job = todo.back();
    todo.pop_back();

    AccessPath *path = job.path;
    JOIN *join = job.join;
    bool eligible_for_batch_mode = job.eligible_for_batch_mode;

    if (job.join != nullptr) {
      assert(!job.join->needs_finalize);
    }

    unique_ptr_destroy_only<RowIterator> iterator;

    ha_rows *examined_rows = nullptr;
    if (path->count_examined_rows && join != nullptr) {
      examined_rows = &join->examined_rows;
    }

    switch (path->type) {
      case AccessPath::TABLE_SCAN: {
        const auto &param = path->table_scan();
        iterator = NewIterator<TableScanIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_SCAN: {
        const auto &param = path->index_scan();
        if (param.reverse) {
          iterator = NewIterator<IndexScanIterator<true>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<IndexScanIterator<false>>(
              thd, mem_root, param.table, param.idx, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::INDEX_DISTANCE_SCAN: {
        const auto &param = path->index_distance_scan();
        iterator = NewIterator<IndexDistanceScanIterator>(
            thd, mem_root, param.table, param.idx, param.range,
            path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::REF: {
        const auto &param = path->ref();
        if (param.reverse) {
          iterator = NewIterator<RefIterator<true>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<RefIterator<false>>(
              thd, mem_root, param.table, param.ref, param.use_order,
              path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::REF_OR_NULL: {
        const auto &param = path->ref_or_null();
        iterator = NewIterator<RefOrNullIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::EQ_REF: {
        const auto &param = path->eq_ref();
        iterator = NewIterator<EQRefIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::PUSHED_JOIN_REF: {
        const auto &param = path->pushed_join_ref();
        iterator = NewIterator<PushedJoinRefIterator>(
            thd, mem_root, param.table, param.ref, param.use_order,
            param.is_unique, examined_rows);
        break;
      }
      case AccessPath::FULL_TEXT_SEARCH: {
        const auto &param = path->full_text_search();
        iterator = NewIterator<FullTextSearchIterator>(
            thd, mem_root, param.table, param.ref, param.ft_func,
            param.use_order, param.use_limit, examined_rows);
        break;
      }
      case AccessPath::CONST_TABLE: {
        const auto &param = path->const_table();
        iterator = NewIterator<ConstIterator>(thd, mem_root, param.table,
                                              param.ref, examined_rows);
        break;
      }
      case AccessPath::MRR: {
        const auto &param = path->mrr();
        const auto &bka_param = param.bka_path->bka_join();
        iterator = NewIterator<MultiRangeRowIterator>(
            thd, mem_root, param.table, param.ref, param.mrr_flags,
            bka_param.join_type,
            GetUsedTables(bka_param.outer, /*include_pruned_tables=*/true),
            bka_param.store_rowids, bka_param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::FOLLOW_TAIL: {
        const auto &param = path->follow_tail();
        iterator = NewIterator<FollowTailIterator>(
            thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_RANGE_SCAN: {
        const auto &param = path->index_range_scan();
        TABLE *table = param.used_key_part[0].field->table;
        if (param.geometry) {
          iterator = NewIterator<GeometryIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        } else if (param.reverse) {
          iterator = NewIterator<ReverseIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, mem_root, param.mrr_flags,
              Bounds_checked_array{param.ranges, param.num_ranges},
              param.using_extended_key_parts);
        } else {
          iterator = NewIterator<IndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(),
              param.index, param.need_rows_in_rowid_order, param.reuse_handler,
              mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        }
        break;
      }
      case AccessPath::INDEX_MERGE: {
        const auto &param = path->index_merge();
        unique_ptr_destroy_only<RowIterator> pk_quick_select;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          AccessPath *range_scan = (*param.children)[child_idx];
          if (param.allow_clustered_primary_key_scan &&
              param.table->file->primary_key_is_clustered() &&
              range_scan->index_range_scan().index ==
                  param.table->s->primary_key) {
            assert(pk_quick_select == nullptr);
            pk_quick_select = std::move(job.children[child_idx]);
          } else {
            children.push_back(std::move(job.children[child_idx]));
          }
        }

        iterator = NewIterator<IndexMergeIterator>(
            thd, mem_root, mem_root, param.table, std::move(pk_quick_select),
            std::move(children));
        break;
      }
      case AccessPath::ROWID_INTERSECTION: {
        const auto &param = path->rowid_intersection();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size() +
                                          (param.cpk_child != nullptr ? 1 : 0));
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          if (param.cpk_child != nullptr) {
            todo.push_back({param.cpk_child,
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[param.children->size()],
                            {}});
          }
          continue;
        }

        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size();
             ++child_idx) {
          children.push_back(std::move(job.children[child_idx]));
        }

        unique_ptr_destroy_only<RowIterator> cpk_child;
        if (param.cpk_child != nullptr) {
          cpk_child = std::move(job.children[param.children->size()]);
        }
        iterator = NewIterator<RowIDIntersectionIterator>(
            thd, mem_root, mem_root, param.table, param.retrieve_full_rows,
            param.need_rows_in_rowid_order, std::move(children),
            std::move(cpk_child));
        break;
      }
      case AccessPath::ROWID_UNION: {
        const auto &param = path->rowid_union();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator = NewIterator<RowIDUnionIterator>(
            thd, mem_root, mem_root, param.table, std::move(children));
        break;
      }
      case AccessPath::INDEX_SKIP_SCAN: {
        const IndexSkipScanParameters *param = path->index_skip_scan().param;
        iterator = NewIterator<IndexSkipScanIterator>(
            thd, mem_root, path->index_skip_scan().table, param->index_info,
            path->index_skip_scan().index, param->eq_prefix_len,
            param->eq_prefix_key_parts, param->eq_prefixes,
            path->index_skip_scan().num_used_key_parts, mem_root,
            param->has_aggregate_function, param->min_range_key,
            param->max_range_key, param->min_search_key, param->max_search_key,
            param->range_cond_flag, param->range_key_len);
        break;
      }
      case AccessPath::GROUP_INDEX_SKIP_SCAN: {
        const GroupIndexSkipScanParameters *param =
            path->group_index_skip_scan().param;
        iterator = NewIterator<GroupIndexSkipScanIterator>(
            thd, mem_root, path->group_index_skip_scan().table,
            &param->min_functions, &param->max_functions,
            param->have_agg_distinct, param->min_max_arg_part,
            param->group_prefix_len, param->group_key_parts,
            param->real_key_parts, param->max_used_key_length,
            param->index_info, path->group_index_skip_scan().index,
            param->key_infix_len, mem_root, param->is_index_scan,
            &param->prefix_ranges, &param->key_infix_ranges,
            &param->min_max_ranges);
        break;
      }
      case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
        const auto &param = path->dynamic_index_range_scan();
        iterator = NewIterator<DynamicRangeIterator>(
            thd, mem_root, param.table, param.qep_tab, examined_rows);
        break;
      }
      case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
        assert(join != nullptr);
        Query_block *query_block = join->query_block;
        iterator = NewIterator<TableValueConstructorIterator>(
            thd, mem_root, examined_rows, *query_block->row_value_list,
            path->table_value_constructor().output_refs);
        break;
      }
      case AccessPath::FAKE_SINGLE_ROW:
        iterator =
            NewIterator<FakeSingleRowIterator>(thd, mem_root, examined_rows);
        break;
      case AccessPath::ZERO_ROWS: {
        iterator = NewIterator<ZeroRowsIterator>(thd, mem_root,
                                                 CollectTables(thd, path));
        break;
      }
      case AccessPath::ZERO_ROWS_AGGREGATED:
        iterator = NewIterator<ZeroRowsAggregatedIterator>(thd, mem_root, join,
                                                           examined_rows);
        break;
      case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
        const auto &param = path->materialized_table_function();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializedTableFunctionIterator>(
            thd, mem_root, param.table_function, param.table,
            std::move(job.children[0]));
        break;
      }
      case AccessPath::UNQUALIFIED_COUNT:
        iterator = NewIterator<UnqualifiedCountIterator>(thd, mem_root, join);
        break;
      case AccessPath::NESTED_LOOP_JOIN: {
        const auto &param = path->nested_loop_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }

        iterator = NewIterator<NestedLoopIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.join_type, param.pfs_batch_mode);
        break;
      }
      case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
        const auto &param = path->nested_loop_semijoin_with_duplicate_removal();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<NestedLoopSemiJoinWithDuplicateRemovalIterator>(
            thd, mem_root, std::move(job.children[0]),
            std::move(job.children[1]), param.table, param.key, param.key_len);
        break;
      }
      case AccessPath::BKA_JOIN: {
        const auto &param = path->bka_join();
        AccessPath *mrr_path =
            FindSingleAccessPathOfType(param.inner, AccessPath::MRR);
        if (job.children.is_null()) {
          mrr_path->mrr().bka_path = path;
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/false, &job,
                               &todo);
          continue;
        }

        MultiRangeRowIterator *mrr_iterator =
            down_cast<MultiRangeRowIterator *>(
                mrr_path->iterator->real_iterator());
        iterator = NewIterator<BKAIterator>(
            thd, mem_root, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            std::move(job.children[1]), thd->variables.join_buff_size,
            param.mrr_length_per_rec, param.rec_per_key, param.store_rowids,
            param.tables_to_get_rowid_for, mrr_iterator,
            CollectSingleRowIndexLookups(thd, path), param.join_type);
        break;
      }
      case AccessPath::HASH_JOIN: {
        const auto &param = path->hash_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/true, &job,
                               &todo);
          continue;
        }
        const JoinPredicate *join_predicate = param.join_predicate;
        vector<HashJoinCondition> conditions;
        conditions.reserve(join_predicate->expr->equijoin_conditions.size());
        for (Item_eq_base *cond : join_predicate->expr->equijoin_conditions) {
          conditions.emplace_back(cond, thd->mem_root);
        }
        const Mem_root_array<Item *> *extra_conditions =
            GetExtraHashJoinConditions(
                mem_root, thd->lex->using_hypergraph_optimizer(), conditions,
                join_predicate->expr->join_conditions);
        if (extra_conditions == nullptr) return nullptr;
        const bool probe_input_batch_mode =
            eligible_for_batch_mode && ShouldEnableBatchMode(param.outer);
        double estimated_build_rows = param.inner->num_output_rows();
        if (param.inner->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        JoinType join_type{JoinType::INNER};
        switch (join_predicate->expr->type) {
          case RelationalExpression::INNER_JOIN:
          case RelationalExpression::STRAIGHT_INNER_JOIN:
            join_type = JoinType::INNER;
            break;
          case RelationalExpression::LEFT_JOIN:
            join_type = JoinType::OUTER;
            break;
          case RelationalExpression::ANTIJOIN:
            join_type = JoinType::ANTI;
            break;
          case RelationalExpression::SEMIJOIN:
            join_type =
                param.rewrite_semi_to_inner ? JoinType::INNER : JoinType::SEMI;
            break;
          case RelationalExpression::TABLE:
          default:
            assert(false);
        }
        // See if we can allow the hash table to keep its contents across Init()
        // calls.
        //
        // The old optimizer will sometimes push join conditions referring
        // to outer tables (in the same query block) down in under the hash
        // operation, so without analysis of each filter and join condition, we
        // cannot say for sure, and thus have to turn it off. But the hypergraph
        // optimizer sets parameter_tables properly, so we're safe if we just
        // check that.
        //
        // Regardless of optimizer, we can push outer references down in under
        // the hash, but join->hash_table_generation will increase whenever we
        // need to recompute the query block (in JOIN::clear_hash_tables()).
        //
        // TODO(sgunders): The old optimizer had a concept of _when_ to clear
        // derived tables (invalidators), and this is somehow similar. If it
        // becomes a performance issue, consider reintroducing them.
        //
        // TODO(sgunders): Should this perhaps be set as a flag on the access
        // path instead of being computed here? We do make the same checks in
        // the cost model, so perhaps it should set the flag as well.
        uint64_t *hash_table_generation =
            (thd->lex->using_hypergraph_optimizer() &&
             path->parameter_tables == 0)
                ? &join->hash_table_generation
                : nullptr;

        // If the probe (outer) input is empty, the join result will be empty,
        // and we do not need to read the build input. For inner join and
        // semijoin, the converse is also true. To benefit from this, we want to
        // start with the input where the cost of reading the first row is
        // lowest. (We only do this for Hypergraph, as the cost data for the
        // traditional optimizer are incomplete, and since we are reluctant to
        // change existing behavior.) Note that we always try the probe input
        // first for left join and antijoin.
        const HashJoinInput first_input =
            (thd->lex->using_hypergraph_optimizer() &&
             param.inner->first_row_cost() > param.outer->first_row_cost())
                ? HashJoinInput::kProbe
                : HashJoinInput::kBuild;

        iterator = NewIterator<HashJoinIterator>(
            thd, mem_root, std::move(job.children[1]),
            GetUsedTables(param.inner, /*include_pruned_tables=*/true),
            estimated_build_rows, std::move(job.children[0]),
            GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            param.store_rowids, param.tables_to_get_rowid_for,
            thd->variables.join_buff_size, std::move(conditions),
            param.allow_spill_to_disk, join_type, *extra_conditions,
            CollectSingleRowIndexLookups(thd, path), first_input,
            probe_input_batch_mode, hash_table_generation);
        break;
      }
      case AccessPath::FILTER: {
        const auto &param = path->filter();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (FinalizeMaterializedSubqueries(thd, join, path)) {
          return nullptr;
        }
        iterator = NewIterator<FilterIterator>(
            thd, mem_root, std::move(job.children[0]), param.condition);
        break;
      }
      case AccessPath::SORT: {
        const auto &param = path->sort();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows num_rows_estimate = param.child->num_output_rows() < 0.0
                                        ? HA_POS_ERROR
                                        : lrint(param.child->num_output_rows());
        Filesort *filesort = param.filesort;
        iterator = NewIterator<SortingIterator>(
            thd, mem_root, filesort, std::move(job.children[0]),
            CollectSingleRowIndexLookups(thd, param.child), num_rows_estimate,
            param.tables_to_get_rowid_for, examined_rows);
        if (filesort->m_remove_duplicates) {
          filesort->tables[0]->duplicate_removal_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        } else {
          filesort->tables[0]->sorting_iterator =
              down_cast<SortingIterator *>(iterator->real_iterator());
        }
        break;
      }
      case AccessPath::AGGREGATE: {
        const auto &param = path->aggregate();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        Prealloced_array<TABLE *, 4> tables =
            GetUsedTables(param.child, /*include_pruned_tables=*/true);
        iterator = NewIterator<AggregateIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            TableCollection(tables, /*store_rowids=*/false,
                            /*tables_to_get_rowid_for=*/0),
            CollectSingleRowIndexLookups(thd, path), param.olap == ROLLUP_TYPE);
        break;
      }
      case AccessPath::TEMPTABLE_AGGREGATE: {
        const auto &param = path->temptable_aggregate();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.subquery_path,
                          join,
                          /*eligible_for_batch_mode=*/true,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }

        iterator = unique_ptr_destroy_only<RowIterator>(
            temptable_aggregate_iterator::CreateIterator(
                thd, std::move(job.children[0]), param.temp_table_param,
                param.table, std::move(job.children[1]), join,
                param.ref_slice));

        break;
      }
      case AccessPath::LIMIT_OFFSET: {
        const auto &param = path->limit_offset();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows *send_records = nullptr;
        if (param.send_records_override != nullptr) {
          send_records = param.send_records_override;
        } else if (join != nullptr) {
          send_records = &join->send_records;
        }
        iterator = NewIterator<LimitOffsetIterator>(
            thd, mem_root, std::move(job.children[0]), param.limit,
            param.offset, param.count_all_rows, param.reject_multiple_rows,
            send_records);
        break;
      }
      case AccessPath::STREAM: {
        const auto &param = path->stream();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, param.join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<StreamingIterator>(
            thd, mem_root, std::move(job.children[0]), param.temp_table_param,
            param.table, param.provide_rowid, param.join, param.ref_slice);
        break;
      }
      case AccessPath::MATERIALIZE: {
        // The table access path should be a single iterator, not a tree.
        // (ALTERNATIVE counts as a single iterator in this regard.)
        assert(
            path->materialize().table_path->type == AccessPath::TABLE_SCAN ||
            path->materialize().table_path->type == AccessPath::LIMIT_OFFSET ||
            path->materialize().table_path->type == AccessPath::REF ||
            path->materialize().table_path->type == AccessPath::REF_OR_NULL ||
            path->materialize().table_path->type == AccessPath::EQ_REF ||
            path->materialize().table_path->type == AccessPath::ALTERNATIVE ||
            path->materialize().table_path->type == AccessPath::CONST_TABLE ||
            path->materialize().table_path->type == AccessPath::INDEX_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::INDEX_RANGE_SCAN ||
            path->materialize().table_path->type ==
                AccessPath::DYNAMIC_INDEX_RANGE_SCAN);

        MaterializePathParameters *param = path->materialize().param;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param->m_operands.size() + 1);
          todo.push_back(job);
          todo.push_back({path->materialize().table_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          for (size_t i = 0; i < param->m_operands.size(); ++i) {
            const MaterializePathParameters::Operand &from =
                param->m_operands[i];
            todo.push_back({from.subquery_path,
                            from.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[i + 1],
                            {}});
          }
          continue;
        }
        unique_ptr_destroy_only<RowIterator> table_iterator =
            std::move(job.children[0]);
        Mem_root_array<materialize_iterator::Operand> operands(
            thd->mem_root, param->m_operands.size());
        for (size_t i = 0; i < param->m_operands.size(); ++i) {
          const MaterializePathParameters::Operand &from = param->m_operands[i];
          materialize_iterator::Operand &to = operands[i];
          to.subquery_iterator = std::move(job.children[i + 1]);
          to.select_number = from.select_number;
          to.join = from.join;
          to.disable_deduplication_by_hash_field =
              from.disable_deduplication_by_hash_field;
          to.copy_items = from.copy_items;
          to.temp_table_param = from.temp_table_param;
          to.is_recursive_reference = from.is_recursive_reference;
          to.m_first_distinct = from.m_first_distinct;
          to.m_total_operands = from.m_total_operands;
          to.m_operand_idx = from.m_operand_idx;
          to.m_estimated_output_rows = from.subquery_path->num_output_rows();

          if (to.is_recursive_reference) {
            // Find the recursive reference to ourselves; there should be
            // exactly one, as per the standard.
            RowIterator *recursive_reader = FindSingleIteratorOfType(
                from.subquery_path, AccessPath::FOLLOW_TAIL);
            if (recursive_reader == nullptr) {
              // The recursive reference was optimized away, e.g. due to an
              // impossible WHERE condition, so we're not a recursive
              // reference after all.
              to.is_recursive_reference = false;
            } else {
              to.recursive_reader =
                  down_cast<FollowTailIterator *>(recursive_reader);
            }
          }
        }
        JOIN *subjoin = param->ref_slice == -1 ? nullptr : operands[0].join;

        iterator = unique_ptr_destroy_only<RowIterator>(
            materialize_iterator::CreateIterator(
                thd, std::move(operands), param, std::move(table_iterator),
                subjoin));

        break;
      }
      case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
        const auto &param = path->materialize_information_schema_table();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializeInformationSchemaTableIterator>(
            thd, mem_root, std::move(job.children[0]), param.table_list,
            param.condition);
        break;
      }
      case AccessPath::APPEND: {
        const auto &param = path->append();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size();
               ++child_idx) {
            const AppendPathParameters &child_param =
                (*param.children)[child_idx];
            todo.push_back({child_param.path,
                            child_param.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        vector<unique_ptr_destroy_only<RowIterator>> children;
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator =
            NewIterator<AppendIterator>(thd, mem_root, std::move(children));
        break;
      }
      case AccessPath::WINDOW: {
        const auto &param = path->window();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (param.needs_buffering) {
          iterator = NewIterator<BufferingWindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        } else {
          iterator = NewIterator<WindowIterator>(
              thd, mem_root, std::move(job.children[0]), param.temp_table_param,
              join, param.ref_slice);
        }
        break;
      }
      case AccessPath::WEEDOUT: {
        const auto &param = path->weedout();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<WeedoutIterator>(
            thd, mem_root, std::move(job.children[0]), param.weedout_table,
            param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES: {
        const auto &param = path->remove_duplicates();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesIterator>(
            thd, mem_root, std::move(job.children[0]), join, param.group_items,
            param.group_items_size);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
        const auto &param = path->remove_duplicates_on_index();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesOnIndexIterator>(
            thd, mem_root, std::move(job.children[0]), param.table, param.key,
            param.loosescan_key_len);
        break;
      }
      case AccessPath::ALTERNATIVE: {
        const auto &param = path->alternative();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.child,
                          join,
                          eligible_for_batch_mode,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_scan_path,
                          join,
                          eligible_for_batch_mode,
                          &job.children[1],
                          {}});
          continue;
        }
        iterator = NewIterator<AlternativeIterator>(
            thd, mem_root, param.table_scan_path->table_scan().table,
            std::move(job.children[0]), std::move(job.children[1]),
            param.used_ref);
        break;
      }
      case AccessPath::CACHE_INVALIDATOR: {
        const auto &param = path->cache_invalidator();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<CacheInvalidatorIterator>(
            thd, mem_root, std::move(job.children[0]), param.name);
        break;
      }
      case AccessPath::DELETE_ROWS: {
        const auto &param = path->delete_rows();
        if (job.children.is_null()) {
          // Setting up tables for delete must be done before the child
          // iterators are created, as some of the child iterators need to see
          // the final read set when they are constructed, so doing it in
          // DeleteRowsIterator's constructor or Init() is too late.
          SetUpTablesForDelete(thd, join);
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<DeleteRowsIterator>(
            thd, mem_root, std::move(job.children[0]), join,
            param.tables_to_delete_from, param.immediate_tables);
        break;
      }
      case AccessPath::UPDATE_ROWS: {
        const auto &param = path->update_rows();
        if (job.children.is_null()) {
          // Do the final setup for UPDATE before the child iterators are
          // created.
          if (FinalizeOptimizationForUpdate(join)) {
            return nullptr;
          }
          SetupJobsForChildren(mem_root, param.child, join,
                               eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = CreateUpdateRowsIterator(thd, mem_root, join,
                                            std::move(job.children[0]));
        break;
      }
      case AccessPath::SAMPLE_SCAN: { /* LCOV_EXCL_LINE */
        // SampleScan can be executed only in the secondary engine.
        assert(false); /* LCOV_EXCL_LINE */
      }
    }

    if (iterator == nullptr) {
      return nullptr;
    }

    path->iterator = iterator.get();
    *job.destination = std::move(iterator);
  }
  return ret;
}

void FindTablesToGetRowidFor(AccessPath *path) {
  // A map of the tables for which other paths further down in the tree will
  // take care of copying the correct row ID into table->file->ref. The hash
  // join iterators and the BKA join iterators do that, so iterators higher up
  // should not call handler::position(), as that would overwrite the copied row
  // ID with the row ID of the last row that was read by the join. Sorting
  // iterators, on the other hand, do not
  table_map handled_by_others = 0;

  auto add_tables_handled_by_others = [path, &handled_by_others](
                                          AccessPath *subpath, const JOIN *) {
    if (path == subpath) return false;  // Skip ourselves.
    switch (subpath->type) {
      case AccessPath::HASH_JOIN:
        handled_by_others |=
            GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::BKA_JOIN:
        handled_by_others |= GetUsedTableMap(subpath->bka_join().outer,
                                             /*include_pruned_tables=*/true);
        FindTablesToGetRowidFor(subpath);
        return true;  // Don't double-traverse.
      case AccessPath::STREAM: {
        subpath->stream().provide_rowid = true;
        TABLE *table = subpath->stream().table;
        if (table->pos_in_table_list == nullptr) {
          // Don't need to set anything; see comment on the similar
          // test in NewSortAccessPath().
        } else {
          handled_by_others |= table->pos_in_table_list->map();
        }
        // Doesn't really matter, we don't cross query blocks anyway.
        return true;
      }
      case AccessPath::SORT:
        // The sorting iterators do not populate handler::ref with the row ID
        // while returning rows, so row IDs in any tables handled by paths below
        // it have to be fetched again from the handler by the paths above the
        // sort. Therefore, we don't add any of the tables in the subtree below
        // SORT to handled_by_others.
        FindTablesToGetRowidFor(subpath);
        return true;  // Skip the rest of the subtree.
      default:
        return false;
    }
  };

  // We stop at MATERIALIZE and STREAM (they supply row IDs for us without
  // having to ask the tables below).
  switch (path->type) {
    case AccessPath::HASH_JOIN:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->hash_join().store_rowids = true;
      path->hash_join().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::BKA_JOIN:
      WalkAccessPaths(path->bka_join().outer, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->bka_join().store_rowids = true;
      path->bka_join().tables_to_get_rowid_for =
          GetUsedTableMap(path->bka_join().outer,
                          /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::WEEDOUT:
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->weedout().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    case AccessPath::SORT:
      // Enabling use of row IDs must happen before the Filesort object is
      // created, so assert that we either have not created the Filesort object,
      // or the Filesort object already has row IDs enabled.
      assert(path->sort().filesort == nullptr ||
             !path->sort().filesort->using_addon_fields());
      path->sort().force_sort_rowids = true;
      WalkAccessPaths(path, /*join=*/nullptr,
                      WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                      add_tables_handled_by_others);
      path->sort().tables_to_get_rowid_for =
          GetUsedTableMap(path, /*include_pruned_tables=*/true) &
          ~handled_by_others;
      break;
    default:
      my_abort();
  }
}

// Move the join conditions that are left in path->filter_predicates into the
// hash join predicate of the given HASH_JOIN access path. Note that join
// conditions with subqueries are not moved. If the subqueries need to be
// materialized, then a filter access path is expected from the caller.
// So they will continue to stay as filters on top of the hash join.
//
// TODO(khatlen): It's a bit of a hack to widen the hash join condition like
// this after the plan has been found. It would be better if we found a way to
// encode the necessary information in the hypergraph itself. For example, when
// creating cycles in the hypergraph, we could add redundant complex hyperedges
// in addition to the simple cycle edges that we currently add.
static void MoveFilterPredicatesIntoHashJoinCondition(
    THD *thd, AccessPath *path, const Mem_root_array<Predicate> &predicates,
    int num_where_predicates) {
  Mem_root_array<Item_eq_base *> equijoin_conditions(thd->mem_root);
  Mem_root_array<Item *> join_conditions(thd->mem_root);
  MutableOverflowBitset moved_predicates(thd->mem_root, predicates.size());

  for (int filter_idx : BitsSetIn(path->filter_predicates)) {
    if (filter_idx >= num_where_predicates) break;
    const Predicate &predicate = predicates[filter_idx];
    if (!predicate.was_join_condition) continue;

    Item *condition = predicate.condition;
    // Conditions with subqueries are not moved.
    if (condition->has_subquery()) continue;
    moved_predicates.SetBit(filter_idx);
    if (condition->type() == Item::FUNC_ITEM &&
        down_cast<Item_func *>(condition)
            ->contains_only_equi_join_condition()) {
      equijoin_conditions.push_back(down_cast<Item_eq_base *>(condition));
    } else {
      join_conditions.push_back(condition);
    }
  }

  if (equijoin_conditions.empty() && join_conditions.empty()) {
    // No join conditions were found in the filter predicates.
    return;
  }

  // Create a new JoinPredicate with all the conditions. We don't fully
  // initialize it, since we're done planning and don't need most of the
  // information any more. Just add enough to make EXPLAIN and
  // CreateIteratorFromAccessPath() happy.
  // TODO(khatlen): Maybe it's better to put directly into the access path those
  // few parts of the join predicate that are needed, and leave the actual
  // predicate and relational expression out.
  auto &param = path->hash_join();
  for (Item_eq_base *item : param.join_predicate->expr->equijoin_conditions) {
    equijoin_conditions.push_back(item);
  }
  for (Item *item : param.join_predicate->expr->join_conditions) {
    join_conditions.push_back(item);
  }
  RelationalExpression *expr = new (thd->mem_root) RelationalExpression(thd);
  expr->type = param.join_predicate->expr->type;
  expr->equijoin_conditions = std::move(equijoin_conditions);
  expr->join_conditions = std::move(join_conditions);
  JoinPredicate *join_predicate = new (thd->mem_root) JoinPredicate;
  join_predicate->expr = expr;
  param.join_predicate = join_predicate;

  path->filter_predicates = OverflowBitset::Xor(
      thd->mem_root, path->filter_predicates, std::move(moved_predicates));
}

Item *ConditionFromFilterPredicates(const Mem_root_array<Predicate> &predicates,
                                    OverflowBitset mask,
                                    int num_where_predicates) {
  List<Item> items;
  for (int pred_idx : BitsSetIn(mask)) {
    if (pred_idx >= num_where_predicates) break;
    items.push_back(predicates[pred_idx].condition);
  }
  return CreateConjunction(&items);
}

void ExpandSingleFilterAccessPath(THD *thd, AccessPath *path, const JOIN *join,
                                  const Mem_root_array<Predicate> &predicates,
                                  unsigned num_where_predicates) {
  // Expand join filters for nested loop joins.
  if (path->type == AccessPath::NESTED_LOOP_JOIN &&
      !path->nested_loop_join().already_expanded_predicates &&
      !(IsEmpty(path->nested_loop_join().equijoin_predicates) &&
        path->nested_loop_join()
            .join_predicate->expr->join_conditions.empty()) &&
      path->nested_loop_join().inner->type != AccessPath::ZERO_ROWS) {
    AccessPath *right_path = path->nested_loop_join().inner;
    const RelationalExpression *expr =
        path->nested_loop_join().join_predicate->expr;

    // While we're collecting the join conditions, calculate cost and output
    // rows (purely for display purposes). Note that this mirrors the
    // calculation we are doing in CostingReceiver::ProposeNestedLoopJoin();
    // we don't have space in the AccessPath to store it there.
    double filter_cost = right_path->cost();
    double filter_rows = right_path->num_output_rows();

    List<Item> items;
    for (size_t filter_idx :
         BitsSetIn(path->nested_loop_join().equijoin_predicates)) {
      Item *condition = expr->equijoin_conditions[filter_idx];
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, *expr->companion_set);
    }
    for (Item *condition : expr->join_conditions) {
      items.push_back(condition);
      filter_cost +=
          EstimateFilterCost(thd, filter_rows, condition, join->query_block)
              .cost_if_not_materialized;
      filter_rows *= EstimateSelectivity(thd, condition, *expr->companion_set);
    }
    assert(!items.is_empty());

    AccessPath *filter_path = new (thd->mem_root) AccessPath;
    filter_path->type = AccessPath::FILTER;
    filter_path->filter().child = right_path;
    filter_path->has_group_skip_scan = right_path->has_group_skip_scan;

    // We don't bother trying to materialize subqueries in join conditions,
    // since they should be very rare.
    filter_path->filter().materialize_subqueries = false;

    CopyBasicProperties(*right_path, filter_path);
    filter_path->filter().condition = CreateConjunction(&items);
    filter_path->set_cost(filter_cost);
    filter_path->set_num_output_rows(filter_rows);

    path->nested_loop_join().inner = filter_path;

    // Since multiple root paths may have their filters expanded,
    // and the same nested loop may be a subpath in several
    // of them, we need to make sure we don't add the join predicates
    // more than once, so mark them as done here.
    path->nested_loop_join().already_expanded_predicates = true;
  }

  // If a hash join follows an edge that is part of a cycle in the hypergraph,
  // there may be other applicable join predicates left in filter_predicates.
  // Say we have {t1,t2} HJ {t3} along the t1.a=t3.a edge. If there is also a
  // t2.b=t3.b edge, that predicate will be in filtered_predicates. In this
  // case, it is desirable to have t1.a=t3.a AND t2.b=t3.b as the hash join
  // predicate, and remove t2.b=t3.b from the filter predicates.
  if (path->type == AccessPath::HASH_JOIN &&
      path->hash_join().join_predicate->expr->join_predicate_first !=
          path->hash_join().join_predicate->expr->join_predicate_last) {
    MoveFilterPredicatesIntoHashJoinCondition(thd, path, predicates,
                                              num_where_predicates);
  }

  // Expand filters _after_ the access path (these are much more common).
  Item *condition = ConditionFromFilterPredicates(
      predicates, path->filter_predicates, num_where_predicates);
  if (condition == nullptr) {
    return;
  }
  AccessPath *new_path = new (thd->mem_root) AccessPath(*path);
  new_path->filter_predicates.Clear();
  new_path->set_num_output_rows(path->num_output_rows_before_filter);
  new_path->set_cost(path->cost_before_filter());

  // We don't really know how much of init_cost comes from the filter,
  // but we need to heed the invariant that cost >= init_cost
  // also for the new (non-filter) path we're creating, even if it's
  // just for display. Heuristically allocate as much as possible to
  // the filter.
  double filter_only_cost = path->cost() - path->cost_before_filter();
  new_path->set_init_cost(
      std::max(new_path->init_cost() - filter_only_cost, 0.0));
  new_path->set_init_once_cost(
      std::max(new_path->init_once_cost() - filter_only_cost, 0.0));
  assert(new_path->cost() >= new_path->init_cost());
  assert(new_path->init_cost() >= new_path->init_once_cost());

  path->type = AccessPath::FILTER;
  path->filter().condition = condition;
  path->filter().child = new_path;
  path->has_group_skip_scan = new_path->has_group_skip_scan;
  path->filter().materialize_subqueries = false;

  // Clear filter_predicates, but keep applied_sargable_join_predicates.
  path->applied_sargable_join_predicates() =
      ClearFilterPredicates(path->applied_sargable_join_predicates(),
                            num_where_predicates, thd->mem_root);
}

void ExpandFilterAccessPaths(THD *thd, AccessPath *path_arg, const JOIN *join,
                             const Mem_root_array<Predicate> &predicates,
                             unsigned num_where_predicates) {
  WalkAccessPaths(path_arg, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [thd, &predicates, num_where_predicates](
                      AccessPath *path, const JOIN *sub_join) {
                    ExpandSingleFilterAccessPath(
                        thd, path, sub_join, predicates, num_where_predicates);
                    return false;
                  });
}

MutableOverflowBitset ClearFilterPredicates(OverflowBitset predicates,
                                            int num_where_predicates,
                                            MEM_ROOT *mem_root) {
  MutableOverflowBitset applied_sargable_join_predicates =
      predicates.Clone(mem_root);
  applied_sargable_join_predicates.ClearBits(0, num_where_predicates);
  return applied_sargable_join_predicates;
}

table_map GetHashJoinTables(AccessPath *path) {
  table_map tables = 0;
  WalkAccessPaths(
      path, /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
      [&tables](AccessPath *subpath, const JOIN *) {
        if (subpath->type == AccessPath::HASH_JOIN) {
          tables |= GetUsedTableMap(subpath, /*include_pruned_tables=*/true);
          return true;
        }
        return false;
      });
  return tables;
}

void CollectStatusVariables(THD *thd, const JOIN *top_join,
                            const AccessPath &top_path) {
  MutableOverflowBitset seen_first_tables(thd->mem_root,
                                          thd->lex->select_number);
  WalkAccessPaths(
      &top_path, top_join, WalkAccessPathPolicy::ENTIRE_TREE,
      [thd, &seen_first_tables](const AccessPath *path, const JOIN *join) {
        if (join == nullptr) {
          // Skip paths that don't belong to a particular query block. In
          // practice, this means the materialization path of a UNION used as a
          // derived table.
          return false;
        }

        const TABLE *const table = GetBasicTable(path);
        if (table == nullptr) {
          // Skip paths that don't represent a table access.
          return false;
        }
        if (table->pos_in_table_list == nullptr) {
          // Skip paths that read a table that is not in the FROM list.
          // (Typically an internal temporary table created by the optimizer.)
          // We count scans of tables in the FROM list only.
          return false;
        }

        // Check if this is the first table we see in this query block. The
        // first table of each query block is counted in a different status
        // variable than the other tables.
        const int query_block_bit = join->query_block->select_number - 1;
        const bool first_table = !IsBitSet(query_block_bit, seen_first_tables);
        if (first_table) {
          seen_first_tables.SetBit(query_block_bit);
        }

        switch (path->type) {
          case AccessPath::TABLE_SCAN:
            thd->set_status_no_index_used();
            [[fallthrough]];
          case AccessPath::INDEX_SCAN:
            if (first_table) {
              thd->inc_status_select_scan();
            } else {
              thd->inc_status_select_full_join();
            }
            break;

          case AccessPath::INDEX_RANGE_SCAN:
          case AccessPath::INDEX_SKIP_SCAN:
          case AccessPath::GROUP_INDEX_SKIP_SCAN:
          case AccessPath::INDEX_MERGE:
          case AccessPath::ROWID_INTERSECTION:
          case AccessPath::ROWID_UNION:
            if (first_table) {
              thd->inc_status_select_range();
            } else {
              thd->inc_status_select_full_range_join();
            }
            break;

          case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
            thd->set_status_no_index_used();
            thd->set_status_no_good_index_used();
            thd->inc_status_select_range_check();
            break;

          default:;
        }

        // Stop traversing the sub-tree when we have seen a table access. There
        // could be more paths below it (in particular for INDEX_MERGE,
        // ROWID_INTERSECTION and ROWID_UNION), but they would all be for the
        // same table, so we don't want to visit them and double-count the
        // table. Returning true skips the sub-tree below this path.
        return true;
      });
}
