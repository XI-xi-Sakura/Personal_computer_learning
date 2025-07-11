/*****************************************************************************

Copyright (c) 2008, 2025, Oracle and/or its affiliates.

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

/** @file include/api0api.h
 InnoDB Native API

 2008-08-01 Created by Sunny Bains.
 3/20/2011 Jimmy Yang extracted from Embedded InnoDB
 *******************************************************/

#ifndef api0api_h
#define api0api_h

#include <stdint.h>
#include <stdio.h>

#include "my_config.h"

#include "db0err.h"

/** Page number */
typedef uint32_t page_no_t;
/** Tablespace identifier */
typedef uint32_t space_id_t;

typedef struct ib_sdi_key ib_sdi_key_t;
typedef struct ib_sdi_vector ib_sdi_vector_t;

/* Basic types used by the InnoDB API. */
/** All InnoDB error codes are represented by ib_err_t */
typedef enum dberr_t ib_err_t;
/** Representation of a byte within InnoDB */
typedef unsigned char ib_byte_t;

/** Representation of an unsigned long int within InnoDB */
#ifdef _WIN64
typedef unsigned __int64 ib_ulint_t;
#else
typedef unsigned long int ib_ulint_t;
#endif /* _WIN64 */

typedef void *ib_opaque_t;
typedef ib_opaque_t ib_charset_t;
typedef uint64_t ib_id_u64_t;

/** @enum ib_cfg_type_t Possible types for a configuration variable. */
typedef enum {
  IB_CFG_BOOL, /*!< The configuration parameter is
                of type bool */

  /* XXX Can we avoid having different types for ulint and ulong?
  - On Win64 "unsigned long" is 32 bits
  - ulong is always defined as "unsigned long"
  - On Win64 ulint is defined as 64 bit integer
  => On Win64 ulint != ulong.
  If we typecast all ulong and ulint variables to the smaller type
  ulong, then we will cut the range of the ulint variables.
  This is not a problem for most ulint variables because their max
  allowed values do not exceed 2^32-1 (e.g. log_groups is ulint
  but its max allowed value is 10). BUT buffer_pool_size and
  log_file_size allow up to 2^64-1. */

  IB_CFG_ULINT, /*!< The configuration parameter is
                of type ulint */

  IB_CFG_ULONG, /*!< The configuration parameter is
                of type ulong */

  IB_CFG_TEXT, /*!< The configuration parameter is
               of type char* */

  IB_CFG_CB /*!< The configuration parameter is
            a callback parameter */
} ib_cfg_type_t;

/** @enum ib_col_type_t  column types that are supported. */
typedef enum {
  IB_VARCHAR = 1, /*!< Character varying length. The
                  column is not padded. */

  IB_CHAR = 2, /*!< Fixed length character string. The
               column is padded to the right. */

  IB_BINARY = 3, /*!< Fixed length binary, similar to
                 IB_CHAR but the column is not padded
                 to the right. */

  IB_VARBINARY = 4, /*!< Variable length binary */

  IB_BLOB = 5, /*!< Binary large object, or
               a TEXT type */

  IB_INT = 6, /*!< Integer: can be any size
              from 1 - 8 bytes. If the size is
              1, 2, 4 and 8 bytes then you can use
              the typed read and write functions. For
              other sizes you will need to use the
              ib_col_get_value() function and do the
              conversion yourself. */

  IB_SYS = 8, /*!< System column, this column can
              be one of DATA_TRX_ID, DATA_ROLL_PTR
              or DATA_ROW_ID. */

  IB_FLOAT = 9, /*!< C (float)  floating point value. */

  IB_DOUBLE = 10, /*!> C (double) floating point value. */

  IB_DECIMAL = 11, /*!< Decimal stored as an ASCII
                   string */

  IB_VARCHAR_ANYCHARSET = 12, /*!< Any charset, varying length */

  IB_CHAR_ANYCHARSET = 13 /*!< Any charset, fixed length */

} ib_col_type_t;

