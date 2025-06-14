/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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

#ifndef TRANSACTION_INFO_INCLUDED
#define TRANSACTION_INFO_INCLUDED

#include <stddef.h>
#include <sys/types.h>

#include "my_alloc.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"  // strmake_root
#include "sql/engine_combination_tracker.h"
#include "sql/mdl.h"                            // MDL_savepoint
#include "sql/rpl_transaction_ctx.h"            // Rpl_transaction_ctx
#include "sql/rpl_transaction_write_set_ctx.h"  // Transaction_write_set_ctx
#include "sql/xa.h"                             // XID_STATE
#include "strmake.h"

class Ha_trx_info;
class Ha_trx_info_list;
class THD;
struct handlerton;

struct SAVEPOINT {
  SAVEPOINT *prev;
  char *name;
  size_t length;
  Ha_trx_info *ha_list;
  /** State of metadata locks before this savepoint was set. */
  MDL_savepoint mdl_savepoint;
};

class Transaction_ctx {
 public:
  enum enum_trx_scope { STMT = 0, SESSION };

  SAVEPOINT *m_savepoints;

  void register_ha(enum_trx_scope scope, Ha_trx_info *ha_info, handlerton *ht);

 public:
  struct THD_TRANS {
    /* true is not all entries in the ht[] support 2pc */
    bool m_no_2pc;
    int m_rw_ha_count;
    /* storage engines that registered in this transaction */
    Ha_trx_info *m_ha_list;

   private:
    /*
      The purpose of this member variable (i.e. flag) is to keep track of
      statements which cannot be rolled back safely(completely).
      For example,

      * statements that modified non-transactional tables. The value
      MODIFIED_NON_TRANS_TABLE is set within mysql_insert, mysql_update,
      mysql_delete, etc if a non-transactional table is modified.

      * 'DROP TEMPORARY TABLE' and 'CREATE TEMPORARY TABLE' statements.
      The former sets the value DROPPED_TEMP_TABLE and the latter
      the value CREATED_TEMP_TABLE.

      The tracked statements are modified in scope of:

      * transaction, when the variable is a member of
      THD::m_transaction.m_scope_info[SESSION]

      * top-level statement or sub-statement, when the variable is a
      member of THD::m_transaction.m_scope_info[STMT]

      This member has the following life cycle:

      * m_scope_info[STMT].m_unsafe_rollback_flags is used to keep track of
      top-level statements which cannot be rolled back safely. At the end of the
      statement, the value of m_scope_info[STMT].m_unsafe_rollback_flags is
      merged with m_scope_info[SESSION].m_unsafe_rollback_flags
      and gets reset.

      * m_scope_info[SESSION].cannot_safely_rollback is reset at the end
      of transaction

      * Since we do not have a dedicated context for execution of
      a sub-statement, to keep track of non-transactional changes in a
      sub-statement, we re-use m_scope_info[STMT].m_unsafe_rollback_flags.
      At entrance into a sub-statement, a copy of the value of
      m_scope_info[STMT].m_unsafe_rollback_flags (containing the changes of the
      outer statement) is saved on stack.
      Then m_scope_info[STMT].m_unsafe_rollback_flags is reset to 0 and the
      substatement is executed. Then the new value is merged
      with the saved value.
    */

    unsigned int m_unsafe_rollback_flags;
    /*
      Define the type of statements which cannot be rolled back safely.
      Each type occupies one bit in m_unsafe_rollback_flags.
    */
    static unsigned int const MODIFIED_NON_TRANS_TABLE = 0x01;
    static unsigned int const CREATED_TEMP_TABLE = 0x02;
    static unsigned int const DROPPED_TEMP_TABLE = 0x04;

