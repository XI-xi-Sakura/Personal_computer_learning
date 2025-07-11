/*****************************************************************************

Copyright (c) 2007, 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/trx0i_s.h
 INFORMATION SCHEMA innodb_trx, innodb_locks and
 innodb_lock_waits tables cache structures and public
 functions.

 Created July 17, 2007 Vasil Dimov
 *******************************************************/

#ifndef trx0i_s_h
#define trx0i_s_h

#include <optional>

#include "dict0types.h"
#include "lock0types.h"
#include "trx0types.h"
#include "univ.i"

class PSI_server_data_lock_container;

struct CHARSET_INFO;

/** The maximum amount of memory that can be consumed by innodb_trx,
innodb_locks and innodb_lock_waits information schema tables. */
constexpr uint32_t TRX_I_S_MEM_LIMIT = 16777216; /* 16 MiB */

/** The maximum length of a string that can be stored in
i_s_locks_row_t::lock_data */
constexpr uint32_t TRX_I_S_LOCK_DATA_MAX_LEN = 8192;

/** The maximum length of a string that can be stored in
i_s_trx_row_t::trx_query */
constexpr uint32_t TRX_I_S_TRX_QUERY_MAX_LEN = 1024;

/** The maximum length of a string that can be stored in
i_s_trx_row_t::trx_operation_state */
constexpr uint32_t TRX_I_S_TRX_OP_STATE_MAX_LEN = 64;

/** The maximum length of a string that can be stored in
i_s_trx_row_t::trx_foreign_key_error */
constexpr uint32_t TRX_I_S_TRX_FK_ERROR_MAX_LEN = 256;

/** The maximum length of a string that can be stored in
i_s_trx_row_t::trx_isolation_level */
constexpr uint32_t TRX_I_S_TRX_ISOLATION_LEVEL_MAX_LEN = 16;

/** Safely copy strings in to the INNODB_TRX table's
string based columns */
#define TRX_I_S_STRING_COPY(data, field, constraint, tcache)             \
  do {                                                                   \
    if (strlen(data) > constraint) {                                     \
      char buff[constraint + 1];                                         \
      strncpy(buff, data, constraint);                                   \
      buff[constraint] = '\0';                                           \
                                                                         \
      field = static_cast<const char *>(                                 \
          ha_storage_put_memlim((tcache)->storage, buff, constraint + 1, \
                                MAX_ALLOWED_FOR_STORAGE(tcache)));       \
    } else {                                                             \
      field = static_cast<const char *>(ha_storage_put_str_memlim(       \
          (tcache)->storage, data, MAX_ALLOWED_FOR_STORAGE(tcache)));    \
    }                                                                    \
  } while (0)

/** This structure represents INFORMATION_SCHEMA.innodb_locks row */
struct i_s_locks_row_t {
  /** The unique identifier of this lock, which does not change over time for a
  single lock request and no two lock requests ever have the same guid. */
  lock_guid_t lock_guid;

  /** Information for record locks.  All these are
  ULINT_UNDEFINED for table locks. */
  /** @{ */
  space_id_t lock_space; /*!< tablespace identifier */
  page_no_t lock_page;   /*!< page number within the_space */
  ulint lock_rec;        /*!< heap number of the record
                         on the page */
  /** @} */

  /** The following are auxiliary and not included in the table */
  /** @{ */
  table_id_t lock_table_id;
  /*!< table identifier from
  lock_get_table_id */
  /** @} */
};

/** This structure represents INFORMATION_SCHEMA.innodb_trx row */
struct i_s_trx_row_t {
  /** transaction identifier */
  trx_id_t trx_id;

  /** transaction state from trx_get_que_state_str()*/
  const char *trx_state;

  /** trx_t::start_time */
  std::chrono::system_clock::time_point trx_started;

  /** a description of lock request if trx is waiting, or nullptr otherwise */
  const i_s_locks_row_t *requested_lock_row;

  /** The value of trx->lock.wait_started */
  std::chrono::system_clock::time_point trx_wait_started;

  /** The value of TRX_WEIGHT(trx) */
  uintmax_t trx_weight;

  /** If `first` is `true` then `second` is the value of the
  trx->lock.schedule_weight, otherwise the `second` should be ignored and
  displayed as NULL to the end user.
  (This could be std::optional once we move to C++17) */
  std::pair<bool, trx_schedule_weight_t> trx_schedule_weight;

  /** thd_get_thread_id() */
  ulint trx_mysql_thread_id;

  /** TRX_I_S_TRX_QUERY_MAX_LEN byte prefix of MySQL statement being executed
  in the transaction */
  const char *trx_query;

  /** the charset of trx_query */
  const CHARSET_INFO *trx_query_cs;

  /** trx_t::op_info */
  const char *trx_operation_state;

  /** n_mysql_tables_in_use in trx_t*/
  ulint trx_tables_in_use;

  /** mysql_n_tables_locked in trx_t */
  ulint trx_tables_locked;

  /** list len of trx_locks in trx_t */
  ulint trx_lock_structs;

  /** mem_heap_get_size(trx->lock_heap) */
  ulint trx_lock_memory_bytes;

  /** lock_number_of_rows_locked() */
  ulint trx_rows_locked;

  /** trx_t::undo_no */
  uintmax_t trx_rows_modified;