/** @enum ib_tbl_fmt_t InnoDB table format types */
typedef enum {
  IB_TBL_REDUNDANT, /*!< Redundant row format, the column
                    type and length is stored in the row.*/

  IB_TBL_COMPACT, /*!< Compact row format, the column
                  type is not stored in the row. The
                  length is stored in the row but the
                  storage format uses a compact format
                  to store the length of the column data
                  and record data storage format also
                  uses less storage. */

  IB_TBL_DYNAMIC, /*!< Compact row format. BLOB prefixes
                  are not stored in the clustered index */

  IB_TBL_COMPRESSED /*!< Similar to dynamic format but
                    with pages compressed */
} ib_tbl_fmt_t;

/** @enum ib_col_attr_t InnoDB column attributes */
typedef enum {
  IB_COL_NONE = 0, /*!< No special attributes. */

  IB_COL_NOT_NULL = 1, /*!< Column data can't be NULL. */

  IB_COL_UNSIGNED = 2, /*!< Column is IB_INT and unsigned. */

  IB_COL_NOT_USED = 4, /*!< Future use, reserved. */

  IB_COL_CUSTOM1 = 8, /*!< Custom precision type, this is
                      a bit that is ignored by InnoDB and so
                      can be set and queried by users. */

  IB_COL_CUSTOM2 = 16, /*!< Custom precision type, this is
                       a bit that is ignored by InnoDB and so
                       can be set and queried by users. */

  IB_COL_CUSTOM3 = 32 /*!< Custom precision type, this is
                      a bit that is ignored by InnoDB and so
                      can be set and queried by users. */
} ib_col_attr_t;

/* Note: must match lock0types.h */
/** @enum ib_lck_mode_t InnoDB lock modes. */
typedef enum {
  IB_LOCK_IS = 0, /*!< Intention shared, an intention
                  lock should be used to lock tables */

  IB_LOCK_IX, /*!< Intention exclusive, an intention
              lock should be used to lock tables */

  IB_LOCK_S, /*!< Shared locks should be used to
             lock rows */

  IB_LOCK_X, /*!< Exclusive locks should be used to
             lock rows*/

  IB_LOCK_TABLE_X, /*!< exclusive table lock */

  IB_LOCK_NONE, /*!< This is used internally to note
                consistent read */

  IB_LOCK_NUM = IB_LOCK_NONE /*!< number of lock modes */
} ib_lck_mode_t;

typedef enum {
  IB_CLUSTERED = 1, /*!< clustered index */
  IB_UNIQUE = 2     /*!< unique index */
} ib_index_type_t;

/** @enum ib_srch_mode_t InnoDB cursor search modes for ib_cursor_moveto().
Note: Values must match those found in page0cur.h */
typedef enum {
  IB_CUR_G = 1, /*!< If search key is not found then
                position the cursor on the row that
                is greater than the search key */

  IB_CUR_GE = 2, /*!< If the search key not found then
                 position the cursor on the row that
                 is greater than or equal to the search
                 key */

  IB_CUR_L = 3, /*!< If search key is not found then
                position the cursor on the row that
                is less than the search key */

  IB_CUR_LE = 4 /*!< If search key is not found then
                position the cursor on the row that
                is less than or equal to the search
                key */
} ib_srch_mode_t;

/** @enum ib_match_mode_t Various match modes used by ib_cursor_moveto() */
typedef enum {
  IB_CLOSEST_MATCH, /*!< Closest match possible */

  IB_EXACT_MATCH, /*!< Search using a complete key
                  value */

  IB_EXACT_PREFIX /*!< Search using a key prefix which
                  must match to rows: the prefix may
                  contain an incomplete field (the
                  last field in prefix may be just
                  a prefix of a fixed length column) */
} ib_match_mode_t;

/** InnoDB column meta data. */
typedef struct {
  ib_col_type_t type; /*!< Type of the column */

  ib_col_attr_t attr; /*!< Column attributes */

  uint32_t type_len; /*!< Length of type */

  uint16_t client_type; /*!< 16 bits of data relevant only to
                        the client. InnoDB doesn't care */

  ib_charset_t *charset; /*!< Column charset */
} ib_col_meta_t;

