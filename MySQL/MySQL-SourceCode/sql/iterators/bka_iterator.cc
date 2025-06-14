/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#include "sql/iterators/bka_iterator.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <algorithm>
#include <iterator>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "my_alloc.h"
#include "my_base.h"

#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/row_iterator.h"
#include "sql/psi_memory_key.h"
#include "sql/sql_executor.h"
#include "sql/sql_opt_exec_shared.h"
#include "sql/table.h"

class JOIN;

using hash_join_buffer::BufferRow;
using hash_join_buffer::LoadBufferRowIntoTableBuffers;
using pack_rows::TableCollection;
using std::string;
using std::vector;

static bool NeedMatchFlags(JoinType join_type) {
  return join_type == JoinType::OUTER || join_type == JoinType::SEMI ||
         join_type == JoinType::ANTI;
}

static size_t BytesNeededForMatchFlags(size_t rows) {
  // One bit per row.
  return (rows + 7) / 8;
}

BKAIterator::BKAIterator(
    THD *thd, unique_ptr_destroy_only<RowIterator> outer_input,
    const Prealloced_array<TABLE *, 4> &outer_input_tables,
    unique_ptr_destroy_only<RowIterator> inner_input,
    size_t max_memory_available, size_t mrr_bytes_needed_for_single_inner_row,
    float expected_inner_rows_per_outer_row, bool store_rowids,
    table_map tables_to_get_rowid_for, MultiRangeRowIterator *mrr_iterator,
    std::span<AccessPath *> single_row_index_lookups, JoinType join_type)
    : RowIterator(thd),
      m_outer_input(std::move(outer_input)),
      m_inner_input(std::move(inner_input)),
      m_mem_root(key_memory_hash_op, 16384 /* 16 kB */),
      m_rows(&m_mem_root),
      m_outer_input_tables(outer_input_tables, store_rowids,
                           tables_to_get_rowid_for),
      m_max_memory_available(max_memory_available),
      m_mrr_bytes_needed_for_single_inner_row(
          mrr_bytes_needed_for_single_inner_row),
      m_mrr_iterator(mrr_iterator),
      m_single_row_index_lookups(single_row_index_lookups),
      m_join_type(join_type) {
  assert(m_outer_input != nullptr);
  assert(m_inner_input != nullptr);

  m_mrr_bytes_needed_per_row =
      lrint(mrr_bytes_needed_for_single_inner_row *
            std::max(expected_inner_rows_per_outer_row, 1.0f));
}

bool BKAIterator::Init() {
  if (!m_outer_input_tables.has_blob_column()) {
    size_t upper_row_size =
        pack_rows::ComputeRowSizeUpperBoundSansBlobs(m_outer_input_tables);
    if (m_outer_row_buffer.reserve(upper_row_size)) {
      my_error(ER_OUTOFMEMORY, MYF(0), upper_row_size);
      return true;
    }
  }
  m_outer_input_tables.PrepareForRequestRowId();

  BeginNewBatch();
  m_end_of_outer_rows = false;
  m_has_row_from_previous_batch = false;

  return m_outer_input->Init();
}

void BKAIterator::BeginNewBatch() {
  m_mem_root.ClearForReuse();
  new (&m_rows) Mem_root_array<BufferRow>(&m_mem_root);
  m_bytes_used = 0;
  m_state = State::NEED_OUTER_ROWS;

  // Invalidate the cache in all single-row index lookups below us. The previous
  // execution of the join, or the processing of the previous batch in the same
  // join, may have overwritten the cached value in EQRefIterator with a value
  // from a different row, and the next read from the EQRefIterator must read
  // the correct value from the index.
  for (AccessPath *lookup : m_single_row_index_lookups) {
    lookup->eq_ref().ref->key_err = true;
  }
}