  /** n_tickets_to_enter_innodb in trx_t */
  ulint trx_concurrency_tickets;

  /** isolation_level in trx_t */
  const char *trx_isolation_level;

  /** check_unique_secondary in trx_t */
  bool trx_unique_checks;

  /** check_foreigns in trx_t */
  bool trx_foreign_key_checks;

  /** detailed_error in trx_t */
  const char *trx_foreign_key_error;

  /** has_search_latch in trx_t */
  bool trx_has_search_latch;

  /** trx_t::read_only */
  ulint trx_is_read_only;

  /** trx_is_autocommit_non_locking(trx) */
  ulint trx_is_autocommit_non_locking;
};

/** Cache of INFORMATION_SCHEMA table data */
struct trx_i_s_cache_t;

/** Auxiliary enum used by functions that need to select one of the
INFORMATION_SCHEMA tables */
enum i_s_table {
  I_S_INNODB_TRX, /*!< INFORMATION_SCHEMA.innodb_trx */
};

/** This is the intermediate buffer where data needed to fill the
INFORMATION SCHEMA tables is fetched and later retrieved by the C++
code in handler/i_s.cc. */
extern trx_i_s_cache_t *trx_i_s_cache;

/** Initialize INFORMATION SCHEMA trx related cache. */
void trx_i_s_cache_init(trx_i_s_cache_t *cache); /*!< out: cache to init */
/** Free the INFORMATION SCHEMA trx related cache. */
void trx_i_s_cache_free(trx_i_s_cache_t *cache); /*!< in/out: cache to free */

/** Issue a shared/read lock on the tables cache. */
void trx_i_s_cache_start_read(trx_i_s_cache_t *cache); /*!< in: cache */

/** Release a shared/read lock on the tables cache. */
void trx_i_s_cache_end_read(trx_i_s_cache_t *cache); /*!< in: cache */

/** Issue an exclusive/write lock on the tables cache. */
void trx_i_s_cache_start_write(trx_i_s_cache_t *cache); /*!< in: cache */

/** Release an exclusive/write lock on the tables cache. */
void trx_i_s_cache_end_write(trx_i_s_cache_t *cache); /*!< in: cache */

/** Retrieves the number of used rows in the cache for a given
 INFORMATION SCHEMA table.
 @return number of rows */
ulint trx_i_s_cache_get_rows_used(trx_i_s_cache_t *cache, /*!< in: cache */
                                  enum i_s_table table); /*!< in: which table */

/** Retrieves the nth row in the cache for a given INFORMATION SCHEMA
 table.
 @return row */
void *trx_i_s_cache_get_nth_row(trx_i_s_cache_t *cache, /*!< in: cache */
                                enum i_s_table table,   /*!< in: which table */
                                ulint n);               /*!< in: row number */

/** Update the transactions cache if it has not been read for some time.
 @return 0 - fetched, 1 - not */
int trx_i_s_possibly_fetch_data_into_cache(
    trx_i_s_cache_t *cache); /*!< in/out: cache */

/** Returns true if the data in the cache is truncated due to the memory
 limit posed by TRX_I_S_MEM_LIMIT.
 @param[in]   cache   The cache
 @return true if truncated */
bool trx_i_s_cache_is_truncated(trx_i_s_cache_t *cache);

/** The maximum length of a resulting lock_id_size in
trx_i_s_create_lock_id(), not including the terminating NUL.
"%lu:%lu:%lu:%lu:%lu:%lu" -> 20*6+5 chars */
constexpr uint32_t TRX_I_S_LOCK_ID_MAX_LEN = 20 * 6 + 5;

/** Crafts a lock id string from a i_s_locks_row_t object. Returns its
 second argument. This function aborts if there is not enough space in
 lock_id. Be sure to provide at least TRX_I_S_LOCK_ID_MAX_LEN + 1 if you
 want to be 100% sure that it will not abort.
 @param[in]     row
                    The description of the lock sufficient to populate a row
 @param[out]    lock_id
                    The buffer to store the resulting lock_id
 @param[in]     lock_id_size
                    The size of the lock_id buffer
 @return resulting lock id */
char *trx_i_s_create_lock_id(const i_s_locks_row_t &row, char *lock_id,
                             size_t lock_id_size);

/** Fill performance schema lock data.
Create a string that represents the LOCK_DATA
column, for a given lock record.
@param[out]     lock_data       Lock data string
@param[in]      lock            Lock to inspect
@param[in]      heap_no         Lock heap number
@param[in]      container       Data container to fill
*/
void p_s_fill_lock_data(const char **lock_data, const lock_t *lock,
                        ulint heap_no,
                        PSI_server_data_lock_container *container);

/** Fills i_s_locks_row_t object with data about the lock.
@param[out] row     Result object that's filled
@param[in]  lock    Lock to get data from
@param[in]  heap_no Lock's record number or ULINT_UNDEFINED if the lock is a
                    table lock */
void fill_locks_row(i_s_locks_row_t *row, const lock_t *lock, ulint heap_no);

/** Parses lock id into row
@param[in]      lock_id     Lock id generated with trx_i_s_create_lock_id
@param[out]     row         Row to be filled in with data
@return LOCK_REC, LOCK_TABLE or 0 if failed to parse */
int trx_i_s_parse_lock_id(const char *lock_id, i_s_locks_row_t *row);
#endif /* trx0i_s_h */