/* Note: Must be in sync with trx0trx.h */
/** @enum ib_trx_level_t Transaction isolation levels */
typedef enum {
  IB_TRX_READ_UNCOMMITTED = 0, /*!< Dirty read: non-locking SELECTs are
                               performed so that we do not look at a
                               possible earlier version of a record;
                               thus they are not 'consistent' reads
                               under this isolation level; otherwise
                               like level 2 */

  IB_TRX_READ_COMMITTED = 1, /*!< Somewhat Oracle-like isolation,
                             except that in range UPDATE and DELETE
                             we must block phantom rows with
                             next-key locks; SELECT ... FOR UPDATE
                             and ...  LOCK IN SHARE MODE only lock
                             the index records, NOT the gaps before
                             them, and thus allow free inserting;
                             each consistent read reads its own
                             snapshot */

  IB_TRX_REPEATABLE_READ = 2, /*!< All consistent reads in the same
                              trx read the same snapshot; full
                              next-key locking used in locking reads
                              to block insertions into gaps */

  IB_TRX_SERIALIZABLE = 3 /*!< All plain SELECTs are converted to
                          LOCK IN SHARE MODE reads */
} ib_trx_level_t;

/** Generical InnoDB callback prototype. */
typedef void (*ib_cb_t)(void);

constexpr uint32_t IB_CFG_BINLOG_ENABLED = 0x1;
constexpr uint32_t IB_CFG_MDL_ENABLED = 0x2;
constexpr uint32_t IB_CFG_DISABLE_ROWLOCK = 0x4;

/** Used by ib_read_tuple to determine number of bytes to allocate for new slot
if needed */
static const size_t REC_BUF_SLOT_SIZE = 16384;

/** The first argument to the InnoDB message logging function. By default
it's set to stderr. You should treat ib_msg_stream_t as a void*, since
it will probably change in the future. */
typedef FILE *ib_msg_stream_t;

/** All log messages are written to this function.It should have the same
behavior as fprintf(3). */
typedef int (*ib_msg_log_t)(ib_msg_stream_t, const char *, ...);

/* Note: This is to make it easy for API users to have type
checking for arguments to our functions. Making it ib_opaque_t
by itself will result in pointer decay resulting in subverting
of the compiler's type checking. */

/** InnoDB tuple handle. This handle can refer to either a cluster index
tuple or a secondary index tuple. There are two types of tuples for each
type of index, making a total of four types of tuple handles. There
is a tuple for reading the entire row contents and another for searching
on the index key. */
typedef struct ib_tuple_t *ib_tpl_t;

/** InnoDB transaction handle, all database operations need to be covered
by transactions. This handle represents a transaction. The handle can be
created with ib_trx_begin(), you commit your changes with ib_trx_commit()
and undo your changes using ib_trx_rollback(). If the InnoDB deadlock
monitor rolls back the transaction then you need to free the transaction
using the function ib_trx_release(). You can query the state of an InnoDB
transaction by calling ib_trx_state(). */
typedef struct trx_t *ib_trx_t;

/** InnoDB cursor handle */
typedef struct ib_cursor_t *ib_crsr_t;

/** This function is used to compare two data fields for which the data type
 is such that we must use the client code to compare them.

 @param col_meta column meta data
 @param p1 key
 @param p1_len key length
 @param p2 second key
 @param p2_len second key length
 @return 1, 0, -1, if a is greater, equal, less than b, respectively */

typedef int (*ib_client_cmp_t)(const ib_col_meta_t *col_meta,
                               const ib_byte_t *p1, uint64_t p1_len,
                               const ib_byte_t *p2, uint64_t p2_len);

/* This should be the same as univ.i */
/** Represents SQL_NULL length */
constexpr uint32_t IB_SQL_NULL = 0xFFFFFFFF;

/** Start a transaction that's been rolled back. This special function
 exists for the case when InnoDB's deadlock detector has rolledack
 a transaction. While the transaction has been rolled back the handle
 is still valid and can be reused by calling this function. If you
 don't want to reuse the transaction handle then you can free the handle
 by calling ib_trx_release().
 @return innobase txn handle */
