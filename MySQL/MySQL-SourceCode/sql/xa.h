/*
   Copyright (c) 2013, 2025, Oracle and/or its affiliates.

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

#ifndef XA_H_INCLUDED
#define XA_H_INCLUDED

#include <string.h>
#include <sys/types.h>
#include <list>
#include <mutex>

#include "lex_string.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sqlcommand.h"
#include "mysql/binlog/event/control_events.h"  // XA_prepare_event
#include "sql/malloc_allocator.h"               // Malloc_allocator
#include "sql/psi_memory_key.h"  // key_memory_xa_recovered_transactions
#include "sql/sql_cmd.h"         // Sql_cmd
#include "sql/sql_list.h"        // List
#include "sql/sql_plugin_ref.h"  // plugin_ref
#include "sql/xa_aux.h"          // serialize_xid

class Protocol;
class THD;
struct xid_t;
class XID_STATE;
class Transaction_ctx;

typedef int64 query_id_t;

enum xa_option_words {
  XA_NONE,
  XA_JOIN,
  XA_RESUME,
  XA_ONE_PHASE,
  XA_SUSPEND,
  XA_FOR_MIGRATE
};

static const int TC_HEURISTIC_NOT_USED = 0;
static const int TC_HEURISTIC_RECOVER_COMMIT = 1;
static const int TC_HEURISTIC_RECOVER_ROLLBACK = 2;

typedef ulonglong my_xid;  // this line is the same as in log_event.h
#define MYSQL_XID_PREFIX "MySQLXid"

/*
 Same as MYSQL_XIDDATASIZE but we do not want to include plugin.h here
 See static_assert in .cc file.
*/
#define XIDDATASIZE 128

/**
  struct xid_t is binary compatible with the XID structure as
  in the X/Open CAE Specification, Distributed Transaction Processing:
  The XA Specification, X/Open Company Ltd., 1991.
  http://www.opengroup.org/bookstore/catalog/c193.htm

  @see MYSQL_XID in mysql/plugin.h
*/
typedef struct xid_t {
 private:
  /**
    -1 means that the XID is null
  */
  long formatID;

  /**
    value from 1 through 64
  */
  long gtrid_length;

  /**
    value from 1 through 64
  */
  long bqual_length;

  /**
    distributed trx identifier. not \0-terminated.
  */
  char data[XIDDATASIZE];

 public:
  xid_t() : formatID(-1), gtrid_length(0), bqual_length(0) {
    memset(data, 0, XIDDATASIZE);
  }

  long get_format_id() const { return formatID; }

  void set_format_id(long v) {
    DBUG_TRACE;
    DBUG_PRINT("debug", ("SETTING XID_STATE formatID: %ld", v));
    formatID = v;
    return;
  }

  long get_gtrid_length() const { return gtrid_length; }

  void set_gtrid_length(long v) { gtrid_length = v; }

  long get_bqual_length() const { return bqual_length; }

  void set_bqual_length(long v) { bqual_length = v; }

  const char *get_data() const { return data; }

  void set_data(const void *v, long l) {
    assert(l <= XIDDATASIZE);
    memcpy(data, v, l);
  }

  void reset() {
    formatID = -1;
    gtrid_length = 0;
    bqual_length = 0;
    memset(data, 0, XIDDATASIZE);
  }

  void set(long f, const char *g, long gl, const char *b, long bl) {
    DBUG_TRACE;
    DBUG_PRINT("debug", ("SETTING XID_STATE formatID: %ld", f));
    formatID = f;
    memcpy(data, g, gtrid_length = gl);
    bqual_length = bl;
    if (bl > 0) memcpy(data + gl, b, bl);
    return;
  }

  my_xid get_my_xid() const;

  uchar *key() { return reinterpret_cast<uchar *>(&gtrid_length); }

  const uchar *key() const {
    return reinterpret_cast<const uchar *>(&gtrid_length);
  }

  uint key_length() const {
    return sizeof(gtrid_length) + sizeof(bqual_length) + gtrid_length +
           bqual_length;
  }

  /*
    The size of the string containing serialized Xid representation
    is computed as a sum of
      eight as the number of formatting symbols (X'',X'',)
      plus 2 x XIDDATASIZE (2 due to hex format),
      plus space for decimal digits of XID::formatID,
      plus one for 0x0.
   */
  static const uint ser_buf_size = 8 + 2 * XIDDATASIZE + 4 * sizeof(long) + 1;

  /**
     The method fills XID in a buffer in format of GTRID,BQUAL,FORMATID
     where GTRID, BQUAL are represented as hex strings.

     @param  buf  a pointer to buffer
     @return the value of the first argument
  */

  char *serialize(char *buf) const {
    return serialize_xid(buf, formatID, gtrid_length, bqual_length, data);
  }

#ifndef NDEBUG
  /**
     Get printable XID value.

     @param buf  pointer to the buffer where printable XID value has to be
     stored

     @return  pointer to the buffer passed in the first argument
  */
  char *xid_to_str(char *buf) const;
#endif
  /**
    Check if equal to another xid.

    @param[in]  xid   the id of another X/Open XA transaction

    @return true iff formats, gtrid_length, bqual_length and the content of
            gtrid_length+bqual_length bytes is exactly the same
  */
  bool eq(const xid_t *xid) const {
    return xid->formatID == formatID && xid->gtrid_length == gtrid_length &&
           xid->bqual_length == bqual_length &&
           !memcmp(xid->data, data, gtrid_length + bqual_length);
  }

  bool is_null() const { return formatID == -1; }

  /**
    Instantiates this object with the contents of the parameter of type
    `XA_prepare_event::MY_XID`.

    The `XA_prepare_event::MY_XID` is a mirror class of `xid_t` so the
    instantiation is a direct instantiation relation of each class member
    variables.

    @param rhs The `XA_prepare_event::MY_XID` to instantiate from

    @return This object reference.
   */
  xid_t &operator=(mysql::binlog::event::XA_prepare_event::MY_XID const &rhs);
  /**
    Compares for equality two instances of `xid_t`.

    @param rhs The instance to compare this object with.

    @return true if both instance are equal, false otherwise.
   */
  bool operator==(struct xid_t const &rhs) const;
  /**
    Compares for inequality two instances of `xid_t`.

    @param rhs The instance to compare this object with.

    @return true if both instance are different, false otherwise.
   */
  bool operator!=(struct xid_t const &rhs) const;
  /**
    Compares for lower-than-inequality two instances of `xid_t`.

    The lesser-than relation between two given XIDs, x1 and x2, is as
    follows:

    x1 < x2 if any of the following is true:
    - x1[FORMAT_ID] < x2[FORMAT_ID]
    - x1[strlen(GTRID)] < x2[strlen(GTRID)]
    - x1[strlen(BQUAL)] < x2[strlen(BQUAL)]
    - std::strncmp(x1[DATA], x2[DATA]) < 0

    @param rhs The instance to compare this object with.

    @return true if this instance is lesser than the parameter, false
            otherwise.
   */
  bool operator<(struct xid_t const &rhs) const;
  /**
    Writes the parameter's `in` string representation to the `out` stream
    parameter object.

    @param out The stream to write the XID representation to
    @param in  The XID for which the string representation should be written

    @return The reference for the stream passed on as parameter.
   */
  friend std::ostream &operator<<(std::ostream &out, struct xid_t const &in);

 private:
  void set(const xid_t *xid) {
    memcpy(this, xid, sizeof(xid->formatID) + xid->key_length());
  }

  void set(my_xid xid);

  void null() { formatID = -1; }

  friend class XID_STATE;
} XID;