   public:
    bool cannot_safely_rollback() const { return m_unsafe_rollback_flags > 0; }
    unsigned int get_unsafe_rollback_flags() const {
      return m_unsafe_rollback_flags;
    }
    void set_unsafe_rollback_flags(unsigned int flags) {
      DBUG_PRINT("debug", ("set_unsafe_rollback_flags: %u", flags));
      m_unsafe_rollback_flags = flags;
    }
    void add_unsafe_rollback_flags(unsigned int flags) {
      DBUG_PRINT("debug", ("add_unsafe_rollback_flags: %u", flags));
      m_unsafe_rollback_flags |= flags;
    }
    void reset_unsafe_rollback_flags() {
      DBUG_PRINT("debug", ("reset_unsafe_rollback_flags"));
      m_unsafe_rollback_flags = 0;
    }
    void mark_modified_non_trans_table() {
      DBUG_PRINT("debug", ("mark_modified_non_trans_table"));
      m_unsafe_rollback_flags |= MODIFIED_NON_TRANS_TABLE;
    }
    bool has_modified_non_trans_table() const {
      return m_unsafe_rollback_flags & MODIFIED_NON_TRANS_TABLE;
    }
    void mark_created_temp_table() {
      DBUG_PRINT("debug", ("mark_created_temp_table"));
      m_unsafe_rollback_flags |= CREATED_TEMP_TABLE;
    }
    bool has_created_temp_table() const {
      return m_unsafe_rollback_flags & CREATED_TEMP_TABLE;
    }
    void mark_dropped_temp_table() {
      DBUG_PRINT("debug", ("mark_dropped_temp_table"));
      m_unsafe_rollback_flags |= DROPPED_TEMP_TABLE;
    }
    bool has_dropped_temp_table() const {
      return m_unsafe_rollback_flags & DROPPED_TEMP_TABLE;
    }

    void reset() {
      m_no_2pc = false;
      m_rw_ha_count = 0;
      reset_unsafe_rollback_flags();
    }
    bool is_empty() const { return m_ha_list == nullptr; }
  };

 private:
  THD_TRANS m_scope_info[2];

  XID_STATE m_xid_state;

  MEM_ROOT m_mem_root;  // Transaction-life memory allocation pool

 public:
  /*
    (Mostly) binlog-specific fields use while flushing the caches
    and committing transactions.
    We don't use bitfield any more in the struct. Modification will
    be lost when concurrently updating multiple bit fields. It will
    cause a race condition in a multi-threaded application. And we
    already caught a race condition case between xid_written and
    ready_preempt in MYSQL_BIN_LOG::ordered_commit.
  */
  struct {
    bool enabled{false};      // see ha_enable_transaction()
    bool xid_written{false};  // The session wrote an XID
    bool real_commit{false};  // Is this a "real" commit?
    bool commit_low{false};   // see MYSQL_BIN_LOG::ordered_commit
    bool run_hooks{false};    // Call the after_commit hook
#ifndef NDEBUG
    bool ready_preempt{false};  // internal in MYSQL_BIN_LOG::ordered_commit
#endif
  } m_flags;
  /* Binlog-specific logical timestamps. */
  /*
    Store for the transaction's commit parent sequence_number.
    The value specifies this transaction dependency with a "parent"
    transaction.
    The member is assigned, when the transaction is about to commit
    in binlog to a value of the last committed transaction's sequence_number.
    This and last_committed as numbers are kept ever incremented
    regardless of binary logs being rotated or when transaction
    is logged in multiple pieces.
    However the logger to the binary log may convert them
    according to its specification.
  */
  int64 last_committed;
  /*
    The transaction's private logical timestamp assigned at the
    transaction prepare phase. The timestamp enumerates transactions
    in the binary log. The value is gained through incrementing (stepping) a
    global clock.
    Eventually the value is considered to increase max_committed_transaction
    system clock when the transaction has committed.
  */
  int64 sequence_number;

  void store_commit_parent(int64 last_arg) { last_committed = last_arg; }

  Transaction_ctx();
  virtual ~Transaction_ctx() { m_mem_root.Clear(); }