ib_err_t ib_trx_start(
    ib_trx_t ib_trx,             /*!< in: transaction to restart */
    ib_trx_level_t ib_trx_level, /*!< in: trx isolation level */
    bool read_write,             /*!< in: true if read write
                                      transaction */
    bool auto_commit,            /*!< in: auto commit after each
                                      single DML */
    void *thd);                  /*!< in: THD */

/** Begin a transaction. This will allocate a new transaction handle and
put the transaction in the active state.
@param[in]      ib_trx_level    trx isolation level
@param[in]      read_write      true if read write transaction
@param[in]      auto_commit     auto commit after each single DML
@param[in,out]  thd             MySQL THD
@return innobase txn handle */
ib_trx_t ib_trx_begin(ib_trx_level_t ib_trx_level, bool read_write,
                      bool auto_commit, void *thd);

/** Check if the transaction is read_only */
uint32_t ib_trx_read_only(ib_trx_t ib_trx); /*!< in: trx handle */

/** Release the resources of the transaction. If the transaction was
 selected as a victim by InnoDB and rolled back then use this function
 to free the transaction handle.
 @return DB_SUCCESS or err code */
ib_err_t ib_trx_release(ib_trx_t ib_trx); /*!< in: trx handle */

/** Commit a transaction. This function will release the schema latches too.
 It will also free the transaction handle.
 @return DB_SUCCESS or err code */
ib_err_t ib_trx_commit(ib_trx_t ib_trx); /*!< in: trx handle */

/** Rollback a transaction. This function will release the schema latches too.
 It will also free the transaction handle.
 @return DB_SUCCESS or err code */
ib_err_t ib_trx_rollback(ib_trx_t ib_trx); /*!< in: trx handle */

/** Open an InnoDB secondary index cursor and return a cursor handle to it.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_open_index_using_name(
    ib_crsr_t ib_open_crsr, /*!< in: open/active cursor */
    const char *index_name, /*!< in: secondary index name */
    ib_crsr_t *ib_crsr,     /*!< out,own: InnoDB index cursor */
    int *idx_type,          /*!< out: index is cluster index */
    ib_id_u64_t *idx_id);   /*!< out: index id */

/** Open an InnoDB table by name and return a cursor handle to it.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_open_table(
    const char *name,    /*!< in: table name */
    ib_trx_t ib_trx,     /*!< in: Current transaction handle
                         can be NULL */
    ib_crsr_t *ib_crsr); /*!< out,own: InnoDB cursor */

/** Reset the cursor.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_reset(ib_crsr_t ib_crsr); /*!< in/out: InnoDB cursor */

/** Close an InnoDB table and free the cursor.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_close(ib_crsr_t ib_crsr); /*!< in/out: InnoDB cursor */

/** update the cursor with new transactions and also reset the cursor
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_new_trx(ib_crsr_t ib_crsr, /*!< in/out: InnoDB cursor */
                           ib_trx_t ib_trx);  /*!< in: transaction */

/** Commit the transaction in a cursor
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_commit_trx(ib_crsr_t ib_crsr, /*!< in/out: InnoDB cursor */
                              ib_trx_t ib_trx);  /*!< in: transaction */

/** Insert a row to a table.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_insert_row(
    ib_crsr_t ib_crsr,      /*!< in/out: InnoDB cursor instance */
    const ib_tpl_t ib_tpl); /*!< in: tuple to insert */

/** Update a row in a table.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_update_row(
    ib_crsr_t ib_crsr,          /*!< in: InnoDB cursor instance */
    const ib_tpl_t ib_old_tpl,  /*!< in: Old tuple in table */
    const ib_tpl_t ib_new_tpl); /*!< in: New tuple to update */

/** Delete a row in a table.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_delete_row(ib_crsr_t ib_crsr); /*!< in: cursor instance */