struct st_handler_tablename;

/**
  Plain structure to store information about XA transaction id
  and a list of table names involved into XA transaction with
  specified id.
*/
typedef struct st_xarecover_txn {
  XID id;
  List<st_handler_tablename> *mod_tables;
} XA_recover_txn;

class XID_STATE {
 public:
  enum xa_states {
    XA_NOTR = 0,
    XA_ACTIVE,
    XA_IDLE,
    XA_PREPARED,
    XA_ROLLBACK_ONLY
  };

  /**
     Transaction identifier.
     For now, this is only used to catch duplicated external xids.
  */
 private:
  static const char *xa_state_names[];

  XID m_xid;
  /**
    This mutex used for eliminating a possibility to run two
    XA COMMIT/XA ROLLBACK statements concurrently against the same xid value.
    m_xa_lock is used on handling XA COMMIT/XA ROLLBACK and acquired only for
    external XA branches.
  */
  std::mutex m_xa_lock;

  /// Used by external XA only
  xa_states xa_state;
  bool m_is_detached = false;
  /// Error reported by the Resource Manager (RM) to the Transaction Manager.
  uint rm_error;
  /*
    XA-prepare binary logging status. The flag serves as a facility
    to conduct XA transaction two round binary logging.
    It is set to @c false at XA-start.
    It is set to @c true by binlogging routine of XA-prepare handler as well
    as recovered to @c true at the server recovery upon restart.
    Checked and reset at XA-commit/rollback.
  */
  bool m_is_binlogged;

 public:
  XID_STATE() : xa_state(XA_NOTR), rm_error(0), m_is_binlogged(false) {
    m_xid.null();
  }

  std::mutex &get_xa_lock() { return m_xa_lock; }