  void cleanup() {
    DBUG_TRACE;
    m_savepoints = nullptr;
    m_xid_state.cleanup();
    m_rpl_transaction_ctx.cleanup();
    m_transaction_write_set_ctx.reset_state();
    trans_begin_hook_invoked = false;
    m_mem_root.ClearForReuse();

    // Reset the flag indicating the warning of mixing transactional engine
    // with non-transactional engine is always emitted per transaction
    tracker.set_warning_already_emitted(false);

    /// Clear the registered engine
    tracker.clear_known_engine();
    return;
  }

  bool is_active(enum_trx_scope scope) const {
    return m_scope_info[scope].m_ha_list != nullptr;
  }

  void push_unsafe_rollback_warnings(THD *thd);

  void merge_unsafe_rollback_flags() {
    /*
      Merge m_scope_info[STMT].unsafe_rollback_flags to
      m_scope_info[SESSION].unsafe_rollback_flags. If the statement
      cannot be rolled back safely, the transaction including
      this statement definitely cannot rolled back safely.
    */
    m_scope_info[SESSION].add_unsafe_rollback_flags(
        m_scope_info[STMT].get_unsafe_rollback_flags());
  }

  void init_mem_root_defaults(ulong trans_alloc_block_size, ulong) {
    m_mem_root.set_block_size(trans_alloc_block_size);
  }

  MEM_ROOT *transaction_memroot() { return &m_mem_root; }

  void *allocate_memory(unsigned int size) { return m_mem_root.Alloc(size); }

  void claim_memory_ownership(bool claim) { m_mem_root.Claim(claim); }

  void free_memory() { m_mem_root.Clear(); }

  char *strmake(const char *str, size_t len) {
    return strmake_root(&m_mem_root, str, len);
  }

  void invalidate_changed_tables_in_cache(THD *thd);

  void add_changed_table(const char *key, uint32 key_length);

  Ha_trx_info_list ha_trx_info(enum_trx_scope scope);

  void set_ha_trx_info(enum_trx_scope scope, Ha_trx_info *trx_info) {
    DBUG_TRACE;
    m_scope_info[scope].m_ha_list = trx_info;
    return;
  }

  XID_STATE *xid_state() { return &m_xid_state; }

  const XID_STATE *xid_state() const { return &m_xid_state; }

  bool cannot_safely_rollback(enum_trx_scope scope) const {
    return m_scope_info[scope].cannot_safely_rollback();
  }

  unsigned int get_unsafe_rollback_flags(enum_trx_scope scope) const {
    return m_scope_info[scope].get_unsafe_rollback_flags();
  }

  void set_unsafe_rollback_flags(enum_trx_scope scope, unsigned int flags) {
    m_scope_info[scope].set_unsafe_rollback_flags(flags);
  }

  void add_unsafe_rollback_flags(enum_trx_scope scope, unsigned int flags) {
    m_scope_info[scope].add_unsafe_rollback_flags(flags);
  }

  void reset_unsafe_rollback_flags(enum_trx_scope scope) {
    m_scope_info[scope].reset_unsafe_rollback_flags();
  }

  void mark_modified_non_trans_table(enum_trx_scope scope) {
    m_scope_info[scope].mark_modified_non_trans_table();
  }

  bool has_modified_non_trans_table(enum_trx_scope scope) const {
    return m_scope_info[scope].has_modified_non_trans_table();
  }

  void mark_created_temp_table(enum_trx_scope scope) {
    m_scope_info[scope].mark_created_temp_table();
  }

  bool has_created_temp_table(enum_trx_scope scope) const {
    return m_scope_info[scope].has_created_temp_table();
  }

  void mark_dropped_temp_table(enum_trx_scope scope) {
    m_scope_info[scope].mark_dropped_temp_table();
  }

  bool has_dropped_temp_table(enum_trx_scope scope) const {
    return m_scope_info[scope].has_dropped_temp_table();
  }

  void reset(enum_trx_scope scope) { m_scope_info[scope].reset(); }