/** Read current row.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_read_row(
    ib_crsr_t ib_crsr,   /*!< in: InnoDB cursor instance */
    ib_tpl_t ib_tpl,     /*!< out: read cols into this tuple */
    ib_tpl_t cmp_tpl,    /*!< in: tuple to compare and stop
                         reading */
    int mode,            /*!< in: mode determine when to
                         stop read */
    void **row_buf,      /*!< in/out: row buffer */
    uint64_t *row_len,   /*!< in/out: row buffer len */
    uint64_t *used_len); /*!< in/out: row buffer len used */

/** Move cursor to the first record in the table.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_first(ib_crsr_t ib_crsr); /*!< in: InnoDB cursor instance */

/** Move cursor to the next record in the table.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_next(ib_crsr_t ib_crsr); /*!< in: InnoDB cursor instance */

/** Search for key.
 @return DB_SUCCESS or err code */
ib_err_t ib_cursor_moveto(ib_crsr_t ib_crsr, /*!< in: InnoDB cursor instance */
                          ib_tpl_t ib_tpl,   /*!< in: Key to search for */
                          ib_srch_mode_t ib_srch_mode, /*!< in: search mode */
                          uint64_t direction); /*!< in: search direction */

/** Set the match mode for ib_cursor_move().
@param[in] ib_crsr Cursor instance
@param[in] match_mode ib_cursor_moveto match mode*/
void ib_cursor_set_match_mode(ib_crsr_t ib_crsr, ib_match_mode_t match_mode);

/** Set a column of the tuple. Make a copy using the tuple's heap.
 @param[in] ib_tpl tuple instance
 @param[in] col_no column index in tuple
 @param[in] src data value
 @param[in] len data value len
 @param[in] need_cpy if need memcpy
 @return DB_SUCCESS or error code */
ib_err_t ib_col_set_value(ib_tpl_t ib_tpl, ib_ulint_t col_no, const void *src,
                          uint64_t len, bool need_cpy);

/** Get the size of the data available in the column the tuple.
 @param[in] ib_tpl tuple instance
 @param[in] i column index in tuple
 @return bytes avail or IB_SQL_NULL */
uint64_t ib_col_get_len(ib_tpl_t ib_tpl, ib_ulint_t i);

/** Copy a column value from the tuple.
 @param[in] ib_tpl tuple instance
 @param[in] i in column index in tuple
 @param[out] dst copied data value
 @param[in] len max data value len to copy
 @return bytes copied or IB_SQL_NULL */
uint64_t ib_col_copy_value(ib_tpl_t ib_tpl, ib_ulint_t i, void *dst,
                           uint32_t len);

/** Read a signed int 8 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_i8(ib_tpl_t ib_tpl, ib_ulint_t i, int8_t *ival);

/** Read an unsigned int 8 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuplr
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_u8(ib_tpl_t ib_tpl, ib_ulint_t i, uint8_t *ival);

/** Read a signed int 16 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_i16(ib_tpl_t ib_tpl, ib_ulint_t i, int16_t *ival);

/** Read an unsigned int 16 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_u16(ib_tpl_t ib_tpl, ib_ulint_t i, uint16_t *ival);

/** Read a signed int 32 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_i32(ib_tpl_t ib_tpl, ib_ulint_t i, int32_t *ival);

/** Read an unsigned int 32 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_u32(ib_tpl_t ib_tpl, ib_ulint_t i, uint32_t *ival);

/** Read a signed int 64 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_i64(ib_tpl_t ib_tpl, ib_ulint_t i, int64_t *ival);

/** Read an unsigned int 64 bit column from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ival integer value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_u64(ib_tpl_t ib_tpl, ib_ulint_t i, uint64_t *ival);

/** Get a column value pointer from the tuple.
 @param[in] ib_tpl tuple instance
 @param[in] i column index in tuple
 @return NULL or pointer to buffer */
const void *ib_col_get_value(ib_tpl_t ib_tpl, ib_ulint_t i);

/** Get a column type, length and attributes from the tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] i column number
 @param[out] ib_col_meta column meta data
 @return len of column data */
uint64_t ib_col_get_meta(ib_tpl_t ib_tpl, ib_ulint_t i,
                         ib_col_meta_t *ib_col_meta);