  void set_state(xa_states state) { xa_state = state; }

  enum xa_states get_state() { return xa_state; }

  bool has_state(xa_states state) const { return xa_state == state; }

  const char *state_name() const { return xa_state_names[xa_state]; }

  const XID *get_xid() const { return &m_xid; }

  XID *get_xid() { return &m_xid; }

  bool has_same_xid(const XID *xid) const { return m_xid.eq(xid); }

  void set_query_id(query_id_t query_id) {
    if (m_xid.is_null()) m_xid.set(query_id);
  }

  void set_error(THD *thd);

  void reset_error() { rm_error = 0; }

  void cleanup() {
    /*
      If rm_error is raised, it means that this piece of a distributed
      transaction has failed and must be rolled back. But the user must
      rollback it explicitly, so don't start a new distributed XA until
      then.
    */
    if (!rm_error) m_xid.null();
  }

  void reset() {
    xa_state = XA_NOTR;
    m_xid.null();
    m_is_detached = false;
    m_is_binlogged = false;
  }

  void start_normal_xa(const XID *xid) {
    assert(m_xid.is_null());
    xa_state = XA_ACTIVE;
    m_xid.set(xid);
    m_is_detached = false;
    rm_error = 0;
  }

  void start_detached_xa(const XID *xid, bool binlogged_arg = false) {
    xa_state = XA_PREPARED;
    m_xid.set(xid);
    m_is_detached = true;
    rm_error = 0;
    m_is_binlogged = binlogged_arg;
  }

  bool is_detached() const { return m_is_detached; }

  bool is_binlogged() const { return m_is_binlogged; }

  void set_binlogged() { m_is_binlogged = true; }

  void unset_binlogged() { m_is_binlogged = false; }

  void store_xid_info(Protocol *protocol, bool print_xid_as_hex) const;

  /**
     Mark a XA transaction as rollback-only if the RM unilaterally
     rolled back the transaction branch.

     @note If a rollback was requested by the RM, this function sets
           the appropriate rollback error code and transits the state
           to XA_ROLLBACK_ONLY.

     @return true if transaction was rolled back or if the transaction
             state is XA_ROLLBACK_ONLY. false otherwise.
  */

  bool xa_trans_rolled_back();

  /**
    Check that XA transaction is in state IDLE or PREPARED.

    @param  report_error  true if state IDLE or PREPARED has to be interpreted
                          as an error, else false

    @return  result of check
      @retval  false  XA transaction is NOT in state IDLE or PREPARED
      @retval  true   XA transaction is in state IDLE or PREPARED
  */

  bool check_xa_idle_or_prepared(bool report_error) const;

  /**
    Check that XA transaction has an uncommitted work. Report an error
    to a mysql user in case when there is an uncommitted work for XA
    transaction.

    @return  result of check
      @retval  false  XA transaction is NOT in state IDLE, PREPARED
                      or ROLLBACK_ONLY.
      @retval  true   XA transaction is in state IDLE or PREPARED
                      or ROLLBACK_ONLY.
  */

  bool check_has_uncommitted_xa() const;

  /**
    Check if an XA transaction has been started.

    @param  report_error  true if report an error in case when
                          XA transaction has been stared, else false.

    @return  result of check
      @retval  false  XA transaction hasn't been started (XA_NOTR)
      @retval  true   XA transaction has been started (!XA_NOTR)
  */

  bool check_in_xa(bool report_error) const;
};

/**
  This class servers as a registry for prepared XA transactions existed before
  server was shutdown and being resurrected during the server restart.
  The class is singleton. To collect a list of XA transaction identifiers and
  a list of tables for that MDL locks have be acquired the method
  add_prepared_xa_transaction() must be called. This method is invoked by
  the function trx_recover_for_mysql() called by innobase_xa_recover during
  running of X/Open XA distributed transaction recovery procedure. After a list
  of XA transaction identifiers and a list of table names to be locked in MDL
  have been collected and the function ha_recover() has returned control flow
  the method recover_prepared_xa_transactions() must be called to resurrect
  prepared XA transactions. Separation of collecting information about prepared
  XA transactions from restoring XA transactions is done in order to exclude
  possible suspending on MDL locks inside the function
  dd::reset_tables_and_tablespaces() that is called right after the function
  ha_recover() returns control flow.
 */

class Recovered_xa_transactions {
 public:
  /**
    Initialize singleton.
  */
  static bool init();

  /**
    Cleanup and delete singleton object
  */
  static void destroy();

  /**
    Get instance of the class Recovered_xa_transactions
  */
  static Recovered_xa_transactions &instance();