  bool is_empty(enum_trx_scope scope) const {
    return m_scope_info[scope].is_empty();
  }

  void set_no_2pc(enum_trx_scope scope, bool value) {
    m_scope_info[scope].m_no_2pc = value;
  }

  bool no_2pc(enum_trx_scope scope) const {
    return m_scope_info[scope].m_no_2pc;
  }

  int rw_ha_count(enum_trx_scope scope) const {
    return m_scope_info[scope].m_rw_ha_count;
  }

  void set_rw_ha_count(enum_trx_scope scope, int value) {
    m_scope_info[scope].m_rw_ha_count = value;
  }

  void reset_scope(enum_trx_scope scope) {
    DBUG_TRACE;
    m_scope_info[scope].m_ha_list = nullptr;
    m_scope_info[scope].m_no_2pc = false;
    m_scope_info[scope].m_rw_ha_count = 0;
    return;
  }

  Rpl_transaction_ctx *get_rpl_transaction_ctx() {
    return &m_rpl_transaction_ctx;
  }

  const Rpl_transaction_ctx *get_rpl_transaction_ctx() const {
    return &m_rpl_transaction_ctx;
  }

  Rpl_transaction_write_set_ctx *get_transaction_write_set_ctx() {
    return &m_transaction_write_set_ctx;
  }

  const Rpl_transaction_write_set_ctx *get_transaction_write_set_ctx() const {
    return &m_transaction_write_set_ctx;
  }

  bool was_trans_begin_hook_invoked() { return trans_begin_hook_invoked; }

  void set_trans_begin_hook_invoked() { trans_begin_hook_invoked = true; }

 private:
  Rpl_transaction_ctx m_rpl_transaction_ctx;
  Rpl_transaction_write_set_ctx m_transaction_write_set_ctx;
  bool trans_begin_hook_invoked;

 public:
  Engine_combination_tracker tracker;
};

/**
  Either statement transaction or normal transaction - related
  thread-specific storage engine data.

  If a storage engine participates in a statement/transaction,
  an instance of this class is present in
  thd->m_transaction.m_scope_info[STMT|SESSION].ha_list. The addition
  this list is made by trans_register_ha().

  When it's time to commit or rollback, each element of ha_list
  is used to access storage engine's prepare()/commit()/rollback()
  methods, and also to evaluate if a full two phase commit is
  necessary.

  @sa General description of transaction handling in handler.cc.
*/

class Ha_trx_info {
 public:
  friend class Ha_trx_info_list;

  /**
    Register this storage engine in the given transaction context.
  */
  void register_ha(Transaction_ctx::THD_TRANS *trans, handlerton *ht_arg) {
    DBUG_TRACE;
    assert(m_flags == 0);
    assert(m_ht == nullptr);
    assert(m_next == nullptr);

    m_ht = ht_arg;
    m_flags = (int)TRX_READ_ONLY; /* Assume read-only at start. */

    if (trans->m_ha_list != this) {
      m_next = trans->m_ha_list;
      trans->m_ha_list = this;
    }

    return;
  }

  /**
    Clear, prepare for reuse.
  */

  void reset() {
    DBUG_TRACE;
    m_next = nullptr;
    m_ht = nullptr;
    m_flags = 0;
    return;
  }

  Ha_trx_info() { reset(); }

  void set_trx_read_write() {
    assert(is_started());
    m_flags |= (int)TRX_READ_WRITE;
  }

  bool is_trx_read_write() const {
    assert(is_started());
    return m_flags & (int)TRX_READ_WRITE;
  }

  bool is_started() const { return m_ht != nullptr; }

  /**
    Mark this transaction read-write if the argument is read-write.
  */

  void coalesce_trx_with(const Ha_trx_info *stmt_trx) {
    this->coalesce_trx_with(*stmt_trx);
  }