int BKAIterator::ReadOuterRows() {
  for (;;) {
    if (m_has_row_from_previous_batch) {
      // The outer row will be in m_outer_row_buffer already. Load it back
      // into the global table buffers; MultiRangeRowIterator has loaded other
      // rows into them, and in case we are reading from a join, Read() may
      // not update all of the tables.
      m_has_row_from_previous_batch = false;
      LoadBufferRowIntoTableBuffers(
          m_outer_input_tables,
          hash_join_buffer::Key(m_outer_row_buffer.ptr(),
                                m_outer_row_buffer.length()));
    } else {
      int result = m_outer_input->Read();
      if (result == 1) {
        // Error.
        return 1;
      }
      if (result == -1) {
        // EOF.
        m_end_of_outer_rows = true;
        break;
      }
      m_outer_input_tables.RequestRowId();

      // Save the contents of all columns marked for reading.
      if (StoreFromTableBuffers(m_outer_input_tables, &m_outer_row_buffer)) {
        return 1;
      }
    }

    // See if we have room for this row, and the associated number of MRR
    // rows, without going over our total RAM budget. (We ignore the budget
    // if the buffer is empty; at least a single row must be allowed at all
    // times.)
    const size_t row_size = m_outer_row_buffer.length();
    size_t total_bytes_needed_after_this_row =
        m_bytes_used + row_size +
        (m_mrr_bytes_needed_per_row + sizeof(m_rows[0])) * (m_rows.size() + 1);

    if (NeedMatchFlags(m_join_type)) {
      total_bytes_needed_after_this_row +=
          BytesNeededForMatchFlags(m_rows.size() + 1);
    }

    if (!m_rows.empty() &&
        total_bytes_needed_after_this_row > m_max_memory_available) {
      // Out of memory, so end the batch and send it.
      // This row will be dealt with in the next batch.
      m_has_row_from_previous_batch = true;
      break;
    }

    char *row = m_mem_root.ArrayAlloc<char>(row_size);
    if (row == nullptr) {
      return 1;
    }
    memcpy(row, m_outer_row_buffer.ptr(), row_size);

    m_rows.push_back(BufferRow(row, row_size));
    m_bytes_used += row_size;
  }

  // If we had no rows at all, we're done.
  if (m_rows.empty()) {
    assert(!m_has_row_from_previous_batch);
    m_state = State::END_OF_ROWS;
    return -1;
  }

  // Figure out how much RAM we need to allocate for the MRR row buffer,
  // given to the handler for holding inner rows.
  size_t mrr_buffer_size = m_mrr_bytes_needed_per_row * m_rows.size();
  if (m_bytes_used + mrr_buffer_size >= m_max_memory_available) {
    // Even if it will take us over budget, DS-MRR needs space for at least
    // one row to work.
    assert(m_rows.size() ==
           1);  // Otherwise, we would have stopped reading rows earlier.
    if (m_bytes_used + m_mrr_bytes_needed_for_single_inner_row >=
        m_max_memory_available) {
      mrr_buffer_size = m_mrr_bytes_needed_for_single_inner_row;
    } else {
      mrr_buffer_size = m_max_memory_available - m_bytes_used;
    }
  } else {
    // We're under budget. Heuristically, increase it to get some
    // extra headroom if the estimate is pessimistic.
    mrr_buffer_size = std::min(mrr_buffer_size * 2 + 16384,
                               m_max_memory_available - m_bytes_used);
  }
  assert(mrr_buffer_size >= m_mrr_bytes_needed_for_single_inner_row);

  // Ask the MRR iterator to do the actual read.
  m_mrr_iterator->set_rows(m_rows.begin(), m_rows.end());
  m_mrr_iterator->set_mrr_buffer(m_mem_root.ArrayAlloc<uchar>(mrr_buffer_size),
                                 mrr_buffer_size);
  if (NeedMatchFlags(m_join_type)) {
    const size_t bytes_needed = BytesNeededForMatchFlags(m_rows.size());
    m_mrr_iterator->set_match_flag_buffer(
        m_mem_root.ArrayAlloc<uchar>(bytes_needed));
  }
  if (m_inner_input->Init()) {
    return 1;
  }

  // Probe the rows we've got using MRR.
  m_state = State::RETURNING_JOINED_ROWS;
  m_mrr_iterator->SetNullRowFlag(false);
  return 0;
}