/** "Clear" or reset an InnoDB tuple. We free the heap and recreate the tuple.
 @return new tuple, or NULL */
ib_tpl_t ib_tuple_clear(ib_tpl_t ib_tpl); /*!< in: InnoDB tuple */

/** Create a new cluster key search tuple and copy the contents of  the
 secondary index key tuple columns that refer to the cluster index record
 to the cluster key. It does a deep copy of the column data.
 @param[in] ib_crsr secondary index cursor
 @param[out] ib_dst_tpl destination tuple
 @param[in] ib_src_tpl source tuple
 @return DB_SUCCESS or error code */
ib_err_t ib_tuple_get_cluster_key(ib_crsr_t ib_crsr, ib_tpl_t *ib_dst_tpl,
                                  const ib_tpl_t ib_src_tpl);

/** Create an InnoDB tuple used for index/table search.
 @param[in] ib_crsr Cursor instance
 @return tuple for current index */
ib_tpl_t ib_sec_search_tuple_create(ib_crsr_t ib_crsr);

/** Create an InnoDB tuple used for index/table search.
 @param[in] ib_crsr Cursor instance
 @return tuple for current index */
ib_tpl_t ib_sec_read_tuple_create(ib_crsr_t ib_crsr);

/** Create an InnoDB tuple used for table key operations.
* @param[in] ib_crsr Cursor instance
 @return tuple for current table */
ib_tpl_t ib_clust_search_tuple_create(ib_crsr_t ib_crsr);

/** Create an InnoDB tuple for table row operations.
 @param[in] ib_crsr Cursor instance
 @return tuple for current table */
ib_tpl_t ib_clust_read_tuple_create(ib_crsr_t ib_crsr);

/** Return the number of user columns in the tuple definition.
 @param[in] ib_tpl Tuple for current table
 @return number of user columns */
uint64_t ib_tuple_get_n_user_cols(const ib_tpl_t ib_tpl);

/** Return the number of columns in the tuple definition.
 @param[in] ib_tpl Tuple for current table
 @return number of columns */
uint64_t ib_tuple_get_n_cols(const ib_tpl_t ib_tpl);

/** Destroy an InnoDB tuple.
 @param[in] ib_tpl Tuple for current table*/
void ib_tuple_delete(ib_tpl_t ib_tpl);

/** Get a table id.
 @param[in] table_name table_to_find
 @param[out] table_id table id if found
 @return DB_SUCCESS if found */
ib_err_t ib_table_get_id(const char *table_name, ib_id_u64_t *table_id);

/** Check if cursor is positioned.
 @param[in] ib_crsr InnoDB cursor instance
 @return IB_true if positioned */
bool ib_cursor_is_positioned(const ib_crsr_t ib_crsr);

/** Checks if the data dictionary is latched in exclusive mode by a
 user transaction.
 @param[in] ib_trx transaction
 @return true if exclusive latch */
bool ib_schema_lock_is_exclusive(const ib_trx_t ib_trx);

/** Lock an InnoDB cursor/table.
 @param[in,out] ib_crsr InnoDB cursor
 @param[in] ib_lck_mode InnoDB lock mode
 @return DB_SUCCESS or error code */
ib_err_t ib_cursor_lock(ib_crsr_t ib_crsr, ib_lck_mode_t ib_lck_mode);

/** Set the Lock mode of the cursor.
 @param[in,out] ib_crsr InnoDB cursor
 @param[in] ib_lck_mode InnoDB lock mode
 @return DB_SUCCESS or error code */
ib_err_t ib_cursor_set_lock_mode(ib_crsr_t ib_crsr, ib_lck_mode_t ib_lck_mode);

/** Set need to access clustered index record flag.
 @param[in,out] ib_crsr InnoDB cursor */
void ib_cursor_set_cluster_access(ib_crsr_t ib_crsr);

/** Inform the cursor that it's the start of an SQL statement.
 @param[in,out] ib_crsr InnoDB cursor */
void ib_cursor_stmt_begin(ib_crsr_t ib_crsr);