  /**
    Add information about prepared XA transaction into a list of
    XA transactions to resurrect.

    @param prepared_xa_trn  information about prepared XA transaction

    @return false on success, else true
  */
  bool add_prepared_xa_transaction(XA_recover_txn const *prepared_xa_trn);

  /**
    Iterate along a list of prepared XA transactions, register every XA
    transaction in a cache and acquire MDL locks for every table taking part in
    XA transaction being resurrected.

    @return false on success, else true
  */
  bool recover_prepared_xa_transactions();

  /**
    Get initialized MEM_ROOT.

    @return Pointer to a initialized MEM_ROOT.
  */
  MEM_ROOT *get_allocated_memroot();

 private:
  // Disable direct instantiation. Use the method init() instead.
  Recovered_xa_transactions();

  static Recovered_xa_transactions *m_instance;
  std::list<XA_recover_txn *, Malloc_allocator<XA_recover_txn *>>
      m_prepared_xa_trans;
  bool m_mem_root_inited;
  MEM_ROOT m_mem_root;
};

/**
  This is a specific to "slave" applier collection of standard cleanup
  actions to reset XA transaction state at the end of XA prepare rather than
  to do it at the transaction commit, see @c ha_commit_one_phase.
  THD of the slave applier is dissociated from a transaction object in engine
  that continues to exist there.

  @param  thd current thread
  @return the value of is_error()
*/

bool applier_reset_xa_trans(THD *thd);

/* interface to randomly access plugin data */
struct st_plugin_int *plugin_find_by_type(const LEX_CSTRING &plugin, int type);

/**
  The function detaches existing storage engines transaction
  context from thd. Backup area to save it is provided to low level
  storage engine function.

  is invoked by plugin_foreach() after
  trans_xa_start() for each storage engine.

  @param[in,out]     thd     Thread context
  @param             plugin  Reference to handlerton

  @return    false   on success, true otherwise.
*/

bool detach_native_trx(THD *thd, plugin_ref plugin, void *);

/**
  The function reattaches existing storage engines transaction
  context to thd. Backup area to save it is provided to low level
  storage engine function.

  is invoked by plugin_foreach() after
  trans_xa_prepare() for each storage engine.

  @param[in,out]     thd     Thread context
  @param             plugin  Reference to handlerton

  @return    false   on success,
             true    otherwise.
*/

bool reattach_native_trx(THD *thd, plugin_ref plugin, void *);

/**
  Reset some transaction state information and delete corresponding
  Transaction_ctx object from cache.

  @param thd    Current thread
*/

void cleanup_trans_state(THD *thd);

/**
  Find XA transaction in cache by its xid value.

  @param thd                     Thread context
  @param xid_for_trn_in_recover  xid value to look for in transaction cache
  @param xid_state               State of XA transaction in current session

  @return Pointer to an instance of Transaction_ctx corresponding to a
          xid in argument. If XA transaction not found returns nullptr and
          sets an error in DA to specify a reason of search failure.
*/
std::shared_ptr<Transaction_ctx> find_trn_for_recover_and_check_its_state(
    THD *thd, xid_t *xid_for_trn_in_recover, XID_STATE *xid_state);

/**
  Acquire Commit metadata lock and all locks acquired by a prepared XA
  transaction before server was shutdown or terminated.

  @param thd           Thread context
  @param detached_xid  XID value specified by XA COMMIT or XA ROLLBACK that
                       corresponds to a XA transaction generated outside
                       current session context.

  @retval false        Success
  @retval true         Failure
*/
bool acquire_mandatory_metadata_locks(THD *thd, xid_t *detached_xid);

/**
  Rollback the active XA transaction.

  @note Resets rm_error before calling ha_rollback(), so
        the thd->transaction.xid structure gets reset
        by ha_rollback() / THD::transaction::cleanup().

  @return true if the rollback failed, false otherwise.
*/

bool xa_trans_force_rollback(THD *thd);

bool disconnect_native_trx(THD *, plugin_ref, void *);

/**
  Test if the THD session underlying transaction is an externally
  coordinated (XA) transaction.

  @param thd The session THD object holding the transaction to be tested.

  @return true if the session underlying transaction is an XA transaction,
          false otherwise.
*/
bool thd_holds_xa_transaction(THD *thd);
/**
  Checks whether or not the underlying statement is an `XA PREPARE`.

  @param thd THD session object.

  @return true if the underlying statement is an `XA PREPARE`, false
          if not
 */
bool is_xa_prepare(THD *thd);
/**
  Checks whether or not the underlying statement is an `XA ROLLBACK`.

  @param thd THD session object.

  @return true if the underlying statement is an `XA ROLLBACK`, false
          if not
 */
bool is_xa_rollback(THD *thd);
#endif
