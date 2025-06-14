/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#include <type_traits>

#include <gtest/gtest.h>

#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/compare_access_paths.h"
#include "sql/join_optimizer/interesting_orders.h"
#include "sql/join_optimizer/interesting_orders_defs.h"
#include "unittest/gunit/test_utils.h"

class THD;

TEST(CompareAccessPathsTest, FuzzyComparison) {
  double fuzz_factor = 1.01;
  EXPECT_EQ(FuzzyComparison(1.0, 1.0, fuzz_factor),
            FuzzyComparisonResult::IDENTICAL);
  EXPECT_EQ(FuzzyComparison(1.0, 1.02, fuzz_factor),
            FuzzyComparisonResult::FIRST_BETTER);
  EXPECT_EQ(FuzzyComparison(1.02, 1.0, fuzz_factor),
            FuzzyComparisonResult::SECOND_BETTER);
  EXPECT_EQ(FuzzyComparison(1.0, 1.005, fuzz_factor),
            FuzzyComparisonResult::FIRST_SLIGHTLY_BETTER);
  EXPECT_EQ(FuzzyComparison(1.005, 1.0, fuzz_factor),
            FuzzyComparisonResult::SECOND_SLIGHTLY_BETTER);

  // x is significantly less (better) than y if fuzz_factor * x < y.
  // Verify that for x = 1.0 we switch from better to slightly better
  // around y = 1.01.
  EXPECT_EQ(FuzzyComparison(1.0, 1.0101, fuzz_factor),
            FuzzyComparisonResult::FIRST_BETTER);
  EXPECT_EQ(FuzzyComparison(1.0, 1.0099, fuzz_factor),
            FuzzyComparisonResult::FIRST_SLIGHTLY_BETTER);

  // Exchanging x and y.
  EXPECT_EQ(FuzzyComparison(1.0101, 1.0, fuzz_factor),
            FuzzyComparisonResult::SECOND_BETTER);
  EXPECT_EQ(FuzzyComparison(1.0099, 1.0, fuzz_factor),
            FuzzyComparisonResult::SECOND_SLIGHTLY_BETTER);
}

TEST(CompareAccessPathsTest, CompareAccessPaths) {
  // We need the test server since the LogicalOrderings constructor takes
  // a pointer to THD.
  my_testing::Server_initializer m_initializer;
  m_initializer.SetUp();
  THD *thd = m_initializer.thd();
  my_testing::TraceGuard trace(thd);
  LogicalOrderings orderings(thd);
  orderings.Build(thd);
  OrderingSet obsolete_orderings;

  // Shorthand for comparing two access paths in all dimensions.
  const auto compare = [&](const AccessPath &path1, const AccessPath &path2) {
    return CompareAccessPaths(orderings, path1, path2, obsolete_orderings,
                              /*need_rowid=*/true,
                              /*use_first_row_cost=*/true);
  };

  AccessPath a;
  // Discrete/categorical cost dimensions (non-fuzzy comparison).
  a.parameter_tables = 0b111;
  a.ordering_state = 0;
  a.safe_for_rowid = AccessPath::Safety::UNSAFE;
  // Numerical cost dimensions (fuzzy comparison).
  a.set_num_output_rows(100.0);
  a.set_cost(100.0);
  a.set_init_cost(100.0);
  a.set_init_once_cost(0.0);
  static_assert(std::is_trivially_copy_constructible<AccessPath>::value);
  AccessPath b = a;

  EXPECT_EQ(compare(a, b), PathComparisonResult::IDENTICAL);
  EXPECT_EQ(compare(b, a), PathComparisonResult::IDENTICAL);

  // Domination from a single discrete dimension (the parameter tables of one
  // path is a subset of the parameter tables of the other path).
  b.parameter_tables = 0b001;
  EXPECT_EQ(compare(a, b), PathComparisonResult::SECOND_DOMINATES);
  EXPECT_EQ(compare(b, a), PathComparisonResult::FIRST_DOMINATES);

  // Different strengths caused by categorical dimensions.
  a.safe_for_rowid = AccessPath::Safety::SAFE;
  EXPECT_EQ(compare(a, b), PathComparisonResult::DIFFERENT_STRENGTHS);
  EXPECT_EQ(compare(b, a), PathComparisonResult::DIFFERENT_STRENGTHS);
  // One path dominates if the safe_for_rowid dimension is ignored.
  EXPECT_EQ(
      CompareAccessPaths(orderings, a, b, obsolete_orderings,
                         /*need_rowid=*/false, /*use_first_row_cost=*/true),
      PathComparisonResult::SECOND_DOMINATES);
  EXPECT_EQ(
      CompareAccessPaths(orderings, b, a, obsolete_orderings,
                         /*need_rowid=*/false, /*use_first_row_cost=*/true),
      PathComparisonResult::FIRST_DOMINATES);

  // Fuzzily identical, neither path dominates.
  a = b;
  a.set_cost(100.5);
  a.set_init_cost(99.5);
  EXPECT_EQ(compare(a, b), PathComparisonResult::IDENTICAL);
  EXPECT_EQ(compare(b, a), PathComparisonResult::IDENTICAL);
  // One path dominates if the first_row_cost dimension is ignored.
  EXPECT_EQ(CompareAccessPaths(orderings, a, b, obsolete_orderings,
                               /*need_rowid=*/true,
                               /*use_first_row_cost=*/false),
            PathComparisonResult::SECOND_DOMINATES);
  EXPECT_EQ(CompareAccessPaths(orderings, b, a, obsolete_orderings,
                               /*need_rowid=*/true,
                               /*use_first_row_cost=*/false),
            PathComparisonResult::FIRST_DOMINATES);

  // Fuzzily identical, but one path dominates (slightly).
  b.set_cost(100.0);
  b.set_init_cost(100.0);
  a.set_cost(99.5);
  a.set_init_cost(99.5);
  EXPECT_EQ(compare(a, b), PathComparisonResult::FIRST_DOMINATES);
  EXPECT_EQ(compare(b, a), PathComparisonResult::SECOND_DOMINATES);

  // Different strenghts in numerical dimensions.
  a.set_cost(100.0);
  b.set_cost(95.0);
  a.set_init_cost(50.0);
  b.set_init_cost(80.0);
  EXPECT_EQ(compare(a, b), PathComparisonResult::DIFFERENT_STRENGTHS);
  EXPECT_EQ(compare(b, a), PathComparisonResult::DIFFERENT_STRENGTHS);
}