  void coalesce_trx_with(const Ha_trx_info &stmt_trx) {
    /*
      Must be called only after the transaction has been started.
      Can be called many times, e.g. when we have many
      read-write statements in a transaction.
    */
    assert(is_started());
    if (stmt_trx.is_trx_read_write()) set_trx_read_write();
  }

  handlerton *ht() const {
    assert(is_started());
    return m_ht;
  }

 private:
  enum { TRX_READ_ONLY = 0, TRX_READ_WRITE = 1 };
  /**
    Auxiliary, used for ha_list management
  */
  Ha_trx_info *m_next;

  /**
    Although a given Ha_trx_info instance is currently always used
    for the same storage engine, 'ht' is not-NULL only when the
    corresponding storage is a part of a transaction.
  */
  handlerton *m_ht;

  /**
    Transaction flags related to this engine.
    Not-null only if this instance is a part of transaction.
    May assume a combination of enum values above.
  */
  uchar m_flags;
};

/**
  @class Ha_trx_info_list

  Container to hold and allow iteration over a set of Ha_trx_info objects.
 */
class Ha_trx_info_list {
 public:
  /**
   @class Iterator

   Implements a forward iterator for `Ha_trx_info_list`. The
   `Ha_trx_info_list` methods `begin` and `end` complete the requirements
   for algorithms usage.

   Since the container this iterator targets is a linked-list where the
   list and the list elements are the same, the invalidation rules are not
   the ones usually encontered in iterator classes. Invoking
   `Ha_trx_info::reset()`, which clears the pointer to next element in the
   list, doesn't invalidate the iterator, instead the pointer reference is
   kept by the iterator in order to allow the requirements for forward
   iterating to be valid. Therefore, although `Ha_trx_info::reset()`
   removes the element from the list, the iterator is no invalidated and
   iteration over the rest of the element is kept.
 */
  class Iterator {
   public:
    using difference_type = std::ptrdiff_t;
    using pointer = Ha_trx_info *;
    using reference = Ha_trx_info &;
    using iterator_category = std::forward_iterator_tag;

    Iterator(Ha_trx_info *parent);
    Iterator(std::nullptr_t);
    Iterator(Iterator const &rhs);
    virtual ~Iterator() = default;

    // BASIC ITERATOR METHODS //
    Iterator &operator=(const Iterator &rhs);
    Iterator &operator++();
    reference operator*() const;
    // END / BASIC ITERATOR METHODS //

    // INPUT ITERATOR METHODS //
    Iterator operator++(int);
    pointer operator->() const;
    bool operator==(Iterator const &rhs) const;
    bool operator==(Ha_trx_info const *rhs) const;
    bool operator==(Ha_trx_info const &rhs) const;
    bool operator!=(Iterator const &rhs) const;
    bool operator!=(Ha_trx_info const *rhs) const;
    bool operator!=(Ha_trx_info const &rhs) const;
    // END / INPUT ITERATOR METHODS //

    // OUTPUT ITERATOR METHODS //
    // reference operator*() const; <- already defined
    // iterator operator++(int); <- already defined
    // END / OUTPUT ITERATOR METHODS //

    // FORWARD ITERATOR METHODS //
    // Enable support for both input and output iterator
    // END / FORWARD ITERATOR METHODS //

   private:
    /** Item this iterator is currently pointing to  */
    Ha_trx_info *m_current{nullptr};
    /** Next item in the list  */
    Ha_trx_info *m_next{nullptr};

    Iterator &set_next();
  };

  /**
    Default constructor.
   */
  Ha_trx_info_list() = default;
  /**
    Class constructor that instantiates the underlying head of the list
    with the parameter.

    @param rhs The pointer to initialize the underlying list head with.
   */
  Ha_trx_info_list(Ha_trx_info *rhs);
  /**
    Copy constructor.

    @param rhs The object instance to copy content from.
   */
  Ha_trx_info_list(Ha_trx_info_list const &rhs);
  /**
    Move constructor.

    @param rhs The object instance to move content from.
   */
  Ha_trx_info_list(Ha_trx_info_list &&rhs);
  virtual ~Ha_trx_info_list() = default;