void BKAIterator::BatchFinished() {
  // End of joined rows; start reading the next batch if there are
  // more outer rows.
  if (m_end_of_outer_rows) {
    m_state = State::END_OF_ROWS;
  } else {
    BeginNewBatch();
    assert(m_state == State::NEED_OUTER_ROWS);
  }
}

int BKAIterator::MakeNullComplementedRow() {
  // Find the next row that hasn't been matched to anything yet.
  while (m_current_pos != m_rows.end()) {
    if (m_mrr_iterator->RowHasBeenRead(m_current_pos)) {
      ++m_current_pos;
    } else {
      // Return a NULL-complemented row. (Our table already has the NULL flag
      // set.)
      LoadIntoTableBuffers(m_outer_input_tables,
                           pointer_cast<const uchar *>(m_current_pos->data()));
      ++m_current_pos;
      return 0;
    }
  }

  // No more NULL-complemented rows to return.
  m_mrr_iterator->SetNullRowFlag(false);
  return -1;
}

int BKAIterator::Read() {
  for (;;) {  // Termination condition within loop.
    switch (m_state) {
      case State::END_OF_ROWS:
        return -1;
      case State::NEED_OUTER_ROWS: {
        int err = ReadOuterRows();
        if (err != 0) {
          return err;
        }
        break;
      }
      case State::RETURNING_JOINED_ROWS: {
        int err = m_inner_input->Read();
        if (err != -1) {
          if (err == 0) {
            m_mrr_iterator->MarkLastRowAsRead();
            if (m_join_type == JoinType::ANTI) {
              break;
            }
          }

          // A row or an error; pass it through (unless we are an antijoin).
          return err;
        }

        // No more joined rows in this batch. Go to the next batch -- but
        // if we're an outer join or antijoin, first create NULL-complemented
        // rows for the ones in this batch that we didn't match to anything.
        if (m_join_type == JoinType::OUTER || m_join_type == JoinType::ANTI) {
          m_state = State::RETURNING_NULL_COMPLEMENTED_ROWS;
          m_current_pos = m_rows.begin();
          m_mrr_iterator->SetNullRowFlag(true);
        } else {
          BatchFinished();
          break;
        }
      }
        [[fallthrough]];
      case State::RETURNING_NULL_COMPLEMENTED_ROWS: {
        int err = MakeNullComplementedRow();
        if (err != -1) {
          return err;
        }

        BatchFinished();
        break;
      }
    }
  }
}

MultiRangeRowIterator::MultiRangeRowIterator(
    THD *thd, TABLE *table, Index_lookup *ref, int mrr_flags,
    JoinType join_type, const Prealloced_array<TABLE *, 4> &outer_input_tables,
    bool store_rowids, table_map tables_to_get_rowid_for)
    : TableRowIterator(thd, table),
      m_file(table->file),
      m_ref(ref),
      m_mrr_flags(mrr_flags),
      m_outer_input_tables(outer_input_tables, store_rowids,
                           tables_to_get_rowid_for),
      m_join_type(join_type) {}