/** Write a double value to a column.
 @param[in] ib_tpl InnoDB tuple
 @param[in] col_no column number
 @param[in] val value to write
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_write_double(ib_tpl_t ib_tpl, int col_no, double val);

/** Read a double column value from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] col_no column number
 @param[out] dval double value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_double(ib_tpl_t ib_tpl, uint64_t col_no, double *dval);

/** Write a float value to a column.
 @param[in,out] ib_tpl tuple to write to
 @param[in] col_no column number
 @param[in] val value to write
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_write_float(ib_tpl_t ib_tpl, int col_no, float val);

/** Read a float value from an InnoDB tuple.
 @param[in] ib_tpl InnoDB tuple
 @param[in] col_no column number
 @param[out] fval float value
 @return DB_SUCCESS or error */
ib_err_t ib_tuple_read_float(ib_tpl_t ib_tpl, uint64_t col_no, float *fval);

/** Get a column type, length and attributes from the tuple.
 @param[in] ib_crsr InnoDB cursor instance
 @param[in] i column index in tuple
 @return len of column data */
const char *ib_col_get_name(ib_crsr_t ib_crsr, ib_ulint_t i);

/** Get an index field name from the cursor.
 @param[in] ib_crsr InnoDB cursor instance
 @param[in] i column index in tuple
 @return name of the field */
const char *ib_get_idx_field_name(ib_crsr_t ib_crsr, ib_ulint_t i);

/** Get generic configure status
 @return configure status*/
int ib_cfg_get_cfg();

/** Return isolation configuration set by "innodb_api_trx_level"
 @return trx isolation level*/
ib_trx_level_t ib_cfg_trx_level();

/** Return configure value for background commit interval (in seconds)
 @return background commit interval (in seconds) */
uint64_t ib_cfg_bk_commit_interval();

/** Get a trx start time.
 @return trx start_time */
uint64_t ib_trx_get_start_time(ib_trx_t ib_trx); /*!< in: transaction */

/** Wrapper of ut_strerr() which converts an InnoDB error number to a
 human readable text message.
 @return string, describing the error */
const char *ib_ut_strerr(ib_err_t num); /*!< in: error number */

/** Get the SDI keys in a tablespace into vector.
@param[in]      tablespace_id   tablespace id
@param[in,out]  ib_sdi_vector   vector to hold objects with tablespace types
and ids
@param[in,out]  trx             data dictionary transaction
@return DB_SUCCESS if SDI keys retrieval is successful, else error */
ib_err_t ib_sdi_get_keys(uint32_t tablespace_id, ib_sdi_vector_t *ib_sdi_vector,
                         ib_trx_t trx);

/** Retrieve SDI from tablespace
@param[in]      tablespace_id   tablespace id
@param[in]      ib_sdi_key      SDI key
@param[in,out]  comp_sdi        in: buffer to hold the SDI BLOB
                                out: compressed SDI retrieved from tablespace
@param[in,out]  comp_sdi_len    in:  Size of memory allocated
                                out: compressed length of SDI
@param[out]     uncomp_sdi_len  out: uncompressed length of SDI
@param[in,out]  trx             innodb transaction
@return DB_SUCCESS if SDI retrieval is successful, else error
in case the passed buffer length is smaller than the actual SDI
DB_OUT_OF_MEMORY is thrown and uncompressed length is set in
uncomp_sdi_len */
ib_err_t ib_sdi_get(uint32_t tablespace_id, const ib_sdi_key_t *ib_sdi_key,
                    void *comp_sdi, uint32_t *comp_sdi_len,
                    uint32_t *uncomp_sdi_len, ib_trx_t trx);

/** Insert/Update SDI in tablespace
@param[in]      tablespace_id   tablespace id
@param[in]      sdi_key         SDI key to uniquely identify the tablespace
                                object
@param[in]      uncomp_len      uncompressed length of SDI
@param[in]      comp_len        compressed length of SDI
@param[in]      sdi             compressed SDI to be stored in tablespace
@param[in,out]  trx             innodb transaction
@return DB_SUCCESS if SDI Insert/Update is successful, else error */
ib_err_t ib_sdi_set(uint32_t tablespace_id, const ib_sdi_key_t *sdi_key,
                    uint32_t uncomp_len, uint32_t comp_len, const void *sdi,
                    ib_trx_t trx);