  /**
    Copy operator.

    @param rhs The object instance to copy content from.

    @return this object reference, for chaining puposes.
   */
  Ha_trx_info_list &operator=(Ha_trx_info_list const &rhs);
  /**
    Move operator.

    @param rhs The object instance to move content from.

    @return this object reference, for chaining puposes.
   */
  Ha_trx_info_list &operator=(Ha_trx_info_list &&rhs);
  /**
    Retrieves the reference to the undelying head of the list.

    @return The reference to the undelying head of the list.
   */
  Ha_trx_info &operator*();
  /**
    Retrieves the reference to the undelying head of the list.

    @return The reference to the undelying head of the list.
   */
  Ha_trx_info const &operator*() const;
  /**
    Retrieves the pointer to the undelying head of the list.

    @return The pointer to the undelying head of the list.
   */
  Ha_trx_info *operator->();
  /**
    Retrieves the pointer to the undelying head of the list.

    @return The pointer to the undelying head of the list.
   */
  Ha_trx_info const *operator->() const;
  /**
    Equality operator that compares with another instance of this class. It
    evaluates to true if both object's underlying head point to the same
    address.

    @param rhs The object to compare this object to.

    @return true if both object's underlying head point to the same
            address, false otherwise.
   */
  bool operator==(Ha_trx_info_list const &rhs) const;
  /**
    Equality operator that compares with an instance of Ha_trx_info
    class. It evaluates to true if this object's underlying head points to
    the same address of the parameter object.

    @param rhs The object to compare this object to.

    @return true if this object's underlying head point to the same address
            as the parameter object, false otherwise.
   */
  bool operator==(Ha_trx_info const *rhs) const;
  /**
    Equality operator that compares with null. It evaluates to true if this
    object's underlying head points to null.

    @param rhs The `nullptr` value

    @return true if this object's underlying head point to null, false
            otherwise.
   */
  bool operator==(std::nullptr_t rhs) const;
  /**
    Inequality operator that compares with another instance of this
    class. It evaluates to true if both object's underlying head point to
    the different addresses.

    @param rhs The object to compare this object to.

    @return true if both object's underlying head point to different
            addresses, false otherwise.
   */
  bool operator!=(Ha_trx_info_list const &rhs) const;
  /**
    Inequality operator that compares with an instance of Ha_trx_info
    class. It evaluates to true if this object's underlying head points to
    a different address of the parameter object.

    @param rhs The object to compare this object to.

    @return true if this object's underlying head point to different
            address as the parameter object, false otherwise.
   */
  bool operator!=(Ha_trx_info const *rhs) const;
  /**
    Inequality operator that compares with null. It evaluates to true if
    this object's underlying head points to a non-null value.

    @param rhs The `nullptr` value

    @return true if this object's underlying head point to a non-null
            value, false otherwise.
   */
  bool operator!=(std::nullptr_t rhs) const;
  /**
    Cast operator to `bool`. It returns true if the this object underlying
    list head doesn't point to null, false otherwise.

    @return true if the this object underlying list head doesn't point to
            null, false otherwise.
   */
  operator bool() const;
  /**
    Retrieves the pointer to the underlying list head.

    @return The underlying list head.
   */
  Ha_trx_info *head();
  /**
    Retrieves an iterator pointing to the underlying list head.

    @return An iterator pointing to the underlying list head.
   */
  Iterator begin();
  /**
    Retrieves an iterator pointing to the underlying list head.

    @return An iterator pointing to the underlying list head.
   */
  const Iterator begin() const;
  /**
    Retrieves an iterator pointing to null.

    @return An iterator pointing null.
   */
  Iterator end();
  /**
    Retrieves an iterator pointing to null.

    @return An iterator pointing null.
   */
  const Iterator end() const;

 private:
  /** The head of the list */
  Ha_trx_info *m_underlying{nullptr};
};

#endif