bool MultiRangeRowIterator::Init() {
  /*
    Prepare to iterate over keys from the join buffer and to get
    matching candidates obtained with MRR handler functions.
   */
  if (!m_file->inited) {
    const int error = m_file->ha_index_init(m_ref->key, true);
    if (error) {
      m_file->print_error(error, MYF(0));
      return error;
    }
  }
  RANGE_SEQ_IF seq_funcs = {MultiRangeRowIterator::MrrInitCallbackThunk,
                            MultiRangeRowIterator::MrrNextCallbackThunk,
                            nullptr};
  if (m_join_type == JoinType::SEMI || m_join_type == JoinType::ANTI) {
    seq_funcs.skip_record = MultiRangeRowIterator::MrrSkipRecordCallbackThunk;
  }
  if (m_match_flag_buffer != nullptr) {
    assert(NeedMatchFlags(m_join_type));

    // Reset all the match flags.
    memset(m_match_flag_buffer, 0,
           BytesNeededForMatchFlags(std::distance(m_begin, m_end)));
  } else {
    assert(!NeedMatchFlags(m_join_type));
  }

  /**
    We don't send a set of rows directly to MRR; instead, we give it a set
    of function pointers to iterate over the rows, and a pointer to ourselves.
    The handler will call our callbacks as follows:

     1. MrrInitCallback at the start, to initialize iteration.
     2. MrrNextCallback is called to yield ranges to scan, until it returns 1.
   */
  return m_file->multi_range_read_init(&seq_funcs, this,
                                       std::distance(m_begin, m_end),
                                       m_mrr_flags, &m_mrr_buffer);
}

range_seq_t MultiRangeRowIterator::MrrInitCallback(uint, uint) {
  m_current_pos = m_begin;
  return this;
}

uint MultiRangeRowIterator::MrrNextCallback(KEY_MULTI_RANGE *range) {
  // Load the next row from the buffer, if there is one.
  //
  // NULL values will never match in a inner join. The optimizer will often
  // set up a NULL filter for inner joins, but not in all cases, so we must
  // skip such rows by checking impossible_null_ref(). Thus, we iterate
  // until we have a row that is not NULL-filtered. The typical case is
  // that this happens immediately.
  //
  // TODO(sgunders): Consider whether it would be possible to put this check
  // before putting the rows into the buffer. That would require evaluating
  // any items twice, though.
  for (;;) {
    if (m_current_pos == m_end) {
      return 1;
    }

    LoadBufferRowIntoTableBuffers(m_outer_input_tables, *m_current_pos);

    construct_lookup(thd(), table(), m_ref);
    if (!m_ref->impossible_null_ref()) {
      break;
    }
    ++m_current_pos;
  }

  // Set up a range consisting of a single key, so the only difference
  // between start and end is the flags. They signify that the range starts
  // at the row in question, and ends right after it (exclusive).

  range->range_flag = EQ_RANGE;
  range->ptr = const_cast<char *>(pointer_cast<const char *>(m_current_pos));

  range->start_key.key = m_ref->key_buff;
  range->start_key.keypart_map = (1 << m_ref->key_parts) - 1;  // All keyparts.
  range->start_key.length = m_ref->key_length;
  range->start_key.flag = HA_READ_KEY_EXACT;

  range->end_key = range->start_key;
  range->end_key.flag = HA_READ_AFTER_KEY;

  ++m_current_pos;
  return 0;
}

bool MultiRangeRowIterator::MrrSkipRecord(char *range_info) {
  BufferRow *rec_ptr = pointer_cast<BufferRow *>(range_info);
  return RowHasBeenRead(rec_ptr);
}

int MultiRangeRowIterator::Read() {
  // Read a row from the MRR buffer. rec_ptr tells us which outer row
  // this corresponds to; it corresponds to range->ptr in MrrNextCallback(),
  // and points to the serialized outer row in BKAIterator's m_row array.
  BufferRow *rec_ptr = nullptr;
  do {
    int error =
        m_file->ha_multi_range_read_next(pointer_cast<char **>(&rec_ptr));
    if (error != 0) {
      return HandleError(error);
    }

    // NDB never calls mrr_funcs.skip_record(), so we need to recheck here.
    // See bug #30594210.
  } while (m_join_type == JoinType::SEMI && RowHasBeenRead(rec_ptr));

  LoadIntoTableBuffers(m_outer_input_tables,
                       pointer_cast<const uchar *>(rec_ptr->data()));

  m_last_row_returned = rec_ptr;

  return 0;
}