/** Delete SDI from tablespace.
@param[in]      tablespace_id   tablespace id
@param[in]      sdi_key         SDI key to uniquely identify the tablespace
object
@param[in,out]  trx             innodb transaction
@return DB_SUCCESS if SDI deletion is successful, else error */
ib_err_t ib_sdi_delete(uint32_t tablespace_id, const ib_sdi_key_t *sdi_key,
                       ib_trx_t trx);

/** Create SDI in a tablespace
@param[in]      tablespace_id   InnoDB tablespace id
@return DB_SUCCESS if SDI index creation is successful, else error */
ib_err_t ib_sdi_create(space_id_t tablespace_id);

/** Drop SDI Index from tablespace. This should be used only when SDI
is corrupted.
@param[in]      tablespace_id   InnoDB tablespace id
@return DB_SUCCESS if dropping of SDI indexes is successful, else error */
ib_err_t ib_sdi_drop(space_id_t tablespace_id);

/** Flush SDI in a tablespace. The pages of a SDI Index modified by the
transaction will be flushed to disk.
@param[in]      space_id        tablespace id
@return DB_SUCCESS always */
ib_err_t ib_sdi_flush(space_id_t space_id);

/** Check the table whether it contains virtual columns.
@param[in]      crsr    InnoDB Cursor
@return true if the table contains virtual column else failure. */
bool ib_is_virtual_table(ib_crsr_t crsr);

#define FOR_EACH_API_METHOD_NAME_STEM(transform)   \
  transform(cursor_open_table)                /**/ \
      transform(cursor_read_row)              /**/ \
      transform(cursor_insert_row)            /**/ \
      transform(cursor_delete_row)            /**/ \
      transform(cursor_update_row)            /**/ \
      transform(cursor_moveto)                /**/ \
      transform(cursor_first)                 /**/ \
      transform(cursor_next)                  /**/ \
      transform(cursor_set_match_mode)        /**/ \
      transform(sec_search_tuple_create)      /**/ \
      transform(clust_read_tuple_create)      /**/ \
      transform(tuple_delete)                 /**/ \
      transform(tuple_read_u8)                /**/ \
      transform(tuple_read_u16)               /**/ \
      transform(tuple_read_u32)               /**/ \
      transform(tuple_read_u64)               /**/ \
      transform(tuple_read_i8)                /**/ \
      transform(tuple_read_i16)               /**/ \
      transform(tuple_read_i32)               /**/ \
      transform(tuple_read_i64)               /**/ \
      transform(tuple_get_n_cols)             /**/ \
      transform(col_set_value)                /**/ \
      transform(col_get_value)                /**/ \
      transform(col_get_meta)                 /**/ \
      transform(trx_begin)                    /**/ \
      transform(trx_commit)                   /**/ \
      transform(trx_rollback)                 /**/ \
      transform(trx_start)                    /**/ \
      transform(trx_release)                  /**/ \
      transform(cursor_lock)                  /**/ \
      transform(cursor_close)                 /**/ \
      transform(cursor_new_trx)               /**/ \
      transform(cursor_reset)                 /**/ \
      transform(col_get_name)                 /**/ \
      transform(cursor_open_index_using_name) /**/ \
      transform(cfg_get_cfg)                  /**/ \
      transform(cursor_set_cluster_access)    /**/ \
      transform(cursor_commit_trx)            /**/ \
      transform(cfg_trx_level)                /**/ \
      transform(tuple_get_n_user_cols)        /**/ \
      transform(cursor_set_lock_mode)         /**/ \
      transform(get_idx_field_name)           /**/ \
      transform(trx_get_start_time)           /**/ \
      transform(cfg_bk_commit_interval)       /**/ \
      transform(ut_strerr)                    /**/ \
      transform(cursor_stmt_begin)            /**/ \
      transform(trx_read_only)                /**/ \
      transform(is_virtual_table)             /**/

#endif /* api0api_h */
