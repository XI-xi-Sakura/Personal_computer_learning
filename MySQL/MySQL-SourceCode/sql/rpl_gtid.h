/* Copyright (c) 2011, 2025, Oracle and/or its affiliates.

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

#ifndef RPL_GTID_H_INCLUDED
#define RPL_GTID_H_INCLUDED

#include <atomic>
#include <cinttypes>
#include <list>
#include <mutex>  // std::adopt_lock_t
#include <vector>

#include "map_helpers.h"
#include "my_dbug.h"
#include "my_thread_local.h"
#include "mysql/binlog/event/compression/base.h"  // mysql::binlog::event::compression::type
#include "mysql/gtid/global.h"
#include "mysql/gtid/gtid.h"
#include "mysql/gtid/tsid.h"
#include "mysql/gtid/tsid_plain.h"
#include "mysql/gtid/uuid.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_rwlock.h"  // mysql_rwlock_t
#include "mysql/strings/m_ctype.h"   // my_isspace
#include "mysql/utils/return_status.h"
#include "prealloced_array.h"                          // Prealloced_array
#include "sql/changestreams/index/locked_sidno_set.h"  // Locked_sidno_set
#include "sql/changestreams/index/sidno.h"
#include "sql/rpl_reporting.h"  // MAX_SLAVE_ERRMSG
#include "template_utils.h"

class Table_ref;
class THD;

/**
  Report an error from code that can be linked into either the server
  or mysqlbinlog.  There is no common error reporting mechanism, so we
  have to duplicate the error message (write it out in the source file
  for mysqlbinlog, write it in share/messages_to_clients.txt for the
  server).

  @param MYSQLBINLOG_ERROR arguments to mysqlbinlog's 'error'
  function, including the function call parentheses
  @param SERVER_ERROR arguments to my_error, including the function
  call parentheses.
*/
#ifndef MYSQL_SERVER
#define BINLOG_ERROR(MYSQLBINLOG_ERROR, SERVER_ERROR) error MYSQLBINLOG_ERROR
#else
#define BINLOG_ERROR(MYSQLBINLOG_ERROR, SERVER_ERROR) my_error SERVER_ERROR
#endif

extern PSI_memory_key key_memory_Gtid_set_to_string;
extern PSI_memory_key key_memory_Owned_gtids_to_string;
extern PSI_memory_key key_memory_Gtid_state_to_string;
extern PSI_memory_key key_memory_Gtid_cache_to_string;
extern PSI_memory_key key_memory_Gtid_set_Interval_chunk;
extern PSI_memory_key key_memory_Gtid_state_group_commit_sidno;

/**
  This macro is used to check that the given character, pointed to by the
  character pointer, is a space or not.
*/
#define SKIP_WHITESPACE() \
  while (my_isspace(&my_charset_utf8mb3_general_ci, *s)) s++

/*
  This macro must be used to filter out parts of the code that
  is not used now but may be useful in future. In other words,
  we want to keep such code until we make up our minds on whether
  it should be removed or not.
*/
#undef NON_DISABLED_GTID

/*
  This macro must be used to filter out parts of the code that
  is not used now but we are not sure if there is a bug around
  them. In other words, we want to keep such code until we have
  time to investigate it.
*/
#undef NON_ERROR_GTID

#ifdef MYSQL_SERVER
class String;
class THD;
#endif  // ifdef MYSQL_SERVER

/// Type of SIDNO (source ID number, first component of GTID)
using rpl_sidno = cs::index::rpl_sidno;

/// GNO, the second (numeric) component of a GTID, is an alias of
/// mysql::gtid::gno_t
using rpl_gno = mysql::gtid::gno_t;
typedef int64 rpl_binlog_pos;

namespace cs::index {

class Locked_sidno_set;

}  // namespace cs::index

/**
  Generic return type for many functions that can succeed or fail.

  This is used in conjunction with the macros below for functions where
  the return status either indicates "success" or "failure".  It
  provides the following features:

   - The macros can be used to conveniently propagate errors from
     called functions back to the caller.

   - If a function is expected to print an error using my_error before
     it returns an error status, then the macros assert that my_error
     has been called.

   - Does a DBUG_PRINT before returning failure.
*/
enum enum_return_status {
  /// The function completed successfully.
  RETURN_STATUS_OK = 0,
  /// The function completed with error but did not report it.
  RETURN_STATUS_UNREPORTED_ERROR = 1,
  /// The function completed with error and has called my_error.
  RETURN_STATUS_REPORTED_ERROR = 2
};

/**
  @def __CHECK_RETURN_STATUS
  Lowest level macro used in the PROPAGATE_* and RETURN_* macros
  below.

  If NDEBUG is defined, does nothing. Otherwise, if STATUS is
  RETURN_STATUS_OK, does nothing; otherwise, make a dbug printout and
  (if ALLOW_UNREPORTED==0) assert that STATUS !=
  RETURN_STATUS_UNREPORTED.

  @param STATUS The status to return.
  @param ACTION A text that describes what we are doing: either
  "Returning" or "Propagating" (used in DBUG_PRINT macros)
  @param STATUS_NAME The stringified version of the STATUS (used in
  DBUG_PRINT macros).
  @param ALLOW_UNREPORTED If false, the macro asserts that STATUS is
  not RETURN_STATUS_UNREPORTED_ERROR.
*/
#ifdef NDEBUG
#define __CHECK_RETURN_STATUS(STATUS, ACTION, STATUS_NAME, ALLOW_UNREPORTED)
#else
extern void check_return_status(enum_return_status status, const char *action,
                                const char *status_name, int allow_unreported);
#define __CHECK_RETURN_STATUS(STATUS, ACTION, STATUS_NAME, ALLOW_UNREPORTED) \
  check_return_status(STATUS, ACTION, STATUS_NAME, ALLOW_UNREPORTED);
#endif
/**
  Low-level macro that checks if STATUS is RETURN_STATUS_OK; if it is
  not, then RETURN_VALUE is returned.
  @see __DO_RETURN_STATUS
*/
#define __PROPAGATE_ERROR(STATUS, RETURN_VALUE, ALLOW_UNREPORTED)             \
  do {                                                                        \
    enum_return_status __propagate_error_status = STATUS;                     \
    if (__propagate_error_status != RETURN_STATUS_OK) {                       \
      __CHECK_RETURN_STATUS(__propagate_error_status, "Propagating", #STATUS, \
                            ALLOW_UNREPORTED);                                \
      return RETURN_VALUE;                                                    \
    }                                                                         \
  } while (0)
/// Low-level macro that returns STATUS. @see __DO_RETURN_STATUS
#define __RETURN_STATUS(STATUS, ALLOW_UNREPORTED)                       \
  do {                                                                  \
    enum_return_status __return_status_status = STATUS;                 \
    __CHECK_RETURN_STATUS(__return_status_status, "Returning", #STATUS, \
                          ALLOW_UNREPORTED);                            \
    return __return_status_status;                                      \
  } while (0)
/**
  If STATUS (of type enum_return_status) returns RETURN_STATUS_OK,
  does nothing; otherwise, does a DBUG_PRINT and returns STATUS.
*/
#define PROPAGATE_ERROR(STATUS) \
  __PROPAGATE_ERROR(STATUS, __propagate_error_status, true)
/**
  If STATUS (of type enum_return_status) returns RETURN_STATUS_OK,
  does nothing; otherwise asserts that STATUS ==
  RETURN_STATUS_REPORTED_ERROR, does a DBUG_PRINT, and returns STATUS.
*/
#define PROPAGATE_REPORTED_ERROR(STATUS) \
  __PROPAGATE_ERROR(STATUS, __propagate_error_status, false)
/**
  If STATUS (of type enum_return_status) returns RETURN_STATUS_OK,
  does nothing; otherwise asserts that STATUS ==
  RETURN_STATUS_REPORTED_ERROR, does a DBUG_PRINT, and returns 1.
*/
#define PROPAGATE_REPORTED_ERROR_INT(STATUS) __PROPAGATE_ERROR(STATUS, 1, false)
/**
  If STATUS returns something else than RETURN_STATUS_OK, does a
  DBUG_PRINT.  Then, returns STATUS.
*/
#define RETURN_STATUS(STATUS) __RETURN_STATUS(STATUS, true)
/**
  Asserts that STATUS is not RETURN_STATUS_UNREPORTED_ERROR.  Then, if
  STATUS is RETURN_STATUS_REPORTED_ERROR, does a DBUG_PRINT.  Then,
  returns STATUS.
*/
#define RETURN_REPORTED_STATUS(STATUS) __RETURN_STATUS(STATUS, false)
/// Returns RETURN_STATUS_OK.
#define RETURN_OK return RETURN_STATUS_OK
/// Does a DBUG_PRINT and returns RETURN_STATUS_REPORTED_ERROR.
#define RETURN_REPORTED_ERROR RETURN_STATUS(RETURN_STATUS_REPORTED_ERROR)
/// Does a DBUG_PRINT and returns RETURN_STATUS_UNREPORTED_ERROR.
#define RETURN_UNREPORTED_ERROR RETURN_STATUS(RETURN_STATUS_UNREPORTED_ERROR)

/**
  enum to map the result of Uuid::parse to the above Macros
*/
inline enum_return_status map_macro_enum(int status) {
  DBUG_TRACE;
  if (status == 0)
    RETURN_OK;
  else
    RETURN_UNREPORTED_ERROR;
}

/**
  Possible values for ENFORCE_GTID_CONSISTENCY.
*/
enum enum_gtid_consistency_mode {
  GTID_CONSISTENCY_MODE_OFF = 0,
  GTID_CONSISTENCY_MODE_ON = 1,
  GTID_CONSISTENCY_MODE_WARN = 2
};
/**
  Strings holding the enumeration values for
  gtid_consistency_mode_names.  Use get_gtid_consistency_mode_string
  instead of accessing this directly.
*/
extern const char *gtid_consistency_mode_names[];
/**
  Current value for ENFORCE_GTID_CONSISTENCY.
  Don't use this directly; use get_gtid_consistency_mode.
*/
extern ulong _gtid_consistency_mode;
/**
  Return the current value of ENFORCE_GTID_CONSISTENCY.

  Caller must hold global_tsid_lock.rdlock.
*/
enum_gtid_consistency_mode get_gtid_consistency_mode();
/// Return the given GTID_CONSISTENCY_MODE as a string.
inline const char *get_gtid_consistency_mode_string(
    enum_gtid_consistency_mode mode) {
  return gtid_consistency_mode_names[(int)mode];
}
/**
  Return the current value of ENFORCE_GTID_CONSISTENCY as a string.

  Caller must hold global_tsid_lock.rdlock.
*/
inline const char *get_gtid_consistency_mode_string() {
  return get_gtid_consistency_mode_string(get_gtid_consistency_mode());
}

/// One-past-the-max value of GNO
const rpl_gno GNO_END = INT64_MAX;
/// If the GNO goes above the number, generate a warning.
const rpl_gno GNO_WARNING_THRESHOLD = (GNO_END / 100) * 99;
/// The length of MAX_GNO when printed in decimal.
const int MAX_GNO_TEXT_LENGTH = 19;
/// The maximal possible length of thread_id when printed in decimal.
const int MAX_THREAD_ID_TEXT_LENGTH = 19;

/**
  Parse a GNO from a string.

  @param s Pointer to the string. *s will advance to the end of the
  parsed GNO, if a correct GNO is found.
  @retval GNO if a correct GNO (i.e., 0 or positive number) was found.
  @retval -1 otherwise.
*/
rpl_gno parse_gno(const char **s);
/**
  Formats a GNO as a string.

  @param s The buffer.
  @param gno The GNO.
  @return Length of the generated string.
*/
int format_gno(char *s, rpl_gno gno);

typedef mysql::gtid::Uuid rpl_sid;

/**
  This has the functionality of mysql_rwlock_t, with two differences:
  1. It has additional operations to check if the read and/or write lock
     is held at the moment.
  2. It is wrapped in an object-oriented interface.

  Note that the assertions do not check whether *this* thread has
  taken the lock (that would be more complicated as it would require a
  dynamic data structure).  Luckily, it is still likely that the
  assertions find bugs where a thread forgot to take a lock, because
  most of the time most locks are only used by one thread at a time.

  The assertions are no-ops when DBUG is off.
*/
class Checkable_rwlock {
 public:
  /// Initialize this Checkable_rwlock.
  Checkable_rwlock(
#if defined(HAVE_PSI_INTERFACE)
      PSI_rwlock_key psi_key [[maybe_unused]] = 0
#endif
  ) {
#ifndef NDEBUG
    m_lock_state.store(0);
    m_dbug_trace = true;
#else
    m_is_write_lock = false;
#endif
#if defined(HAVE_PSI_INTERFACE)
    mysql_rwlock_init(psi_key, &m_rwlock);
#else
    mysql_rwlock_init(0, &m_rwlock);
#endif
  }
  /// Destroy this Checkable_lock.
  ~Checkable_rwlock() { mysql_rwlock_destroy(&m_rwlock); }

  enum enum_lock_type {
    NO_LOCK,
    READ_LOCK,
    WRITE_LOCK,
    TRY_READ_LOCK,
    TRY_WRITE_LOCK
  };

  /**
    RAII class to acquire a lock for the duration of a block.
  */
  class Guard {
    Checkable_rwlock &m_lock;
    enum_lock_type m_lock_type;

   public:
    /**
      Create a guard, and optionally acquire a lock on it.
    */
    Guard(Checkable_rwlock &lock, enum_lock_type lock_type)
        : m_lock(lock), m_lock_type(NO_LOCK) {
      DBUG_TRACE;
      switch (lock_type) {
        case READ_LOCK:
          rdlock();
          break;
        case WRITE_LOCK:
          wrlock();
          break;
        case TRY_READ_LOCK:
          tryrdlock();
          break;
        case TRY_WRITE_LOCK:
          trywrlock();
          break;
        case NO_LOCK:
          break;
      }
    }

    /**
      Create a guard, assuming the caller already holds a lock on it.
    */
    Guard(Checkable_rwlock &lock, enum_lock_type lock_type,
          std::adopt_lock_t t [[maybe_unused]])
        : m_lock(lock), m_lock_type(lock_type) {
      DBUG_TRACE;
      switch (lock_type) {
        case READ_LOCK:
          lock.assert_some_rdlock();
          break;
        case WRITE_LOCK:
          lock.assert_some_wrlock();
          break;
        case TRY_READ_LOCK:
        case TRY_WRITE_LOCK:
        case NO_LOCK:
          break;
      }
    }

    /// Objects of this class should not be copied or moved.
    Guard(Guard const &copy) = delete;
    Guard(Guard const &&copy) = delete;

    /// Unlock on destruct.
    ~Guard() {
      DBUG_TRACE;
      unlock_if_locked();
    }

    /// Acquire the read lock.
    void rdlock() {
      DBUG_TRACE;
      assert(m_lock_type == NO_LOCK);
      m_lock.rdlock();
      m_lock_type = READ_LOCK;
    }

    /// Acquire the write lock.
    void wrlock() {
      DBUG_TRACE;
      assert(m_lock_type == NO_LOCK);
      m_lock.wrlock();
      m_lock_type = WRITE_LOCK;
    }

    /**
      Try to acquire the write lock, and fail if it cannot be
      immediately granted.
    */
    int trywrlock() {
      DBUG_TRACE;
      assert(m_lock_type == NO_LOCK);
      int ret = m_lock.trywrlock();
      if (ret == 0) m_lock_type = WRITE_LOCK;
      return ret;
    }

    /**
      Try to acquire a read lock, and fail if it cannot be
      immediately granted.
    */
    int tryrdlock() {
      DBUG_TRACE;
      assert(m_lock_type == NO_LOCK);
      int ret = m_lock.tryrdlock();
      if (ret == 0) m_lock_type = READ_LOCK;
      return ret;
    }

    /// Unlock the lock.
    void unlock() {
      DBUG_TRACE;
      assert(m_lock_type != NO_LOCK);
      m_lock.unlock();
    }

    /// Unlock the lock, if it was acquired by this guard.
    void unlock_if_locked() {
      DBUG_TRACE;
      if (m_lock_type != NO_LOCK) unlock();
    }

    /// Return the underlying Checkable_rwlock object.
    Checkable_rwlock &get_lock() const { return m_lock; }

    /// Return true if this object is read locked.
    bool is_rdlocked() const { return m_lock_type == READ_LOCK; }

    /// Return true if this object is write locked.
    bool is_wrlocked() const { return m_lock_type == WRITE_LOCK; }

    /// Return true if this object is either read locked or write locked.
    bool is_locked() const { return m_lock_type != NO_LOCK; }
  };

  /// Acquire the read lock.
  inline void rdlock() {
    mysql_rwlock_rdlock(&m_rwlock);
    assert_no_wrlock();
#ifndef NDEBUG
    if (m_dbug_trace) DBUG_PRINT("info", ("%p.rdlock()", this));
    ++m_lock_state;
#endif
  }
  /// Acquire the write lock.
  inline void wrlock() {
    mysql_rwlock_wrlock(&m_rwlock);
    assert_no_lock();
#ifndef NDEBUG
    if (m_dbug_trace) DBUG_PRINT("info", ("%p.wrlock()", this));
    m_lock_state.store(-1);
#else
    m_is_write_lock = true;
#endif
  }
  /// Release the lock (whether it is a write or read lock).
  inline void unlock() {
    assert_some_lock();
#ifndef NDEBUG
    if (m_dbug_trace) DBUG_PRINT("info", ("%p.unlock()", this));
    int val = m_lock_state.load();
    if (val > 0)
      --m_lock_state;
    else if (val == -1)
      m_lock_state.store(0);
    else
      assert(0);
#else
    m_is_write_lock = false;
#endif
    mysql_rwlock_unlock(&m_rwlock);
  }
  /**
    Return true if the write lock is held. Must only be called by
    threads that hold a lock.
  */
  inline bool is_wrlock() {
    assert_some_lock();
#ifndef NDEBUG
    return get_state() == -1;
#else
    return m_is_write_lock;
#endif
  }

  /**
    Return 0 if the write lock is held, otherwise an error will be returned.
  */
  inline int trywrlock() {
    int ret = mysql_rwlock_trywrlock(&m_rwlock);

    if (ret == 0) {
      assert_no_lock();
#ifndef NDEBUG
      if (m_dbug_trace) DBUG_PRINT("info", ("%p.wrlock()", this));
      m_lock_state.store(-1);
#else
      m_is_write_lock = true;
#endif
    }

    return ret;
  }

  /**
    Return 0 if the read lock is held, otherwise an error will be returned.
  */
  inline int tryrdlock() {
    int ret = mysql_rwlock_tryrdlock(&m_rwlock);

    if (ret == 0) {
      assert_no_wrlock();
#ifndef NDEBUG
      if (m_dbug_trace) DBUG_PRINT("info", ("%p.rdlock()", this));
      ++m_lock_state;
#endif
    }

    return ret;
  }

  /// Assert that some thread holds either the read or the write lock.
  inline void assert_some_lock() const { assert(get_state() != 0); }
  /// Assert that some thread holds the read lock.
  inline void assert_some_rdlock() const { assert(get_state() > 0); }
  /// Assert that some thread holds the write lock.
  inline void assert_some_wrlock() const { assert(get_state() == -1); }
  /// Assert that no thread holds the write lock.
  inline void assert_no_wrlock() const { assert(get_state() >= 0); }
  /// Assert that no thread holds the read lock.
  inline void assert_no_rdlock() const { assert(get_state() <= 0); }
  /// Assert that no thread holds read or write lock.
  inline void assert_no_lock() const { assert(get_state() == 0); }

#ifndef NDEBUG

  /// If enabled, print any lock/unlock operations to the DBUG trace.
  bool m_dbug_trace;

 private:
  /**
    The state of the lock:
    0 - not locked
    -1 - write locked
    >0 - read locked by that many threads
  */
  std::atomic<int32> m_lock_state;
  /// Read lock_state atomically and return the value.
  inline int32 get_state() const { return m_lock_state.load(); }

#else

 private:
  bool m_is_write_lock;

#endif
  /// The rwlock.
  mysql_rwlock_t m_rwlock{};
};

/// Protects Gtid_state.  See comment above gtid_state for details.
extern Checkable_rwlock *global_tsid_lock;

/**
  Class to access the value of @@global.gtid_mode in an efficient and
  thread-safe manner.
*/
class Gtid_mode {
 private:
  std::atomic<int> m_atomic_mode;

 public:
  Gtid_mode() : m_atomic_mode(0) {}

  /**
    The sys_var framework needs a variable of type ulong to store the
    value in.  The sys_var framework takes the value from there, but
    we copy it (in the methods of sys_var_gtid_mode) to the atomic
    value Gtid_mode::mode, and use only that in all other places.
  */
  static ulong sysvar_mode;

  /// Possible values for @@global.gtid_mode.
  enum value_type {
    /**
      New transactions are anonymous. Replicated transactions must be
      anonymous; replicated GTID-transactions generate an error.
    */
    OFF = 0,
    DEFAULT = OFF,
    /**
      New transactions are anonyomus. Replicated transactions can be
      either anonymous or GTID-transactions.
    */
    OFF_PERMISSIVE = 1,
    /**
      New transactions are GTID-transactions. Replicated transactions
      can be either anonymous or GTID-transactions.
    */
    ON_PERMISSIVE = 2,
    /**
      New transactions are GTID-transactions. Replicated transactions
      must be GTID-transactions; replicated anonymous transactions
      generate an error.
    */
    ON = 3
  };

  /**
    Strings holding the enumeration values for gtid_mode.  Use
    Gtid_mode::get_string instead of accessing this directly.
  */
  static const char *names[];

  /**
    Protects updates to @@global.gtid_mode.

    SET @@global.gtid_mode will try to take the write lock.  If the
    lock is not granted immediately, SET will fail.

    Thus, any other operation can block updates to
    @@global.gtid_mode by acquiring the read lock.
  */
  static Checkable_rwlock lock;

  /**
    Set a new value for @@global.gtid_mode.

    This should only be called from Sys_var_gtid_mode::global_update
    and gtid_server_init.
  */
  void set(value_type value);

 public:
  /**
    Return the current gtid_mode as an enumeration value.
  */
  value_type get() const;

#ifndef NDEBUG
  /**
    Return the current gtid_mode as a string.

    Used only for debugging.  Non-debug code typically reads and acts
    on the enum value before printing it. Then it is better to print
    the enum value.
  */
  const char *get_string() const;
#endif  // ifndef NDEBUG

  /**
    Return the given string gtid_mode as an enumeration value.

    @param s The string to decode.

    @return A pair, where the first component indicates failure and
    the second component is the GTID_MODE.  Specifically, the first
    component is false if the string is a valid GTID_MODE, and true if
    it is not.
  */
  static std::pair<bool, value_type> from_string(std::string s);

  /// Return the given gtid_mode as a string.
  static const char *to_string(value_type value);
};

std::ostream &operator<<(std::ostream &oss, Gtid_mode::value_type const &mode);
#ifndef NDEBUG
/**
  Typically, code will print Gtid_mode only after reading and acting
  on the enum value. Then it is better to print the enum value than to
  read the shared resource again.  Hence we enable this only in debug
  mode, since it makes more sense to just get the string when doing a
  debug printout.
*/
std::ostream &operator<<(std::ostream &oss, Gtid_mode const &mode);
#endif

/**
  The one and only instance of Gtid_mode.

  All access to @@global.gtid_mode should use this object.
*/
extern Gtid_mode global_gtid_mode;

/**
  Represents a bidirectional map between TSID and SIDNO.

  SIDNOs are always numbers greater or equal to 1.

  This data structure OPTIONALLY knows of a read-write lock that
  protects the number of SIDNOs.  The lock is provided by the invoker
  of the constructor and it is generally the caller's responsibility
  to acquire the read lock.  If the lock is not NULL, access methods
  assert that the caller already holds the read (or write) lock.  If
  the lock is not NULL and a method of this class grows the number of
  SIDNOs, then the method temporarily upgrades this lock to a write
  lock and then degrades it to a read lock again; there will be a
  short period when the lock is not held at all.
*/
class Tsid_map {
 public:
  /**
    Create this Tsid_map.

    @param tsid_lock Read-write lock that protects updates to the
    number of SIDNOs.
  */
  Tsid_map(Checkable_rwlock *tsid_lock);

  /// Destroy this Tsid_map.
  ~Tsid_map();
  /**
    Clears this Tsid_map (for RESET REPLICA)

    @return RETURN_STATUS_OK or RETURN_STAUTS_REPORTED_ERROR
  */
  enum_return_status clear();

  using Tsid = mysql::gtid::Tsid;
  using Tag = mysql::gtid::Tag;
  using Tsid_to_sidno_allocator = Map_allocator_type<Tsid, rpl_sidno>;
  using Tsid_to_sidno_umap = malloc_unordered_map<Tsid, rpl_sidno, Tsid::Hash>;
  using Tsid_to_sidno_map = Map_myalloc<Tsid, rpl_sidno>;
  using Tsid_to_sidno_it = Tsid_to_sidno_map::const_iterator;
  using Tsid_ref = std::reference_wrapper<const Tsid>;
  using Sidno_to_tsid_cont = std::vector<Tsid_ref, Malloc_allocator<Tsid_ref>>;

  /**
    Add the given TSID to this map if it does not already exist.

    The caller must hold the read lock or write lock on tsid_lock
    before invoking this function.  If the TSID does not exist in this
    map, it will release the read lock, take a write lock, update the
    map, release the write lock, and take the read lock again.

    @param tsid The TSID.
    @retval SIDNO The SIDNO for the TSID (a new SIDNO if the TSID did
    not exist, an existing if it did exist).
    @retval negative Error. This function calls my_error.
  */
  [[NODISCARD]] rpl_sidno add_tsid(const Tsid &tsid);
  /**
    Get the SIDNO for a given TSID

    The caller must hold the read lock on tsid_lock before invoking
    this function.

    @param tsid The TSID.
    @retval SIDNO if the given TSID exists in this map.
    @retval 0 if the given TSID does not exist in this map.
  */
  rpl_sidno tsid_to_sidno(const Tsid &tsid) const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_lock();
    const auto it = _tsid_to_sidno.find(tsid);
    if (it == _tsid_to_sidno.end()) return 0;
    return it->second;
  }
  /**
    Get the TSID for a given SIDNO.

    Raises an assertion if the SIDNO is not valid.

    If need_lock is true, acquires tsid_lock->rdlock; otherwise asserts
    that it is held already.

    @param sidno The SIDNO.
    @param need_lock If true, and tsid_lock!=NULL, this function will
    acquire tsid_lock before looking up the sid, and then release
    it. If false, and tsid_lock!=NULL, this function will assert the
    tsid_lock is already held. If tsid_lock==NULL, nothing is done
    w.r.t. locking.
    @retval NULL The SIDNO does not exist in this map.
    @retval ref Reference to the TSID.  The data is shared with this
    Tsid_map, so should not be modified.  It is safe to read the data
    even after this Tsid_map is modified, but not if this Tsid_map is
    destroyed.
  */
  const Tsid &sidno_to_tsid(rpl_sidno sidno, bool need_lock = false) const {
    if (tsid_lock != nullptr) {
      if (need_lock)
        tsid_lock->rdlock();
      else
        tsid_lock->assert_some_lock();
    }
    assert(sidno >= 1 && sidno <= get_max_sidno());
    const auto &ret = (_sidno_to_tsid[sidno - 1]);
    if (tsid_lock != nullptr && need_lock) tsid_lock->unlock();
    return ret.get();
  }

  /**
    Returns TSID to SID map

    The caller must hold the read or write lock on tsid_lock before
    invoking this function.

    @return constant reference to sid_to_sidno container
  */
  const Tsid_to_sidno_map &get_sorted_sidno() const { return _sorted; }

  const rpl_sidno &get_sidno(const Tsid_to_sidno_it &it) const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_lock();
    return it->second;
  }

  const Tsid &get_tsid(const Tsid_to_sidno_it &it) const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_lock();
    return it->first;
  }

  /**
    Return the biggest sidno in this Tsid_map.

    The caller must hold the read or write lock on tsid_lock before
    invoking this function.
  */
  rpl_sidno get_max_sidno() const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_lock();
    return static_cast<rpl_sidno>(_sidno_to_tsid.size());
  }

  /// Return the tsid_lock.
  Checkable_rwlock *get_tsid_lock() const { return tsid_lock; }

  /**
    Deep copy this Tsid_map to dest.

    The caller must hold:
     * the read lock on this tsid_lock
     * the write lock on the dest tsid_lock
    before invoking this function.

    @param[out] dest The Tsid_map to which the tsids and sidnos will
                     be copied.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status copy(Tsid_map *dest);

 private:
  /**
    Create a Node from the given SIDNO and a TSID and add it to
    _sidno_to_tsid, _tsid_to_sidno, and _sorted.

    The caller must hold the write lock on tsid_lock before invoking
    this function.

    @param sidno The SIDNO to add.
    @param tsid The TSID to add.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  [[NODISCARD]] enum_return_status add_node(rpl_sidno sidno, const Tsid &tsid);

  /// Read-write lock that protects updates to the number of SIDNOs.
  mutable Checkable_rwlock *tsid_lock;

  /**
    Array that maps SIDNO to TSID; the element at index N points to a
    Node with SIDNO N-1.
  */
  Sidno_to_tsid_cont _sidno_to_tsid{
      Malloc_allocator<Tsid_ref>{key_memory_tsid_map_Node}};

  /**
    Hash that maps TSID to SIDNO.
  */
  Tsid_to_sidno_umap _tsid_to_sidno{key_memory_tsid_map_Node};
  /**
    Data structure that maps numbers in the interval [0, get_max_sidno()-1] to
    SIDNOs, in order of increasing TSID.

    @see Tsid_map::get_sorted_sidno.
  */
  Tsid_to_sidno_map _sorted{Tsid_to_sidno_allocator{key_memory_tsid_map_Node}};
};

extern Tsid_map *global_tsid_map;

/**
  Represents a growable array where each element contains a mutex and
  a condition variable.

  Each element can be locked, unlocked, broadcast, or waited for, and
  it is possible to call "THD::enter_cond" for the condition.  The
  allowed indexes range from 0, inclusive, to get_max_index(),
  inclusive.  Initially there are zero elements (and get_max_index()
  returns -1); more elements can be allocated by calling
  ensure_index().

  This data structure has a read-write lock that protects the number
  of elements.  The lock is provided by the invoker of the constructor
  and it is generally the caller's responsibility to acquire the read
  lock.  Access methods assert that the caller already holds the read
  (or write) lock.  If a method of this class grows the number of
  elements, then the method temporarily upgrades this lock to a write
  lock and then degrades it to a read lock again; there will be a
  short period when the lock is not held at all.
*/
class Mutex_cond_array {
 public:
  /**
    Create a new Mutex_cond_array.

    @param global_lock Read-write lock that protects updates to the
    number of elements.
  */
  Mutex_cond_array(Checkable_rwlock *global_lock);
  /// Destroy this object.
  ~Mutex_cond_array();
  /// Lock the n'th mutex.
  inline void lock(int n) const {
    assert_not_owner(n);
    mysql_mutex_lock(&get_mutex_cond(n)->mutex);
  }
  /// Unlock the n'th mutex.
  inline void unlock(int n) const {
    assert_owner(n);
    mysql_mutex_unlock(&get_mutex_cond(n)->mutex);
  }
  /// Broadcast the n'th condition.
  inline void broadcast(int n) const {
    mysql_cond_broadcast(&get_mutex_cond(n)->cond);
  }
  /**
    Assert that this thread owns the n'th mutex.
    This is a no-op if NDEBUG is on.
  */
  inline void assert_owner(int n [[maybe_unused]]) const {
#ifndef NDEBUG
    mysql_mutex_assert_owner(&get_mutex_cond(n)->mutex);
#endif
  }
  /**
    Assert that this thread does not own the n'th mutex.
    This is a no-op if NDEBUG is on.
  */
  inline void assert_not_owner(int n [[maybe_unused]]) const {
#ifndef NDEBUG
    mysql_mutex_assert_not_owner(&get_mutex_cond(n)->mutex);
#endif
  }

  /**
    Wait for signal on the n'th condition variable.

    The caller must hold the read lock or write lock on tsid_lock, as
    well as the nth mutex lock, before invoking this function.  The
    tsid_lock will be released, whereas the mutex will be released
    during the wait and (atomically) re-acquired when the wait ends
    or the timeout is reached.

    @param[in] thd THD object for the calling thread.
    @param[in] sidno Condition variable to wait for.
    @param[in] abstime The absolute point in time when the wait times
    out and stops, or NULL to wait indefinitely.

    @retval false Success.
    @retval true Failure: either timeout or thread was killed.  If
    thread was killed, the error has been generated.
  */
  inline bool wait(const THD *thd, int sidno, struct timespec *abstime) const {
    DBUG_TRACE;
    int error = 0;
    Mutex_cond *mutex_cond = get_mutex_cond(sidno);
    global_lock->unlock();
    mysql_mutex_assert_owner(&mutex_cond->mutex);
    if (is_thd_killed(thd)) return true;
    if (abstime != nullptr)
      error =
          mysql_cond_timedwait(&mutex_cond->cond, &mutex_cond->mutex, abstime);
    else
      mysql_cond_wait(&mutex_cond->cond, &mutex_cond->mutex);
    mysql_mutex_assert_owner(&mutex_cond->mutex);
    return is_timeout(error);
  }
#ifdef MYSQL_SERVER
  /// Execute THD::enter_cond for the n'th condition variable.
  void enter_cond(THD *thd, int n, PSI_stage_info *stage,
                  PSI_stage_info *old_stage) const;
#endif  // ifdef MYSQL_SERVER
  /// Return the greatest addressable index in this Mutex_cond_array.
  inline int get_max_index() const {
    global_lock->assert_some_lock();
    return static_cast<int>(m_array.size() - 1);
  }
  /**
    Grows the array so that the given index fits.

    If the array is grown, the global_lock is temporarily upgraded to
    a write lock and then degraded again; there will be a
    short period when the lock is not held at all.

    @param n The index.
    @return RETURN_OK or RETURN_REPORTED_ERROR
  */
  enum_return_status ensure_index(int n);

 private:
  /**
    Return true if the given THD is killed.

    @param[in] thd -  The thread object
    @retval true  - thread is killed
            false - thread not killed
  */
  bool is_thd_killed(const THD *thd) const;
  /// A mutex/cond pair.
  struct Mutex_cond {
    mysql_mutex_t mutex;
    mysql_cond_t cond;
  };
  /// Return the Nth Mutex_cond object
  inline Mutex_cond *get_mutex_cond(int n) const {
    global_lock->assert_some_lock();
    assert(n <= get_max_index());
    Mutex_cond *ret = m_array[n];
    assert(ret);
    return ret;
  }
  /// Read-write lock that protects updates to the number of elements.
  mutable Checkable_rwlock *global_lock;
  Prealloced_array<Mutex_cond *, 8> m_array;
};

/**
  Holds information about a GTID interval: the sidno, the first gno
  and the last gno of this interval.
*/
struct Gtid_interval {
  /* SIDNO of this Gtid interval. */
  rpl_sidno sidno;
  /* The first GNO of this Gtid interval. */
  rpl_gno gno_start;
  /* The last GNO of this Gtid interval. */
  rpl_gno gno_end;
  void set(rpl_sidno sid_no, rpl_gno start, rpl_gno end) {
    sidno = sid_no;
    gno_start = start;
    gno_end = end;
  }
};

/**
  TODO: Move this structure to mysql/binlog/event/control_events.h
        when we start using C++11.
  Holds information about a GTID: the sidno and the gno.

  This is a POD. It has to be a POD because it is part of
  Gtid_specification, which has to be a POD because it is used in
  THD::variables.
*/
struct Gtid {
  using Tsid = mysql::gtid::Tsid;
  using Tag = mysql::gtid::Tag;
  /// SIDNO of this Gtid.
  rpl_sidno sidno;
  /// GNO of this Gtid.
  rpl_gno gno;

  /// Set both components to 0.
  void clear() {
    sidno = 0;
    gno = 0;
  }
  /// Set both components to the given, positive values.
  void set(rpl_sidno sidno_arg, rpl_gno gno_arg) {
    assert(sidno_arg > 0);
    assert(gno_arg > 0);
    assert(gno_arg < GNO_END);
    sidno = sidno_arg;
    gno = gno_arg;
  }
  /**
    Return true if sidno is zero (and assert that gno is zero too in
    this case).
  */
  bool is_empty() const {
    // check that gno is not set inconsistently
    if (sidno <= 0)
      assert(gno == 0);
    else
      assert(gno > 0);
    return sidno == 0;
  }
  /**
    The maximal length of the textual representation of a TSID, not
    including the terminating '\0'.
  */
  static const int MAX_TEXT_LENGTH =
      mysql::gtid::tsid_max_length + 1 + MAX_GNO_TEXT_LENGTH;
  /**
    Returns true if parse() would succeed, but doesn't store the
    result anywhere
  */
  static bool is_valid(const char *text);
  /**
    Convert a Gtid to a string.
    @param tsid the TSID to use. This overrides the sidno of this Gtid.
    @param[out] buf Buffer to store the Gtid in (normally
    MAX_TEXT_LENGTH+1 bytes long).
    @return Length of the string, not counting '\0'.
  */
  int to_string(const Tsid &tsid, char *buf) const;

  /**
    Convert this Gtid to a string.
    @param tsid_map tsid_map to use when converting sidno to a TSID.
    @param[out] buf Buffer to store the Gtid in (normally
    MAX_TEXT_LENGTH+1 bytes long).
    @param need_lock If true, the function will acquire tsid_map->tsid_lock;
    otherwise it will assert that the lock is held.
    @return Length of the string, not counting '\0'.
  */
  int to_string(const Tsid_map *tsid_map, char *buf,
                bool need_lock = false) const;
  /// Returns true if this Gtid has the same sid and gno as 'other'.
  bool equals(const Gtid &other) const {
    return sidno == other.sidno && gno == other.gno;
  }
  /**
    Parses the given string and stores in this Gtid.

    @param tsid_map tsid_map to use when converting TSID to a sidno.
    @param text The text to parse
    @return status of operation
  */
  [[NODISCARD]] mysql::utils::Return_status parse(Tsid_map *tsid_map,
                                                  const char *text);

  /// @brief Parses TAG from a textual representation of the GTID (text)
  /// @param[in] text String with full GTID specification
  /// @param[in] pos Current position within a text
  /// @return Parsed tag
  ///         Updated position within text
  static std::pair<Tag, std::size_t> parse_tag_str(const char *text,
                                                   std::size_t pos);

  /// @brief Helper used to report BINLOG error
  /// @param[in] text String with full GTID specification
  static void report_parsing_error(const char *text);

  /// @brief Parses GTID from text. In case GTID is valid, it will return
  /// "ok" status code and a valid mysql::gtid::Gtid object. Otherwise,
  /// it will return an empty Gtid and error
  /// @param[in] text Text containing textual representation of a GTID
  static std::pair<mysql::utils::Return_status, mysql::gtid::Gtid>
  parse_gtid_from_cstring(const char *text);

  /// @brief Definition of GTID separator (colon) which separates UUID and GNO
  static constexpr auto gtid_separator = ':';

#ifndef NDEBUG
  /// Debug only: print this Gtid to stdout.
  void print(const Tsid_map *tsid_map) const {
    char buf[MAX_TEXT_LENGTH + 1];
    to_string(tsid_map, buf);
    printf("%s\n", buf);
  }
#endif
  /// Print this Gtid to the trace file if debug is enabled; no-op otherwise.
  void dbug_print(const Tsid_map *tsid_map [[maybe_unused]],
                  const char *text [[maybe_unused]] = "",
                  bool need_lock [[maybe_unused]] = false) const {
#ifndef NDEBUG
    char buf[MAX_TEXT_LENGTH + 1];
    to_string(tsid_map, buf, need_lock);
    DBUG_PRINT("info", ("%s%s%s", text, *text ? ": " : "", buf));
#endif
  }

 protected:
  /// @brief Converts internal gno into the string
  /// @param[out] buf Buffer to store the GNO in
  /// @return Length of the string
  int to_string_gno(char *buf) const;

  /// @brief Helper function used to skip whitespaces in GTID specification
  /// @param[in] text String with full GTID specification
  /// @param[in] pos Current position within a text
  /// @return Updated position in text
  static std::size_t skip_whitespace(const char *text, std::size_t pos);

  /// @brief Parses SID from a textual representation of the GTID.
  /// @param[in] text String with full GTID specification
  /// @param[in] pos Current position within a text
  /// @return operation status;
  ///         Parsed sidno in case status is equal to true,
  ///           0 otherwise;
  ///         Updated position within text up to which characters has
  ///           been accepted
  static std::tuple<mysql::utils::Return_status, rpl_sid, std::size_t>
  parse_sid_str(const char *text, std::size_t pos);

  /// @brief Parses GNO from a textual representation of the GTID (text)
  /// @param[in] text String with full GTID specification
  /// @param[in] pos Current position within a text
  /// @return operation status;
  ///         Parsed gno in case status is equal to true or 0;
  ///         Updated position within text
  static std::tuple<mysql::utils::Return_status, rpl_gno, std::size_t>
  parse_gno_str(const char *text, std::size_t pos);

  /// @brief Parses GTID separator from a textual representation of the GTID
  /// (text)
  /// @param[in] text String with full GTID specification
  /// @param[in] pos Current position within a text
  /// @return parsing status, error in case separator could not have been parsed
  ///         Updated position within text
  static std::pair<mysql::utils::Return_status, std::size_t>
  parse_gtid_separator(const char *text, std::size_t pos);
};

/// Structure to store the GTID and timing information.
struct Trx_monitoring_info {
  /// GTID being monitored.
  Gtid gtid;
  /// OCT of the GTID being monitored.
  ulonglong original_commit_timestamp;
  /// ICT of the GTID being monitored.
  ulonglong immediate_commit_timestamp;
  /// When the GTID transaction started to be processed.
  ulonglong start_time;
  /// When the GTID transaction finished to be processed.
  ulonglong end_time;
  /// True if the GTID is being applied but will be skipped.
  bool skipped;
  /// True when this information contains useful data.
  bool is_info_set;
  /// Number of the last transient error of this transaction
  uint last_transient_error_number;
  /// Message of the last transient error of this transaction
  char last_transient_error_message[MAX_SLAVE_ERRMSG];
  /// Timestamp in microseconds of the last transient error of this transaction
  ulonglong last_transient_error_timestamp;
  /// Number of times this transaction was retried
  ulong transaction_retries;
  /// True when the transaction is retrying
  bool is_retrying;
  /// The compression type
  mysql::binlog::event::compression::type compression_type;
  /// The compressed bytes
  ulonglong compressed_bytes;
  /// The uncompressed bytes
  ulonglong uncompressed_bytes;

  /// Constructor
  Trx_monitoring_info();
  /// Copy constructor
  Trx_monitoring_info(const Trx_monitoring_info &info);

  Trx_monitoring_info &operator=(const Trx_monitoring_info &) = default;

  /// Clear all fields of the structure.
  void clear();

  /**
    Copies this transaction monitoring information to the output parameters
    passed as input, which are the corresponding fields in a replication
    performance schema table.

    @param[in]  tsid_map                 The TSID map for the GTID.
    @param[out] gtid_arg                 GTID field in the PS table.
    @param[out] gtid_length_arg          Length of the GTID as string.
    @param[out] original_commit_ts_arg   The original commit timestamp.
    @param[out] immediate_commit_ts_arg  The immediate commit timestamp.
    @param[out] start_time_arg           The start time field.
  */
  void copy_to_ps_table(Tsid_map *tsid_map, char *gtid_arg,
                        uint *gtid_length_arg,
                        ulonglong *original_commit_ts_arg,
                        ulonglong *immediate_commit_ts_arg,
                        ulonglong *start_time_arg) const;

  /**
    Copies this transaction monitoring information to the output parameters
    passed as input, which are the corresponding fields in a replication
    performance schema table.

    @param[in]  tsid_map                 The TSID map for the GTID.
    @param[out] gtid_arg                 GTID field in the PS table.
    @param[out] gtid_length_arg          Length of the GTID as string.
    @param[out] original_commit_ts_arg   The original commit timestamp.
    @param[out] immediate_commit_ts_arg  The immediate commit timestamp.
    @param[out] start_time_arg           The start time field.
    @param[out] end_time_arg             The end time field. This can be null
                                         when the PS table fields are for the
                                         "still processing" information.
  */
  void copy_to_ps_table(Tsid_map *tsid_map, char *gtid_arg,
                        uint *gtid_length_arg,
                        ulonglong *original_commit_ts_arg,
                        ulonglong *immediate_commit_ts_arg,
                        ulonglong *start_time_arg,
                        ulonglong *end_time_arg) const;

  /**
    Copies this transaction monitoring information to the output parameters
    passed as input, which are the corresponding fields in a replication
    performance schema table.

    @param[in]  tsid_map                         The TSID map for the GTID.
    @param[out] gtid_arg                         GTID field in the PS table.
    @param[out] gtid_length_arg                  Length of the GTID as string.
    @param[out] original_commit_ts_arg           The original commit timestamp.
    @param[out] immediate_commit_ts_arg          The immediate commit timestamp.
    @param[out] start_time_arg                   The start time field.
    @param[out] last_transient_errno_arg         The last transient error
                                                 number.
    @param[out] last_transient_errmsg_arg        The last transient error
                                                 message.
    @param[out] last_transient_errmsg_length_arg Length of the last transient
                                                 error message.
    @param[out] last_transient_timestamp_arg     The last transient error
                                                 timestamp.
    @param[out] retries_count_arg                The total number of retries for
                                                 this transaction.
  */
  void copy_to_ps_table(
      Tsid_map *tsid_map, char *gtid_arg, uint *gtid_length_arg,
      ulonglong *original_commit_ts_arg, ulonglong *immediate_commit_ts_arg,
      ulonglong *start_time_arg, uint *last_transient_errno_arg,
      char *last_transient_errmsg_arg, uint *last_transient_errmsg_length_arg,
      ulonglong *last_transient_timestamp_arg, ulong *retries_count_arg) const;

  /**
    Copies this transaction monitoring information to the output parameters
    passed as input, which are the corresponding fields in a replication
    performance schema table.

    @param[in]  tsid_map                         The TSID map for the GTID.
    @param[out] gtid_arg                         GTID field in the PS table.
    @param[out] gtid_length_arg                  Length of the GTID as string.
    @param[out] original_commit_ts_arg           The original commit timestamp.
    @param[out] immediate_commit_ts_arg          The immediate commit timestamp.
    @param[out] start_time_arg                   The start time field.
    @param[out] end_time_arg                     The end time field. This can be
                                                 null when the PS table fields
                                                 are for the "still processing"
                                                 information.
    @param[out] last_transient_errno_arg         The last transient error
                                                 number.
    @param[out] last_transient_errmsg_arg        The last transient error
                                                 message.
    @param[out] last_transient_errmsg_length_arg Length of the last transient
                                                 error message.
    @param[out] last_transient_timestamp_arg     The last transient error
                                                 timestamp.
    @param[out] retries_count_arg                The total number of retries for
                                                 this transaction.
  */
  void copy_to_ps_table(
      Tsid_map *tsid_map, char *gtid_arg, uint *gtid_length_arg,
      ulonglong *original_commit_ts_arg, ulonglong *immediate_commit_ts_arg,
      ulonglong *start_time_arg, ulonglong *end_time_arg,
      uint *last_transient_errno_arg, char *last_transient_errmsg_arg,
      uint *last_transient_errmsg_length_arg,
      ulonglong *last_transient_timestamp_arg, ulong *retries_count_arg) const;
};

/**
  Stores information to monitor a transaction during the different replication
  stages.
*/
class Gtid_monitoring_info {
 public:
  /**
    Create this GTID monitoring info object.

    @param atomic_mutex_arg When specified, this object will rely on the mutex
                            to arbitrate the read/update access to object data.
                            This will be used by the receiver thread, relying
                            on mi->data_lock. When no mutex is specified, the
                            object will rely on its own atomic mechanism.
  */
  Gtid_monitoring_info(mysql_mutex_t *atomic_mutex_arg = nullptr);

  /// Destroy this GTID monitoring info object.
  ~Gtid_monitoring_info();

 protected:
  /// Holds information about transaction being processed.
  Trx_monitoring_info *processing_trx;
  /// Holds information about the last processed transaction.
  Trx_monitoring_info *last_processed_trx;

 private:
  /**
    Mutex arbitrating the atomic access to the object.

    Some Gtid_monitoring_info will rely on replication thread locks
    (i.e.: the Master_info's one rely on mi->data_lock, that is already
    acquired every time the Gtid_monitoring_info needs to be updated).

    Other Gtid_monitoring_info will rely on an atomic lock implemented
    in this class to avoid overlapped reads and writes over the information.
    (i.e.: the Relay_log_info's one sometimes is updated without rli locks).

    When atomic_mutex is NULL, the object will rely on its own atomic
    mechanism.
  */
  mysql_mutex_t *atomic_mutex;

  /// The atomic locked flag.
  std::atomic<bool> atomic_locked{false};
#ifndef NDEBUG
  /// Flag to assert the atomic lock behavior.
  bool is_locked = false;
#endif

 public:
  /**
    Lock this object when no thread mutex is used to arbitrate the access.
  */
  void atomic_lock();
  /**
    Unlock this object when no thread mutex is used to arbitrate the access.
  */
  void atomic_unlock();
  /**
    Clear all monitoring information.
  */
  void clear();
  /**
    Clear only the processing_trx monitoring info.
  */
  void clear_processing_trx();
  /**
    Clear only the last_processed_trx monitoring info.
  */
  void clear_last_processed_trx();

  /**
    Sets the initial monitoring information.
    @param gtid_arg         The Gtid to be stored.
    @param original_ts_arg  The original commit timestamp of the GTID.
    @param immediate_ts_arg The immediate commit timestamp of the GTID.
    @param skipped_arg      True if the GTID was already applied.
                            This only make sense for applier threads.
                            That's why it is false by default.
  */
  void start(Gtid gtid_arg, ulonglong original_ts_arg,
             ulonglong immediate_ts_arg, bool skipped_arg = false);

  void update(mysql::binlog::event::compression::type t, size_t payload_size,
              size_t uncompressed_size);

  /**
    Sets the final information, copy processing info to last_processed
    and clears processing info.
  */
  void finish();
  /**
    Copies both processing_trx and last_processed_trx info to other
    Trx_monitoring_info structures.

    @param[out] processing_dest     The destination of processing_trx.
    @param[out] last_processed_dest The destination of last_processed_trx.
  */
  void copy_info_to(Trx_monitoring_info *processing_dest,
                    Trx_monitoring_info *last_processed_dest);
  /**
    Copies all monitoring info to other Gtid_monitoring_info object.

    @param[out] dest     The destination Gtid_monitoring_info.
  */
  void copy_info_to(Gtid_monitoring_info *dest);
  /// Returns true if the processing_trx is set, false otherwise.
  bool is_processing_trx_set();
  /// Returns the GTID of the processing_trx.
  const Gtid *get_processing_trx_gtid();
  /**
    Stores the information about the last transient error in the current
    transaction, namely: the error number, message and total number of retries.
    It also sets the timestamp for this error.

    @param transient_errno_arg        The number of the transient error in this
                                      transaction.
    @param transient_err_message_arg  The message of this transient error.
    @param trans_retries_arg          The number of times this transaction has
                                      been retried.
  */
  void store_transient_error(uint transient_errno_arg,
                             const char *transient_err_message_arg,
                             ulong trans_retries_arg);
};

/**
  Represents a set of GTIDs.

  This is structured as an array, indexed by SIDNO, where each element
  contains a linked list of intervals.

  This data structure OPTIONALLY knows of a Tsid_map that gives a
  correspondence between SIDNO and TSID.  If the Tsid_map is NULL, then
  operations that require a Tsid_map - printing and parsing - raise an
  assertion.

  This data structure OPTIONALLY knows of a read-write lock that
  protects the number of SIDNOs.  The lock is provided by the invoker
  of the constructor and it is generally the caller's responsibility
  to acquire the read lock.  If the lock is not NULL, access methods
  assert that the caller already holds the read (or write) lock.  If
  the lock is not NULL and a method of this class grows the number of
  SIDNOs, then the method temporarily upgrades this lock to a write
  lock and then degrades it to a read lock again; there will be a
  short period when the lock is not held at all.
*/
class Gtid_set {
 public:
  static PSI_mutex_key key_gtid_executed_free_intervals_mutex;
  /**
    Constructs a new, empty Gtid_set.

    @param tsid_map The Tsid_map to use, or NULL if this Gtid_set
    should not have a Tsid_map.
    @param tsid_lock Read-write lock that protects updates to the
    number of TSIDs. This may be NULL if such changes do not need to be
    protected.
  */
  Gtid_set(Tsid_map *tsid_map, Checkable_rwlock *tsid_lock = nullptr);
  /**
    Constructs a new Gtid_set that contains the gtids in the given
    string, in the same format as add_gtid_text(char *).

    @param tsid_map The Tsid_map to use for TSIDs.
    @param text The text to parse.
    @param status Will be set to RETURN_STATUS_OK on success or
    RETURN_STATUS_REPORTED_ERROR on error.
    @param tsid_lock Read/write lock to protect changes in the number
    of SIDs with. This may be NULL if such changes do not need to be
    protected.
    If tsid_lock != NULL, then the read lock on tsid_lock must be held
    before calling this function. If the array is grown, tsid_lock is
    temporarily upgraded to a write lock and then degraded again;
    there will be a short period when the lock is not held at all.
  */
  Gtid_set(Tsid_map *tsid_map, const char *text, enum_return_status *status,
           Checkable_rwlock *tsid_lock = nullptr);

 private:
  /// Worker for the constructor.
  void init();

 public:
  /// Destroy this Gtid_set.
  ~Gtid_set();

  /**
    Claim ownership of memory.

    @param claim  claim ownership of memory.
  */
  void claim_memory_ownership(bool claim);

  /**
    Removes all gtids from this Gtid_set.

    This does not deallocate anything: if gtids are added later,
    existing allocated memory will be re-used.
  */
  void clear();
  /**
    Removes all gtids from this Gtid_set and clear all the sidnos
    used by the Gtid_set and it's TSID map.

    This does not deallocate anything: if gtids are added later,
    existing allocated memory will be re-used.
  */
  void clear_set_and_tsid_map();
  /**
    Adds the given GTID to this Gtid_set.

    The SIDNO must exist in the Gtid_set before this function is called.

    @param sidno SIDNO of the GTID to add.
    @param gno GNO of the GTID to add.
  */
  void _add_gtid(rpl_sidno sidno, rpl_gno gno) {
    DBUG_TRACE;
    assert(sidno > 0);
    assert(gno > 0);
    assert(gno < GNO_END);
    Interval_iterator ivit(this, sidno);
    Free_intervals_lock lock(this);
    add_gno_interval(&ivit, gno, gno + 1, &lock);
    return;
  }
  /**
    Removes the given GTID from this Gtid_set.

    @param sidno SIDNO of the GTID to remove.
    @param gno GNO of the GTID to remove.
  */
  void _remove_gtid(rpl_sidno sidno, rpl_gno gno) {
    DBUG_TRACE;
    if (sidno <= get_max_sidno()) {
      Interval_iterator ivit(this, sidno);
      Free_intervals_lock lock(this);
      remove_gno_interval(&ivit, gno, gno + 1, &lock);
    }
    return;
  }
  /**
    Adds the given GTID to this Gtid_set.

    The SIDNO must exist in the Gtid_set before this function is called.

    @param gtid Gtid to add.
  */
  void _add_gtid(const Gtid &gtid) { _add_gtid(gtid.sidno, gtid.gno); }
  /**
    Removes the given GTID from this Gtid_set.

    @param gtid Gtid to remove.
   */
  void _remove_gtid(const Gtid &gtid) { _remove_gtid(gtid.sidno, gtid.gno); }
  /**
    Adds all gtids from the given Gtid_set to this Gtid_set.

    If tsid_lock != NULL, then the read lock must be held before
    calling this function. If a new sidno is added so that the array
    of lists of intervals is grown, tsid_lock is temporarily upgraded
    to a write lock and then degraded again; there will be a short
    period when the lock is not held at all.

    @param other The Gtid_set to add.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status add_gtid_set(const Gtid_set *other);
  /**
    Removes all gtids in the given Gtid_set from this Gtid_set.

    @param other The Gtid_set to remove.
  */
  void remove_gtid_set(const Gtid_set *other);
  /**
    Removes all intervals of 'other' for a given SIDNO, from 'this'.

    Example:
    this = A:1-100, B:1-100
    other = A:1-100, B:1-50, C:1-100
    this.remove_intervals_for_sidno(other, B) = A:1-100, B:51-100

    It is not required that the intervals exist in this Gtid_set.

    @param other The set to remove.
    @param sidno The sidno to remove.
  */
  void remove_intervals_for_sidno(Gtid_set *other, rpl_sidno sidno);
  /**
    Adds the set of GTIDs represented by the given string to this Gtid_set.

    The string must have the format of a comma-separated list of zero
    or more of the following items:

       XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX(:NUMBER+(-NUMBER)?)*
       | ANONYMOUS

       Each X is a hexadecimal digit (upper- or lowercase).
       NUMBER is a decimal, 0xhex, or 0oct number.

       The start of an interval must be greater than 0. The end of an
       interval may be 0, but any interval that has an endpoint that
       is smaller than the start is discarded.

    The string can start with an optional '+' appender qualifier
    which triggers @c executed_gtids and @c lost_gtids set examination
    on the matter of disjointness with the one being added.

    If tsid_lock != NULL, then the read lock on tsid_lock must be held
    before calling this function. If a new sidno is added so that the
    array of lists of intervals is grown, tsid_lock is temporarily
    upgraded to a write lock and then degraded again; there will be a
    short period when the lock is not held at all.

    @param text The string to parse.
    @param [in,out] anonymous If this is NULL, ANONYMOUS is not
    allowed.  If this is not NULL, it will be set to true if the
    anonymous GTID was found; false otherwise.
    @param[in,out] starts_with_plus If this is not NULL, the string may
    optionally begin with a '+' character, and *starts_with_plus will
    be set to true if the plus character is present. If this is NULL,
    no plus is allowed at the begin of the string.

    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status add_gtid_text(const char *text, bool *anonymous = nullptr,
                                   bool *starts_with_plus = nullptr);

  /// @brief Adds specified GTID (TSID+GNO) to this Gtid_set.
  /// @param gtid mysql::gtid::Gtid object
  /// @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  [[NODISCARD]] enum_return_status add_gtid(const mysql::gtid::Gtid &gtid);

  /**
    Decodes a Gtid_set from the given string.

    @param encoded The string to parse.
    @param length The number of bytes.
    @param actual_length If this is not NULL, it is set to the number
    of bytes used by the encoding (which may be less than 'length').
    If this is NULL, an error is generated if the encoding is shorter
    than the given 'length'.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status add_gtid_encoding(const uchar *encoded, size_t length,
                                       size_t *actual_length = nullptr);
  /// Return true iff the given GTID exists in this set.
  bool contains_gtid(rpl_sidno sidno, rpl_gno gno) const;
  /// Return true iff the given GTID exists in this set.
  bool contains_gtid(const Gtid &gtid) const {
    return contains_gtid(gtid.sidno, gtid.gno);
  }
  // Get last gno or 0 if this set is empty.
  rpl_gno get_last_gno(rpl_sidno sidno) const;
  /// Returns the maximal sidno that this Gtid_set currently has space for.
  rpl_sidno get_max_sidno() const {
    if (tsid_lock) tsid_lock->assert_some_lock();
    return static_cast<rpl_sidno>(m_intervals.size());
  }
  /**
    Allocates space for all sidnos up to the given sidno in the array of
    intervals. The sidno must exist in the Tsid_map associated with this
    Gtid_set.

    If tsid_lock != NULL, then the read lock on tsid_lock must be held
    before calling this function. If the array is grown, tsid_lock is
    temporarily upgraded to a write lock and then degraded again;
    there will be a short period when the lock is not held at all.

    @param sidno The SIDNO.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status ensure_sidno(rpl_sidno sidno);
  /// Returns true if this Gtid_set is a subset of the other Gtid_set.
  bool is_subset(const Gtid_set *super) const;
  /// Returns true if this Gtid_set is a non equal subset of the other Gtid_set.
  bool is_subset_not_equals(const Gtid_set *super) const {
    return (is_subset(super) && !equals(super));
  }

  /**
    Returns true if this Gtid_set is a subset of the given gtid_set
    on the given superset_sidno and subset_sidno.

    @param super          Gtid_set with which this->gtid_set needs to be
                           compared
    @param superset_sidno The sidno that will be compared, relative to
                           super->tsid_map.
    @param subset_sidno   The sidno that will be compared, relative to
                           this->tsid_map.
    @return true          If 'this' Gtid_set is subset of given
                           'super' Gtid_set.
            false         If 'this' Gtid_set is *not* subset of given
                           'super' Gtid_set.
  */
  bool is_subset_for_sidno(const Gtid_set *super, rpl_sidno superset_sidno,
                           rpl_sidno subset_sidno) const;

  /// @brief Returns true if this Gtid_set is a subset of the given gtid_set
  /// with respect to the given sid
  /// @details This function will traverse all TSIDs with SID equal
  /// to "sid" parameter (all registered tags for a given SID)
  /// @param super Gtid_set with which this->gtid_set needs to be compared
  /// @param sid Sid for which we will do the testing
  /// @return True in case super is a superset for this gtid set w.r.t. the
  /// given sid; false otherwise
  bool is_subset_for_sid(const Gtid_set *super, const rpl_sid &sid) const;

  /// Returns true if there is a least one element of this Gtid_set in
  /// the other Gtid_set.
  bool is_intersection_nonempty(const Gtid_set *other) const;
  /**
    Add the intersection of this Gtid_set and the other Gtid_set to result.

    @param other The Gtid_set to intersect with this Gtid_set
    @param result Gtid_set where the result will be stored.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status intersection(const Gtid_set *other, Gtid_set *result);
  /// Returns true if this Gtid_set is empty.
  bool is_empty() const {
    Gtid_iterator git(this);
    return git.get().sidno == 0;
  }

  /**
    Return true if the size of the set is greater than or equal to the given
    number. The size is measure in number of GTIDs, i.e., total length of all
    intervals.

    @param num Number to compare with
    @retval true if the set contains >= num GTIDs.
    @retval false if the set contains < num GTIDs.
  */
  bool is_size_greater_than_or_equal(ulonglong num) const;

  /**
    What is the count of all the GTIDs in all intervals for a sidno

    @param sidno  The sidno that contains the intervals

    @return the number of all GTIDs in all intervals
  */
  ulonglong get_gtid_count(rpl_sidno sidno) const {
    Const_interval_iterator ivit(this, sidno);
    ulonglong ret = 0;
    while (ivit.get() != nullptr) {
      ret += ivit.get()->end - ivit.get()->start;
      ivit.next();
    }
    return ret;
  }

  /// @brief Returns the number of GTIDs
  /// @returns the number of GTIDs on this set
  std::size_t get_count() const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_wrlock();
    rpl_sidno max_sidno = get_max_sidno();
    std::size_t count{0};

    for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++) {
      count += get_gtid_count(sidno);
    }
    return count;
  }

  /**
    Returns true if this Gtid_set contains at least one GTID with
    the given SIDNO.

    @param sidno The SIDNO to test.
    @retval true The SIDNO is less than or equal to the max SIDNO, and
    there is at least one GTID with this SIDNO.
    @retval false The SIDNO is greater than the max SIDNO, or there is
    no GTID with this SIDNO.
  */
  bool contains_sidno(rpl_sidno sidno) const {
    assert(sidno >= 1);
    if (sidno > get_max_sidno()) return false;
    Const_interval_iterator ivit(this, sidno);
    return ivit.get() != nullptr;
  }
  /**
    Returns true if the given string is a valid specification of a
    Gtid_set, false otherwise.
  */
  static bool is_valid(const char *text);

  /**
    Class Gtid_set::String_format defines the separators used by
    Gtid_set::to_string.
  */
  struct String_format {
    /// The generated string begins with this.
    const char *begin;
    /// The generated string begins with this.
    const char *end;
    /// In 'SID:TAG', this is the ':'
    const char *tag_sid_separator;
    /// In 'TSID:GNO', this is the ':'
    const char *tsid_gno_separator;
    /// In 'SID:GNO-GNO', this is the '-'
    const char *gno_start_end_separator;
    /// In 'SID:GNO:GNO', this is the second ':'
    const char *gno_gno_separator;
    /// In 'SID:GNO,SID:GNO', this is the ','
    const char *gno_sid_separator;
    /// If the set is empty and this is not NULL, then this string is generated.
    const char *empty_set_string;
    /// The following fields are the lengths of each field above.
    const int begin_length;
    const int end_length;
    const int tag_sid_separator_length;
    const int tsid_gno_separator_length;
    const int gno_start_end_separator_length;
    const int gno_gno_separator_length;
    const int gno_sid_separator_length;
    const int empty_set_string_length;
  };

  /// @brief Checks if this Gtid set contains any tagged GTIDs
  /// @retval true This gtid set contains tagged GTIDs
  /// @retval false This gtid set contains only untagged GTIDs
  bool contains_tags() const;
  /**
    Returns the length of the output from to_string.

    @warning This does not include the trailing '\0', so your buffer
    needs space for get_string_length() + 1 characters.

    @param string_format String_format object that specifies
    separators in the resulting text.
    @return The length.
  */
  size_t get_string_length(const String_format *string_format = nullptr) const;
  /**
    Formats this Gtid_set as a string and saves in a given buffer.

    @param[out] buf Pointer to the buffer where the string should be
    stored. This should have size at least get_string_length()+1.
    @param need_lock If this Gtid_set has a tsid_lock, then the write
    lock must be held while generating the string. If this parameter
    is true, then this function acquires and releases the lock;
    otherwise it asserts that the caller holds the lock.
    @param string_format String_format object that specifies
    separators in the resulting text.
    @return Length of the generated string.
  */
  size_t to_string(char *buf, bool need_lock = false,
                   const String_format *string_format = nullptr) const;

  /**
    Formats a Gtid_set as a string and saves in a newly allocated buffer.
    @param[out] buf Pointer to pointer to string. The function will
    set it to point to the newly allocated buffer, or NULL on out of memory.
    @param need_lock If this Gtid_set has a tsid_lock, then the write
    lock must be held while generating the string. If this parameter
    is true, then this function acquires and releases the lock;
    otherwise it asserts that the caller holds the lock.
    @param string_format Specifies how to format the string.
    @retval Length of the generated string, or -1 on out of memory.
  */
  long to_string(char **buf, bool need_lock = false,
                 const String_format *string_format = nullptr) const;
#ifndef NDEBUG
  /// Debug only: Print this Gtid_set to stdout.

  /// For use with C `printf`
  void print(bool need_lock = false,
             const Gtid_set::String_format *sf = nullptr) const {
    char *str;
    to_string(&str, need_lock, sf);
    printf("%s\n", str ? str : "out of memory in Gtid_set::print");
    my_free(str);
  }

  /// For use with C++ `std::ostream`
  inline friend std::ostream &operator<<(std::ostream &os, const Gtid_set &in) {
    char *str;
    in.to_string(&str, true, nullptr);
    os << std::string(str) << std::flush;
    my_free(str);
    return os;
  }
#endif
  /**
    Print this Gtid_set to the trace file if debug is enabled; no-op
    otherwise.
  */
  void dbug_print(const char *text [[maybe_unused]] = "",
                  bool need_lock [[maybe_unused]] = false,
                  const Gtid_set::String_format *sf
                  [[maybe_unused]] = nullptr) const {
#ifndef NDEBUG
    char *str;
    to_string(&str, need_lock, sf);
    DBUG_PRINT("info", ("%s%s'%s'", text, *text ? ": " : "",
                        str ? str : "out of memory in Gtid_set::dbug_print"));
    my_free(str);
#endif
  }
  /**
    Gets all gtid intervals from this Gtid_set.

    @param[out] gtid_intervals Store all gtid intervals from this Gtid_set.
  */
  void get_gtid_intervals(std::list<Gtid_interval> *gtid_intervals) const;
  /**
    The default String_format: the format understood by
    add_gtid_text(const char *).
  */
  static const String_format default_string_format;
  /**
    String_format useful to generate an SQL string: the string is
    wrapped in single quotes and there is a newline between SIDs.
  */
  static const String_format sql_string_format;
  /**
    String_format for printing the Gtid_set commented: the string is
    not quote-wrapped, and every TSID is on a new line with a leading '# '.
  */
  static const String_format commented_string_format;

  /// Return the Tsid_map associated with this Gtid_set.
  Tsid_map *get_tsid_map() const { return tsid_map; }

  /**
    Represents one element in the linked list of intervals associated
    with a SIDNO.
  */
  struct Interval {
   public:
    /// The first GNO of this interval.
    rpl_gno start;
    /// The first GNO after this interval.
    rpl_gno end;
    /// Return true iff this interval is equal to the given interval.
    bool equals(const Interval &other) const {
      return start == other.start && end == other.end;
    }
    /// Pointer to next interval in list.
    Interval *next;
  };

  /**
    Provides an array of Intervals that this Gtid_set can use when
    gtids are subsequently added.  This can be used as an
    optimization, to reduce allocation for sets that have a known
    number of intervals.

    @param n_intervals The number of intervals to add.
    @param intervals_param Array of n_intervals intervals.
  */
  void add_interval_memory(int n_intervals, Interval *intervals_param) {
    if (tsid_lock != nullptr) mysql_mutex_lock(&free_intervals_mutex);
    add_interval_memory_lock_taken(n_intervals, intervals_param);
    if (tsid_lock != nullptr) mysql_mutex_unlock(&free_intervals_mutex);
  }

  /**
    Iterator over intervals for a given SIDNO.

    This is an abstract template class, used as a common base class
    for Const_interval_iterator and Interval_iterator.

    The iterator always points to an interval pointer.  The interval
    pointer is either the initial pointer into the list, or the next
    pointer of one of the intervals in the list.
  */
  template <typename Gtid_set_p, typename Interval_p>
  class Interval_iterator_base {
   public:
    /**
      Construct a new iterator over the GNO intervals for a given Gtid_set.

      @param gtid_set The Gtid_set.
      @param sidno The SIDNO.
    */
    Interval_iterator_base(Gtid_set_p gtid_set, rpl_sidno sidno) {
      assert(sidno >= 1 && sidno <= gtid_set->get_max_sidno());
      init(gtid_set, sidno);
    }
    /// Construct a new iterator over the free intervals of a Gtid_set.
    Interval_iterator_base(Gtid_set_p gtid_set) {
      p = const_cast<Interval_p *>(&gtid_set->free_intervals);
    }
    /// Reset this iterator.
    inline void init(Gtid_set_p gtid_set, rpl_sidno sidno) {
      p = const_cast<Interval_p *>(&gtid_set->m_intervals[sidno - 1]);
    }
    /// Advance current_elem one step.
    inline void next() {
      assert(*p != nullptr);
      p = const_cast<Interval_p *>(&(*p)->next);
    }
    /// Return current_elem.
    inline Interval_p get() const { return *p; }

   protected:
    /**
      Holds the address of the 'next' pointer of the previous element,
      or the address of the initial pointer into the list, if the
      current element is the first element.
    */
    Interval_p *p;
  };

  /**
    Iterator over intervals of a const Gtid_set.
  */
  class Const_interval_iterator
      : public Interval_iterator_base<const Gtid_set *, const Interval *> {
   public:
    /// Create this Const_interval_iterator.
    Const_interval_iterator(const Gtid_set *gtid_set, rpl_sidno sidno)
        : Interval_iterator_base<const Gtid_set *, const Interval *>(gtid_set,
                                                                     sidno) {}
    /// Create this Const_interval_iterator.
    Const_interval_iterator(const Gtid_set *gtid_set)
        : Interval_iterator_base<const Gtid_set *, const Interval *>(gtid_set) {
    }
  };

  /**
    Iterator over intervals of a non-const Gtid_set, with additional
    methods to modify the Gtid_set.
  */
  class Interval_iterator
      : public Interval_iterator_base<Gtid_set *, Interval *> {
   public:
    /// Create this Interval_iterator.
    Interval_iterator(Gtid_set *gtid_set, rpl_sidno sidno)
        : Interval_iterator_base<Gtid_set *, Interval *>(gtid_set, sidno) {}
    /// Destroy this Interval_iterator.
    Interval_iterator(Gtid_set *gtid_set)
        : Interval_iterator_base<Gtid_set *, Interval *>(gtid_set) {}

   private:
    /**
      Set current_elem to the given Interval but do not touch the
      next pointer of the given Interval.
    */
    inline void set(Interval *iv) { *p = iv; }
    /// Insert the given element before current_elem.
    inline void insert(Interval *iv) {
      iv->next = *p;
      set(iv);
    }
    /// Remove current_elem.
    inline void remove(Gtid_set *gtid_set) {
      assert(get() != nullptr);
      Interval *next = (*p)->next;
      gtid_set->put_free_interval(*p);
      set(next);
    }
    /**
      Only Gtid_set is allowed to use set/insert/remove.

      They are not safe to use from other code because: (1) very easy
      to make a mistakes (2) they don't clear cached_string_format or
      cached_string_length.
    */
    friend class Gtid_set;
  };

  /**
    Iterator over all gtids in a Gtid_set.  This is a const
    iterator; it does not allow modification of the Gtid_set.
  */
  class Gtid_iterator {
   public:
    Gtid_iterator(const Gtid_set *gs) : gtid_set(gs), sidno(0), ivit(gs) {
      if (gs->tsid_lock != nullptr) gs->tsid_lock->assert_some_wrlock();
      next_sidno();
    }
    /// Advance to next gtid.
    inline void next() {
      assert(gno > 0 && sidno > 0);
      // go to next GTID in current interval
      gno++;
      // end of interval? then go to next interval for this sidno
      if (gno == ivit.get()->end) {
        ivit.next();
        const Interval *iv = ivit.get();
        // last interval for this sidno? then go to next sidno
        if (iv == nullptr) {
          next_sidno();
          // last sidno? then don't try more
          if (sidno == 0) return;
          iv = ivit.get();
        }
        gno = iv->start;
      }
    }
    /// Return next gtid, or {0,0} if we reached the end.
    inline Gtid get() const {
      Gtid ret = {sidno, gno};
      return ret;
    }

   private:
    /// Find the next sidno that has one or more intervals.
    inline void next_sidno() {
      const Interval *iv;
      do {
        sidno++;
        if (sidno > gtid_set->get_max_sidno()) {
          sidno = 0;
          gno = 0;
          return;
        }
        ivit.init(gtid_set, sidno);
        iv = ivit.get();
      } while (iv == nullptr);
      gno = iv->start;
    }
    /// The Gtid_set we iterate over.
    const Gtid_set *gtid_set;
    /**
      The SIDNO of the current element, or 0 if the iterator is past
      the last element.
    */
    rpl_sidno sidno;
    /**
      The GNO of the current element, or 0 if the iterator is past the
      last element.
    */
    rpl_gno gno;
    /// Iterator over the intervals for the current SIDNO.
    Const_interval_iterator ivit;
  };

 public:
  /// @brief Encodes this Gtid_set as a binary string.
  /// @param buf Buffer to write into
  /// @param skip_tagged_gtids When true, tagged GTIDS will be filtered out
  void encode(uchar *buf, bool skip_tagged_gtids = false) const;

  /// @brief Returns the length of this Gtid_set when encoded using the
  /// encode() function. Before calculation, analyzes GTID set format
  /// @param skip_tagged_gtids When true, tagged GTIDS will be filtered out
  /// @return length of Gtid_set encoding
  size_t get_encoded_length(bool skip_tagged_gtids = false) const;

  /// Returns the length of this Gtid_set when encoded using the
  /// encode() function. Uses already analyzed GTID set format (faster version)
  /// @param format Gtid format
  /// @param skip_tagged_gtids When true, tagged GTIDS will be filtered out
  /// @return Encoded gtid_set length
  std::size_t get_encoded_length(const mysql::gtid::Gtid_format &format,
                                 bool skip_tagged_gtids) const;

  /// Returns true if this Gtid_set is equal to the other Gtid_set.
  /// @param[in] other Gtid set to compare against
  /// @return true in case gtid sets contain the same GTIDs
  bool equals(const Gtid_set *other) const;

 private:
  /**
    Contains a list of intervals allocated by this Gtid_set.  When a
    method of this class needs a new interval and there are no more
    free intervals, a new Interval_chunk is allocated and the
    intervals of it are added to the list of free intervals.
  */
  struct Interval_chunk {
    Interval_chunk *next;
    Interval intervals[1];
  };
  /// The default number of intervals in an Interval_chunk.
  static const int CHUNK_GROW_SIZE = 8;

  /**
    Return true if the given sidno of this Gtid_set contains the same
    intervals as the given sidno of the other Gtid_set.

    @param sidno SIDNO to check for this Gtid_set.
    @param other Other Gtid_set
    @param other_sidno SIDNO to check in other.
    @return true if equal, false is not equal.
  */
  bool sidno_equals(rpl_sidno sidno, const Gtid_set *other,
                    rpl_sidno other_sidno) const;

  /// Return the number of intervals for the given sidno.
  int get_n_intervals(rpl_sidno sidno) const {
    Const_interval_iterator ivit(this, sidno);
    int ret = 0;
    while (ivit.get() != nullptr) {
      ret++;
      ivit.next();
    }
    return ret;
  }
  /// Return the number of intervals in this Gtid_set.
  int get_n_intervals() const {
    if (tsid_lock != nullptr) tsid_lock->assert_some_wrlock();
    rpl_sidno max_sidno = get_max_sidno();
    int ret = 0;
    for (rpl_sidno sidno = 1; sidno < max_sidno; sidno++)
      ret += get_n_intervals(sidno);
    return ret;
  }

  /// @brief Goes through recorded tsids. In case any of the TSIDs has a tag,
  /// this function will return Gtid_format::tagged. Otherwise, it will
  /// return Gtid_format::untagged
  /// @param skip_tagged_gtids When true, function will always return
  /// Gtid_format::untagged
  /// @returns Gtid encoding format
  mysql::gtid::Gtid_format analyze_encoding_format(
      bool skip_tagged_gtids) const;

  /**
    Allocates a new chunk of Intervals and adds them to the list of
    unused intervals.

    @param size The number of intervals in this chunk
  */
  void create_new_chunk(int size);
  /**
    Returns a fresh new Interval object.

    This usually does not require any real allocation, it only pops
    the first interval from the list of free intervals.  If there are
    no free intervals, it calls create_new_chunk.

    @param out The resulting Interval* will be stored here.
  */
  void get_free_interval(Interval **out);
  /**
    Puts the given interval in the list of free intervals.  Does not
    unlink it from its place in any other list.
  */
  void put_free_interval(Interval *iv);
  /**
    Like add_interval_memory, but does not acquire
    free_intervals_mutex.
    @see Gtid_set::add_interval_memory
  */
  void add_interval_memory_lock_taken(int n_ivs, Interval *ivs);

  /// Read-write lock that protects updates to the number of TSIDs.
  mutable Checkable_rwlock *tsid_lock;
  /**
    Lock protecting the list of free intervals.  This lock is only
    used if tsid_lock is not NULL.
  */
  mysql_mutex_t free_intervals_mutex;
  /**
    Class representing a lock on free_intervals_mutex.

    This is used by the add_* and remove_* functions.  The lock is
    declared by the top-level function and a pointer to the lock is
    passed down to low-level functions. If the low-level function
    decides to access the free intervals list, then it acquires the
    lock.  The lock is then automatically released by the destructor
    when the top-level function returns.

    The lock is not taken if Gtid_set->tsid_lock == NULL; such
    Gtid_sets are assumed to be thread-local.
  */
  class Free_intervals_lock {
   public:
    /// Create a new lock, but do not acquire it.
    Free_intervals_lock(Gtid_set *_gtid_set)
        : gtid_set(_gtid_set), locked(false) {}
    /// Lock the lock if it is not already locked.
    void lock_if_not_locked() {
      if (gtid_set->tsid_lock && !locked) {
        mysql_mutex_lock(&gtid_set->free_intervals_mutex);
        locked = true;
      }
    }
    /// Lock the lock if it is locked.
    void unlock_if_locked() {
      if (gtid_set->tsid_lock && locked) {
        mysql_mutex_unlock(&gtid_set->free_intervals_mutex);
        locked = false;
      }
    }
    /// Destroy this object and unlock the lock if it is locked.
    ~Free_intervals_lock() { unlock_if_locked(); }

   private:
    Gtid_set *gtid_set;
    bool locked;
  };
  void assert_free_intervals_locked() {
    if (tsid_lock != nullptr) mysql_mutex_assert_owner(&free_intervals_mutex);
  }

  /**
    Adds the interval (start, end) to the given Interval_iterator.

    This is the lowest-level function that adds gtids; this is where
    Interval objects are added, grown, or merged.

    @param ivitp Pointer to iterator.  After this function returns,
    the current_element of the iterator will be the interval that
    contains start and end.
    @param start The first GNO in the interval.
    @param end The first GNO after the interval.
    @param lock If this function has to add or remove an interval,
    then this lock will be taken unless it is already taken.  This
    mechanism means that the lock will be taken lazily by
    e.g. add_gtid_set() the first time that the list of free intervals
    is accessed, and automatically released when add_gtid_set()
    returns.
  */
  void add_gno_interval(Interval_iterator *ivitp, rpl_gno start, rpl_gno end,
                        Free_intervals_lock *lock);
  /**
    Removes the interval (start, end) from the given
    Interval_iterator. This is the lowest-level function that removes
    gtids; this is where Interval objects are removed, truncated, or
    split.

    It is not required that the gtids in the interval exist in this
    Gtid_set.

    @param ivitp Pointer to iterator.  After this function returns,
    the current_element of the iterator will be the next interval
    after end.
    @param start The first GNO in the interval.
    @param end The first GNO after the interval.
    @param lock If this function has to add or remove an interval,
    then this lock will be taken unless it is already taken.  This
    mechanism means that the lock will be taken lazily by
    e.g. add_gtid_set() the first time that the list of free intervals
    is accessed, and automatically released when add_gtid_set()
    returns.
  */
  void remove_gno_interval(Interval_iterator *ivitp, rpl_gno start, rpl_gno end,
                           Free_intervals_lock *lock);
  /**
    Adds a list of intervals to the given SIDNO.

    The SIDNO must exist in the Gtid_set before this function is called.

    @param sidno The SIDNO to which intervals will be added.
    @param ivit Iterator over the intervals to add. This is typically
    an iterator over some other Gtid_set.
    @param lock If this function has to add or remove an interval,
    then this lock will be taken unless it is already taken.  This
    mechanism means that the lock will be taken lazily by
    e.g. add_gtid_set() the first time that the list of free intervals
    is accessed, and automatically released when add_gtid_set()
    returns.
  */
  void add_gno_intervals(rpl_sidno sidno, Const_interval_iterator ivit,
                         Free_intervals_lock *lock);
  /**
    Removes a list of intervals from the given SIDNO.

    It is not required that the intervals exist in this Gtid_set.

    @param sidno The SIDNO from which intervals will be removed.
    @param ivit Iterator over the intervals to remove. This is typically
    an iterator over some other Gtid_set.
    @param lock If this function has to add or remove an interval,
    then this lock will be taken unless it is already taken.  This
    mechanism means that the lock will be taken lazily by
    e.g. add_gtid_set() the first time that the list of free intervals
    is accessed, and automatically released when add_gtid_set()
    returns.
  */
  void remove_gno_intervals(rpl_sidno sidno, Const_interval_iterator ivit,
                            Free_intervals_lock *lock);

  /// Returns true if every interval of sub is a subset of some
  /// interval of super.
  static bool is_interval_subset(Const_interval_iterator *sub,
                                 Const_interval_iterator *super);
  /// Returns true if at least one sidno in ivit1 is also in ivit2.
  static bool is_interval_intersection_nonempty(Const_interval_iterator *ivit1,
                                                Const_interval_iterator *ivit2);

  /// Tsid_map associated with this Gtid_set.
  Tsid_map *tsid_map;
  /**
    Array where the N'th element contains the head pointer to the
    intervals of SIDNO N+1.
  */
  Prealloced_array<Interval *, 8> m_intervals;
  /// Linked list of free intervals.
  Interval *free_intervals;
  /// Linked list of chunks.
  Interval_chunk *chunks;
  /// If the string is cached.
  mutable bool has_cached_string_length;
  /// The string length.
  mutable size_t cached_string_length;
  /// The String_format that was used when cached_string_length was computed.
  mutable const String_format *cached_string_format;
#ifndef NDEBUG
  /**
    The number of chunks.  Used only to check some invariants when
    DBUG is on.
  */
  int n_chunks;
#endif
  /// Used by unit tests that need to access private members.
#ifdef FRIEND_OF_GTID_SET
  friend FRIEND_OF_GTID_SET;
#endif
  /// Only Free_intervals_lock is allowed to access free_intervals_mutex.
  friend class Gtid_set::Free_intervals_lock;
};

/**
  Holds information about a Gtid_set.  Can also be NULL.

  This is used as backend storage for @@session.gtid_next_list.  The
  idea is that we allow the user to set this to NULL, but we keep the
  Gtid_set object so that we can re-use the allocated memory and
  avoid costly allocations later.

  This is stored in struct system_variables (defined in sql_class.h),
  which is cleared using memset(0); hence the negated form of
  is_non_null.

  The convention is: if is_non_null is false, then the value of the
  session variable is NULL, and the field gtid_set may be NULL or
  non-NULL.  If is_non_null is true, then the value of the session
  variable is not NULL, and the field gtid_set has to be non-NULL.

  This is a POD. It has to be a POD because it is stored in
  THD::variables.
*/
struct Gtid_set_or_null {
  /// Pointer to the Gtid_set.
  Gtid_set *gtid_set;
  /// True if this Gtid_set is NULL.
  bool is_non_null;
  /// Return NULL if this is NULL, otherwise return the Gtid_set.
  inline Gtid_set *get_gtid_set() const {
    assert(!(is_non_null && gtid_set == nullptr));
    return is_non_null ? gtid_set : nullptr;
  }
  /**
    Do nothing if this object is non-null; set to empty set otherwise.

    @return NULL if out of memory; Gtid_set otherwise.
  */
  Gtid_set *set_non_null(Tsid_map *sm) {
    if (!is_non_null) {
      if (gtid_set == nullptr)
        gtid_set = new Gtid_set(sm);
      else
        gtid_set->clear();
    }
    is_non_null = (gtid_set != nullptr);
    return gtid_set;
  }
  /// Set this Gtid_set to NULL.
  inline void set_null() { is_non_null = false; }
};

/**
  Represents the set of GTIDs that are owned by some thread.

  This data structure has a read-write lock that protects the number
  of SIDNOs.  The lock is provided by the invoker of the constructor
  and it is generally the caller's responsibility to acquire the read
  lock.  Access methods assert that the caller already holds the read
  (or write) lock.  If a method of this class grows the number of
  SIDNOs, then the method temporarily upgrades this lock to a write
  lock and then degrades it to a read lock again; there will be a
  short period when the lock is not held at all.

  The internal representation is a multi-valued map from GTIDs to
  threads, mapping GTIDs to one or more threads that owns it.

  In Group Replication multiple threads can own a GTID whereas if GR
  is disabeld there is at most one owner per GTID.
*/
class Owned_gtids {
 public:
  /**
    Constructs a new, empty Owned_gtids object.

    @param tsid_lock Read-write lock that protects updates to the
    number of TSIDs.
  */
  Owned_gtids(Checkable_rwlock *tsid_lock);
  /// Destroys this Owned_gtids.
  ~Owned_gtids();
  /**
    Add a GTID to this Owned_gtids.

    @param gtid The Gtid to add.
    @param owner The my_thread_id of the gtid to add.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status add_gtid_owner(const Gtid &gtid, my_thread_id owner);

  /*
    Fill all gtids into the given Gtid_set object. It doesn't clear the given
    gtid set before filling its owned gtids into it.
  */
  void get_gtids(Gtid_set &gtid_set) const;
  /**
    Removes the given GTID.

    If the gtid does not exist in this Owned_gtids object, does
    nothing.

    @param gtid The Gtid.
    @param owner thread_id of the owner thread
  */
  void remove_gtid(const Gtid &gtid, const my_thread_id owner);
  /**
    Ensures that this Owned_gtids object can accommodate SIDNOs up to
    the given SIDNO.

    If this Owned_gtids object needs to be resized, then the lock
    will be temporarily upgraded to a write lock and then degraded to
    a read lock again; there will be a short period when the lock is
    not held at all.

    @param sidno The SIDNO.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status ensure_sidno(rpl_sidno sidno);
  /// Returns true if there is a least one element of this Owned_gtids
  /// set in the other Gtid_set.
  bool is_intersection_nonempty(const Gtid_set *other) const;
  /// Returns true if this Owned_gtids is empty.
  bool is_empty() const {
    Gtid_iterator git(this);
    return git.get().sidno == 0;
  }
  /// Returns the maximal sidno that this Owned_gtids currently has space for.
  rpl_sidno get_max_sidno() const {
    if (tsid_lock != nullptr) {
      tsid_lock->assert_some_lock();
    }
    return static_cast<rpl_sidno>(sidno_to_hash.size());
  }

  /**
    Write a string representation of this Owned_gtids to the given buffer.

    @param out Buffer to write to.
    @return Number of characters written.
  */
  int to_string(char *out) const {
    char *p = out;
    rpl_sidno max_sidno = get_max_sidno();
    for (const auto &sid_it : global_tsid_map->get_sorted_sidno()) {
      rpl_sidno sidno = sid_it.second;
      if (sidno > max_sidno) continue;
      bool printed_sid = false;
      for (const auto &key_and_value : *get_hash(sidno)) {
        Node *node = key_and_value.second.get();
        assert(node != nullptr);
        if (!printed_sid) {
          p += global_tsid_map->sidno_to_tsid(sidno).to_string(p);
          printed_sid = true;
        }
        p += sprintf(p, ":%" PRId64 "#%u", node->gno, node->owner);
      }
    }
    *p = 0;
    return (int)(p - out);
  }

  /**
    Return an upper bound on the length of the string representation
    of this Owned_gtids.  The actual length may be smaller.  This
    includes the trailing '\0'.
  */
  size_t get_max_string_length() const {
    rpl_sidno max_sidno = get_max_sidno();
    size_t ret = 0;
    for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++) {
      size_t records = get_hash(sidno)->size();
      if (records > 0)
        ret +=
            mysql::gtid::tsid_max_length +
            records * (1 + MAX_GNO_TEXT_LENGTH + 1 + MAX_THREAD_ID_TEXT_LENGTH);
    }
    return 1 + ret;
  }

  /**
    Return true if the given thread is the owner of any gtids.
  */
  bool thread_owns_anything(my_thread_id thd_id) const {
    Gtid_iterator git(this);
    Node *node = git.get_node();
    while (node != nullptr) {
      if (node->owner == thd_id) return true;
      git.next();
      node = git.get_node();
    }
    return false;
  }

#ifndef NDEBUG
  /**
    Debug only: return a newly allocated string representation of
    this Owned_gtids.
  */
  char *to_string() const {
    char *str = (char *)my_malloc(key_memory_Owned_gtids_to_string,
                                  get_max_string_length(), MYF(MY_WME));
    assert(str != nullptr);
    to_string(str);
    return str;
  }
  /// Debug only: print this Owned_gtids to stdout.
  void print() const {
    char *str = to_string();
    printf("%s\n", str);
    my_free(str);
  }
#endif
  /**
    Print this Owned_gtids to the trace file if debug is enabled; no-op
    otherwise.
  */
  void dbug_print(const char *text [[maybe_unused]] = "") const {
#ifndef NDEBUG
    char *str = to_string();
    DBUG_PRINT("info", ("%s%s%s", text, *text ? ": " : "", str));
    my_free(str);
#endif
  }

  /**
    If thd_id==0, returns true when gtid is not owned by any thread.
    If thd_id!=0, returns true when gtid is owned by that thread.
  */
  bool is_owned_by(const Gtid &gtid, const my_thread_id thd_id) const;

 private:
  /// Represents one owned GTID.
  struct Node {
    /// GNO of the GTID.
    rpl_gno gno;
    /// Owner of the GTID.
    my_thread_id owner;
  };
  /// Read-write lock that protects updates to the number of TSIDs.
  mutable Checkable_rwlock *tsid_lock;
  /// Returns the hash for the given SIDNO.
  malloc_unordered_multimap<rpl_gno, unique_ptr_my_free<Node>> *get_hash(
      rpl_sidno sidno) const {
    assert(sidno >= 1 && sidno <= get_max_sidno());
    if (tsid_lock != nullptr) {
      tsid_lock->assert_some_lock();
    }
    return sidno_to_hash[sidno - 1];
  }
  /// Return true iff this Owned_gtids object contains the given gtid.
  bool contains_gtid(const Gtid &gtid) const;

  /// Growable array of hashes.
  Prealloced_array<
      malloc_unordered_multimap<rpl_gno, unique_ptr_my_free<Node>> *, 8>
      sidno_to_hash;

 public:
  /**
    Iterator over all gtids in a Owned_gtids set.  This is a const
    iterator; it does not allow modification of the set.
  */
  class Gtid_iterator {
   public:
    Gtid_iterator(const Owned_gtids *og)
        : owned_gtids(og), sidno(1), hash(nullptr), node(nullptr) {
      max_sidno = owned_gtids->get_max_sidno();
      if (sidno <= max_sidno) {
        hash = owned_gtids->get_hash(sidno);
        node_it = hash->begin();
      }
      next();
    }
    /// Advance to next GTID.
    inline void next() {
#ifndef NDEBUG
      if (owned_gtids->tsid_lock) owned_gtids->tsid_lock->assert_some_wrlock();
#endif

      while (sidno <= max_sidno) {
        assert(hash != nullptr);
        if (node_it != hash->end()) {
          node = node_it->second.get();
          assert(node != nullptr);
          // Jump to next node on next iteration.
          ++node_it;
          return;
        }

        // hash is initialized on constructor or in previous iteration
        // for current SIDNO, so we must increment for next iteration.
        sidno++;
        if (sidno <= max_sidno) {
          hash = owned_gtids->get_hash(sidno);
          node_it = hash->begin();
        }
      }
      node = nullptr;
    }
    /// Return next GTID, or {0,0} if we reached the end.
    inline Gtid get() const {
      Gtid ret = {0, 0};
      if (node) {
        ret.sidno = sidno;
        ret.gno = node->gno;
      }
      return ret;
    }
    /// Return the current GTID Node, or NULL if we reached the end.
    inline Node *get_node() const { return node; }

   private:
    /// The Owned_gtids set we iterate over.
    const Owned_gtids *owned_gtids;
    /// The SIDNO of the current element, or 1 in the initial iteration.
    rpl_sidno sidno;
    /// Max SIDNO of the current iterator.
    rpl_sidno max_sidno;
    /// Current SIDNO hash.
    malloc_unordered_multimap<rpl_gno, unique_ptr_my_free<Node>> *hash;
    /// Current node iterator on current SIDNO hash.
    malloc_unordered_multimap<rpl_gno, unique_ptr_my_free<Node>>::const_iterator
        node_it;
    /// Current node on current SIDNO hash.
    Node *node;
  };
};

/**
  Represents the server's GTID state: the set of committed GTIDs, the
  set of lost gtids, the set of owned gtids, the owner of each owned
  gtid, and a Mutex_cond_array that protects updates to gtids of
  each SIDNO.

  Locking:

  This data structure has a read-write lock that protects the number
  of SIDNOs, and a Mutex_cond_array that contains one mutex per SIDNO.
  The rwlock is always the global_tsid_lock.

  Access methods generally assert that the caller already holds the
  appropriate lock:

   - before accessing any global data, hold at least the rdlock.

   - before accessing a specific SIDNO in a Gtid_set or Owned_gtids
     (e.g., calling Gtid_set::_add_gtid(Gtid)), hold either the rdlock
     and the SIDNO's mutex lock; or the wrlock.  If you need to hold
     multiple mutexes, they must be acquired in order of increasing
     SIDNO.

   - before starting an operation that needs to access all SIDs
     (e.g. Gtid_set::to_string()), hold the wrlock.

  The access type (read/write) does not matter; the write lock only
  implies that the entire data structure is locked whereas the read
  lock implies that everything except TSID-specific data is locked.
*/
class Gtid_state {
 public:
  /**
    Constructs a new Gtid_state object.

    @param _tsid_lock Read-write lock that protects updates to the
    number of TSIDs.
    @param _tsid_map Tsid_map used by this Gtid_state.
  */
  Gtid_state(Checkable_rwlock *_tsid_lock, Tsid_map *_tsid_map)
      : tsid_lock(_tsid_lock),
        tsid_map(_tsid_map),
        tsid_locks(tsid_lock),
        lost_gtids(tsid_map, tsid_lock),
        executed_gtids(tsid_map, tsid_lock),
        gtids_only_in_table(tsid_map, tsid_lock),
        previous_gtids_logged(tsid_map, tsid_lock),
        owned_gtids(tsid_lock),
        commit_group_sidnos(key_memory_Gtid_state_group_commit_sidno) {}
  /**
    Add @@GLOBAL.SERVER_UUID to this binlog's Tsid_map.

    This can't be done in the constructor because the constructor is
    invoked at server startup before SERVER_UUID is initialized.

    The caller must hold the read lock or write lock on tsid_locks
    before invoking this function.

    @retval 0 Success
    @retval 1 Error (out of memory or IO error).
  */
  int init();
  /**
    Reset the state and persistor after RESET BINARY LOGS AND GTIDS:
    remove all logged and lost gtids, but keep owned gtids as they are.

    The caller must hold the write lock on tsid_lock before calling
    this function.

    @param  thd Thread requesting to reset the persistor

    @retval 0  Success
    @retval -1 Error
  */
  int clear(THD *thd);
  /**
    Returns true if the given GTID is logged.

    @param gtid The Gtid to check.

    @retval true The gtid is logged in the binary log.
    @retval false The gtid is not logged in the binary log.
  */
  bool is_executed(const Gtid &gtid) const {
    DBUG_TRACE;
    tsid_locks.assert_owner(gtid.sidno);
    bool ret = executed_gtids.contains_gtid(gtid);
    return ret;
  }
  /**
    Returns true if GTID is owned, otherwise returns 0.

    @param gtid The Gtid to check.
    @return true if some thread owns the gtid, false if the gtid is
    not owned
  */
  bool is_owned(const Gtid &gtid) const {
    return !owned_gtids.is_owned_by(gtid, 0);
  }
#ifdef MYSQL_SERVER
  /**
    Acquires ownership of the given GTID, on behalf of the given thread.

    The caller must lock the SIDNO before invoking this function.

    @param thd The thread that will own the GTID.
    @param gtid The Gtid to acquire ownership of.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status acquire_ownership(THD *thd, const Gtid &gtid);
  /**
    This function updates both the THD and the Gtid_state to reflect that
    the transaction set of transactions has ended, and it does this for the
    whole commit group (by following the thd->next_to_commit pointer).

    It will:

    - Clean up the thread state when a thread owned GTIDs is empty.
    - Release ownership of all GTIDs owned by the THDs. This removes
      the GTIDs from Owned_gtids and clears the ownership status in the
      THDs object.
    - Add the owned GTIDs to executed_gtids when the thread is committing.
    - Decrease counters of GTID-violating transactions.
    - Send a broadcast on the condition variable for every sidno for
      which we released ownership.

    @param first_thd The first thread of the group commit that needs GTIDs to
                     be updated.
  */
  void update_commit_group(THD *first_thd);
  /**
    Remove the GTID owned by thread from owned GTIDs, stating that
    thd->owned_gtid was committed.

    This will:
     - remove owned GTID from owned_gtids;
     - remove all owned GTIDS from thd->owned_gtid and thd->owned_gtid_set;

    @param thd Thread for which owned gtids are updated.
  */
  void update_on_commit(THD *thd);
  /**
    Update the state after the given thread has rollbacked.

    This will:
     - release ownership of all GTIDs owned by the THD;
     - remove owned GTID from owned_gtids;
     - remove all owned GTIDS from thd->owned_gtid and thd->owned_gtid_set;
     - send a broadcast on the condition variable for every sidno for
       which we released ownership.

    @param thd Thread for which owned gtids are updated.
  */
  void update_on_rollback(THD *thd);

  /**
    Acquire anonymous ownership.

    The caller must hold either tsid_lock.rdlock or
    tsid_lock.wrlock. (The caller must have taken the lock and checked
    that gtid_mode!=ON before calling this function, or else the
    gtid_mode could have changed to ON by a concurrent SET GTID_MODE.)
  */
  void acquire_anonymous_ownership() {
    DBUG_TRACE;
    tsid_lock->assert_some_lock();
    assert(global_gtid_mode.get() != Gtid_mode::ON);
#ifndef NDEBUG
    int32 new_value =
#endif
        ++atomic_anonymous_gtid_count;
    DBUG_PRINT("info",
               ("atomic_anonymous_gtid_count increased to %d", new_value));
    assert(new_value >= 1);
    return;
  }

  /// Release anonymous ownership.
  void release_anonymous_ownership() {
    DBUG_TRACE;
    tsid_lock->assert_some_lock();
    assert(global_gtid_mode.get() != Gtid_mode::ON);
#ifndef NDEBUG
    int32 new_value =
#endif
        --atomic_anonymous_gtid_count;
    DBUG_PRINT("info",
               ("atomic_anonymous_gtid_count decreased to %d", new_value));
    assert(new_value >= 0);
    return;
  }

  /// Return the number of clients that hold anonymous ownership.
  int32 get_anonymous_ownership_count() { return atomic_anonymous_gtid_count; }

  /**
    Increase the global counter when starting a GTID-violating
    transaction having GTID_NEXT=AUTOMATIC.
  */
  void begin_automatic_gtid_violating_transaction() {
    DBUG_TRACE;
    assert(global_gtid_mode.get() <= Gtid_mode::OFF_PERMISSIVE);
    assert(get_gtid_consistency_mode() != GTID_CONSISTENCY_MODE_ON);
#ifndef NDEBUG
    int32 new_value =
#endif
        ++atomic_automatic_gtid_violation_count;
    DBUG_PRINT(
        "info",
        ("ongoing_automatic_gtid_violating_transaction_count increased to %d",
         new_value));
    assert(new_value >= 1);
    return;
  }

  /**
    Decrease the global counter when ending a GTID-violating
    transaction having GTID_NEXT=AUTOMATIC.
  */
  void end_automatic_gtid_violating_transaction() {
    DBUG_TRACE;
#ifndef NDEBUG
    global_tsid_lock->rdlock();
    assert(global_gtid_mode.get() <= Gtid_mode::OFF_PERMISSIVE);
    assert(get_gtid_consistency_mode() != GTID_CONSISTENCY_MODE_ON);
    global_tsid_lock->unlock();
    int32 new_value =
#endif
        --atomic_automatic_gtid_violation_count;
    DBUG_PRINT(
        "info",
        ("ongoing_automatic_gtid_violating_transaction_count decreased to %d",
         new_value));
    assert(new_value >= 0);
    return;
  }

  /**
    Return the number of ongoing GTID-violating transactions having
    GTID_NEXT=AUTOMATIC.
  */
  int32 get_automatic_gtid_violating_transaction_count() {
    return atomic_automatic_gtid_violation_count;
  }

  /**
    Increase the global counter when starting a GTID-violating
    transaction having GTID_NEXT=ANONYMOUS.
  */
  void begin_anonymous_gtid_violating_transaction() {
    DBUG_TRACE;
    assert(global_gtid_mode.get() != Gtid_mode::ON);
    assert(get_gtid_consistency_mode() != GTID_CONSISTENCY_MODE_ON);
#ifndef NDEBUG
    int32 new_value =
#endif
        ++atomic_anonymous_gtid_violation_count;
    DBUG_PRINT("info", ("atomic_anonymous_gtid_violation_count increased to %d",
                        new_value));
    assert(new_value >= 1);
    return;
  }

  /**
    Decrease the global counter when ending a GTID-violating
    transaction having GTID_NEXT=ANONYMOUS.
  */
  void end_anonymous_gtid_violating_transaction() {
    DBUG_TRACE;
#ifndef NDEBUG
    global_tsid_lock->rdlock();
    assert(global_gtid_mode.get() != Gtid_mode::ON);
    assert(get_gtid_consistency_mode() != GTID_CONSISTENCY_MODE_ON);
    global_tsid_lock->unlock();
    int32 new_value =
#endif
        --atomic_anonymous_gtid_violation_count;
    DBUG_PRINT(
        "info",
        ("ongoing_anonymous_gtid_violating_transaction_count decreased to %d",
         new_value));
    assert(new_value >= 0);
    return;
  }

  void end_gtid_violating_transaction(THD *thd);

  /**
    Return the number of ongoing GTID-violating transactions having
    GTID_NEXT=AUTOMATIC.
  */
  int32 get_anonymous_gtid_violating_transaction_count() {
    return atomic_anonymous_gtid_violation_count;
  }

  /**
    Increase the global counter when starting a call to
    WAIT_FOR_EXECUTED_GTID_SET.
  */
  void begin_gtid_wait() {
    DBUG_TRACE;
    assert(global_gtid_mode.get() != Gtid_mode::OFF);
#ifndef NDEBUG
    int32 new_value =
#endif
        ++atomic_gtid_wait_count;
    DBUG_PRINT("info", ("atomic_gtid_wait_count changed from %d to %d",
                        new_value - 1, new_value));
    assert(new_value >= 1);
    return;
  }

  /**
    Decrease the global counter when ending a call to
    WAIT_FOR_EXECUTED_GTID_SET.
  */
  void end_gtid_wait() {
    DBUG_TRACE;
    assert(global_gtid_mode.get() != Gtid_mode::OFF);
#ifndef NDEBUG
    int32 new_value =
#endif
        --atomic_gtid_wait_count;
    DBUG_PRINT("info", ("atomic_gtid_wait_count changed from %d to %d",
                        new_value + 1, new_value));
    assert(new_value >= 0);
    return;
  }

  /**
    Return the number of clients that have an ongoing call to
    WAIT_FOR_EXECUTED_GTID_SET.
  */
  int32 get_gtid_wait_count() { return atomic_gtid_wait_count; }

#endif  // ifdef MYSQL_SERVER
  /**
    Computes the next available GNO.

    @param sidno The GTID's SIDNO.

    @retval -1 The range of GNOs was exhausted (i.e., more than 1<<63-1
    GTIDs with the same UUID have been generated).
    @retval >0 The GNO for the GTID.
  */
  rpl_gno get_automatic_gno(rpl_sidno sidno) const;

 private:
  /**
    The next_free_gno map contains next_free_gno for recorded sidnos.
    The next_free_gno variable will be set with the supposed next free GNO
    every time a new GNO is delivered automatically or when a transaction is
    rolled back, releasing a GNO smaller than the last one delivered.
    It was introduced in an optimization of Gtid_state::get_automatic_gno and
    Gtid_state::generate_automatic_gtid functions.

    Locking scheme

    This variable can be read and modified in four places:
    - During server startup, holding global_tsid_lock.wrlock;
    - By a client thread holding global_tsid_lock.wrlock
      when executing RESET BINARY LOGS AND GTIDS
    - By a client thread calling MYSQL_BIN_LOG::write_transaction function
    (often the group commit FLUSH stage leader). It will call
      Gtid_state::generate_automatic_gtid, that will acquire
      global_tsid_lock.rdlock and lock_sidno(get_server_sidno()) when getting a
      new automatically generated GTID;
    - By a client thread rolling back, holding global_tsid_lock.rdlock
      and lock_sidno(get_server_sidno()).
  */
  std::unordered_map<rpl_sidno, rpl_gno> next_free_gno_map;

 public:
  using Locked_sidno_set = cs::index::Locked_sidno_set;
  using Tsid = mysql::gtid::Tsid;
  /**
    Return the last executed GNO for a given SIDNO, e.g.
    for the following set: UUID:1-10, UUID:12, UUID:15-20
    20 will be returned.

    @param sidno The GTID's SIDNO.

    @retval The GNO or 0 if set is empty.
  */
  rpl_gno get_last_executed_gno(rpl_sidno sidno) const;

  /**
    Generates the GTID (or ANONYMOUS, if GTID_MODE = OFF or
    OFF_PERMISSIVE) for the THD, and acquires ownership.
    Before this function, the caller needs to assign sidnos for automatic
    transactions and lock sidno_set (see specify_transaction_sidno).

    @param thd The thread.
    @param specified_sidno Externally generated sidno.
    @param specified_gno   Externally generated gno.
    @see Locked_sidno_set

    @return RETURN_STATUS_OK or RETURN_STATUS_ERROR. Error can happen
    in case of out of memory or if the range of GNOs was exhausted.
  */
  enum_return_status generate_automatic_gtid(THD *thd,
                                             rpl_sidno specified_sidno = 0,
                                             rpl_gno specified_gno = 0);

  /// @brief Determines sidno for thd transaction. In case transaction
  /// is automatic, sidno is generated and added to sidno_set for future
  /// locking (after all transactions from binlog commit group have been added)
  /// @details The usage scheme is as follows: transaction for the binlog
  /// commit group are assigned a sidno. Sidnos are added to sidno_set in this
  /// function.
  /// Afterwards, sidno_set must be locked by the caller. This operation must
  /// be performed before the call to generate_automatic_gtid
  /// @returns sidno specified for thd transaction
  rpl_sidno specify_transaction_sidno(THD *thd,
                                      Gtid_state::Locked_sidno_set &sidno_set);

  /// Locks a mutex for the given SIDNO.
  void lock_sidno(rpl_sidno sidno) { tsid_locks.lock(sidno); }
  /// Unlocks a mutex for the given SIDNO.
  void unlock_sidno(rpl_sidno sidno) { tsid_locks.unlock(sidno); }
  /// Broadcasts updates for the given SIDNO.
  void broadcast_sidno(rpl_sidno sidno) { tsid_locks.broadcast(sidno); }
  /// Assert that we own the given SIDNO.
  void assert_sidno_lock_owner(rpl_sidno sidno) const {
    tsid_locks.assert_owner(sidno);
  }
#ifdef MYSQL_SERVER
  /**
    Wait for a signal on the given SIDNO.

    NOTE: This releases a lock!

    This requires that the caller holds a read lock on tsid_lock.  It
    will release the lock before waiting; neither global_tsid_lock nor
    the mutex lock on SIDNO will not be held when this function
    returns.

    @param thd THD object of the caller.
    @param sidno Sidno to wait for.
    @param[in] abstime The absolute point in time when the wait times
    out and stops, or NULL to wait indefinitely.
    @param[in] update_thd_status when true updates the stage info with
    the new wait condition, when false keeps the current stage info.

    @retval false Success.
    @retval true Failure: either timeout or thread was killed.  If
    thread was killed, the error has been generated.
  */
  bool wait_for_sidno(THD *thd, rpl_sidno sidno, struct timespec *abstime,
                      bool update_thd_status = true);
  /**
    This is only a shorthand for wait_for_sidno, which contains
    additional debug printouts and assertions for the case when the
    caller waits for one specific GTID.
  */
  bool wait_for_gtid(THD *thd, const Gtid &gtid,
                     struct timespec *abstime = nullptr);
  /**
    Wait until the given Gtid_set is included in @@GLOBAL.GTID_EXECUTED.

    @param thd The calling thread.
    @param gtid_set Gtid_set to wait for.
    @param[in] timeout The maximum number of milliseconds that the
    function should wait, or 0 to wait indefinitely.
    @param[in] update_thd_status when true updates the stage info with
    the new wait condition, when false keeps the current stage info.

    @retval false Success.
    @retval true Failure: either timeout or thread was killed.  If
    thread was killed, the error has been generated.
   */
  bool wait_for_gtid_set(THD *thd, Gtid_set *gtid_set, double timeout,
                         bool update_thd_status = true);
#endif  // ifdef MYSQL_SERVER
  /**
    Locks one mutex for each SIDNO where the given Gtid_set has at
    least one GTID.  Locks are acquired in order of increasing SIDNO.
  */
  void lock_sidnos(const Gtid_set *set);
  /**
    Unlocks the mutex for each SIDNO where the given Gtid_set has at
    least one GTID.
  */
  void unlock_sidnos(const Gtid_set *set);
  /**
    Broadcasts the condition variable for each SIDNO where the given
    Gtid_set has at least one GTID.
  */
  void broadcast_sidnos(const Gtid_set *set);
  /**
    Ensure that owned_gtids, executed_gtids, lost_gtids, gtids_only_in_table,
    previous_gtids_logged and tsid_locks have room for at least as many SIDNOs
    as tsid_map.

    This function must only be called in one place:
    Tsid_map::add_tsid().

    Requires that the write lock on tsid_locks is held.  If any object
    needs to be resized, then the lock will be temporarily upgraded to
    a write lock and then degraded to a read lock again; there will be
    a short period when the lock is not held at all.

    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status ensure_sidno();

  /**
    Adds the given Gtid_set to lost_gtids and executed_gtids.
    lost_gtids must be a subset of executed_gtids.
    purged_gtid and executed_gtid sets are appended with the argument set
    provided the latter is disjoint with gtid_executed owned_gtids.

    Requires that the caller holds global_tsid_lock.wrlock.

    @param[in,out] gtid_set The gtid_set to add. If the gtid_set
    does not start with a plus sign (starts_with_plus is false),
    @@GLOBAL.GTID_PURGED will be removed from the gtid_set.
    @param starts_with_plus If true, the gtid_set passed is required to
    be disjoint from @@GLOBAL.GTID_PURGED; if false, the gtid_set passed
    is required to be a superset of @@GLOBAL.GTID_PURGED.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
   */
  enum_return_status add_lost_gtids(Gtid_set *gtid_set, bool starts_with_plus);

  /** Updates previously logged GTID set before writing to table. */
  void update_prev_gtids(Gtid_set *write_gtid_set);

  /// Return a pointer to the Gtid_set that contains the lost gtids.
  const Gtid_set *get_lost_gtids() const { return &lost_gtids; }
  /*
    Return a pointer to the Gtid_set that contains the stored gtids
    in gtid_executed table.
  */
  const Gtid_set *get_executed_gtids() const { return &executed_gtids; }
  /*
    Return a pointer to the Gtid_set that contains the stored gtids
    only in gtid_executed table, not in binlog files.
  */
  const Gtid_set *get_gtids_only_in_table() const {
    return &gtids_only_in_table;
  }
  /*
    Return a pointer to the Gtid_set that contains the previous stored
    gtids in the last binlog file.
  */
  const Gtid_set *get_previous_gtids_logged() const {
    return &previous_gtids_logged;
  }
  /// Return a pointer to the Owned_gtids that contains the owned gtids.
  const Owned_gtids *get_owned_gtids() const { return &owned_gtids; }
  /// Return the server's SIDNO
  rpl_sidno get_server_sidno() const { return server_sidno; }
  /// Return the server's TSID
  const Tsid &get_server_tsid() const {
    return global_tsid_map->sidno_to_tsid(server_sidno);
  }

  /// @brief Increments atomic_automatic_tagged_gtid_session_count
  void increase_gtid_automatic_tagged_count() {
    ++atomic_automatic_tagged_gtid_session_count;
  }

  /// @brief Decrements atomic_automatic_tagged_gtid_session_count
  void decrease_gtid_automatic_tagged_count() {
    --atomic_automatic_tagged_gtid_session_count;
  }

  /// @brief Checks whether there are ongoing sessions executing transactions
  /// with GTID_NEXT set to AUTOMATIC:tag
  /// @return true in case there are ongoing sessions with GTID_NEXT set
  /// to automatic, tagged
  bool is_any_session_assigning_automatic_tagged_gtids() {
    return atomic_automatic_tagged_gtid_session_count.load() != 0;
  }

#ifndef NDEBUG
  /**
    Debug only: Returns an upper bound on the length of the string
    generated by to_string(), not counting '\0'.  The actual length
    may be shorter.
  */
  size_t get_max_string_length() const {
    return owned_gtids.get_max_string_length() +
           executed_gtids.get_string_length() + lost_gtids.get_string_length() +
           gtids_only_in_table.get_string_length() +
           previous_gtids_logged.get_string_length() + 150;
  }
  /// Debug only: Generate a string in the given buffer and return the length.
  int to_string(char *buf) const {
    char *p = buf;
    p += sprintf(p, "Executed GTIDs:\n");
    p += executed_gtids.to_string(p);
    p += sprintf(p, "\nOwned GTIDs:\n");
    p += owned_gtids.to_string(p);
    p += sprintf(p, "\nLost GTIDs:\n");
    p += lost_gtids.to_string(p);
    p += sprintf(p, "\nGTIDs only_in_table:\n");
    p += lost_gtids.to_string(p);
    return (int)(p - buf);
  }
  /// Debug only: return a newly allocated string, or NULL on out-of-memory.
  char *to_string() const {
    char *str = (char *)my_malloc(key_memory_Gtid_state_to_string,
                                  get_max_string_length(), MYF(MY_WME));
    to_string(str);
    return str;
  }
  /// Debug only: print this Gtid_state to stdout.
  void print() const {
    char *str = to_string();
    printf("%s", str);
    my_free(str);
  }
#endif
  /**
    Print this Gtid_state to the trace file if debug is enabled; no-op
    otherwise.
  */
  void dbug_print(const char *text [[maybe_unused]] = "") const {
#ifndef NDEBUG
    tsid_lock->assert_some_wrlock();
    char *str = to_string();
    DBUG_PRINT("info", ("%s%s%s", text, *text ? ": " : "", str));
    my_free(str);
#endif
  }
  /**
    Save gtid owned by the thd into executed_gtids variable
    and gtid_executed table.

    @param thd Session to commit
    @retval
        0    OK
    @retval
        -1   Error
  */
  int save(THD *thd);
  /**
    Insert the gtid set into table.

    @param gtid_set  contains a set of gtid, which holds
                     the sidno and the gno.

    @retval
      0    OK
    @retval
      -1   Error
  */
  int save(const Gtid_set *gtid_set);
  /**
    Save the set of gtids logged in the last binlog into gtid_executed table.

    @retval
      0    OK
    @retval
      -1   Error
  */
  int save_gtids_of_last_binlog_into_table();
  /**
    Fetch gtids from gtid_executed table and store them into
    gtid_executed set.

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int read_gtid_executed_from_table();
  /**
    Compress the gtid_executed table, read each row by the PK(sid, gno_start)
    in increasing order, compress the first consecutive gtids range
    (delete consecutive gtids from the second consecutive gtid, then
    update the first gtid) within a single transaction.

    @param  thd Thread requesting to compress the table

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int compress(THD *thd);
#ifdef MYSQL_SERVER
  /**
    Push a warning to client if user is modifying the gtid_executed
    table explicitly by a non-XA transaction. Push an error to client
    if user is modifying it explicitly by a XA transaction.

    @param thd Thread requesting to access the table
    @param table The table is being accessed.

    @retval 0 No warning or error was pushed to the client.
    @retval 1 Push a warning to client.
    @retval 2 Push an error to client.
  */
  int warn_or_err_on_modify_gtid_table(THD *thd, Table_ref *table);
#endif

 private:
  /**
    Remove the GTID owned by thread from owned GTIDs.

    This will:

    - Clean up the thread state if the thread owned GTIDs is empty.
    - Release ownership of all GTIDs owned by the THD. This removes
      the GTID from Owned_gtids and clears the ownership status in the
      THD object.
    - Add the owned GTID to executed_gtids if the is_commit flag is
      set.
    - Decrease counters of GTID-violating transactions.
    - Send a broadcast on the condition variable for every sidno for
      which we released ownership.

    @param[in] thd Thread for which owned gtids are updated.
    @param[in] is_commit If true, the update is for a commit (not a rollback).
  */
  void update_gtids_impl(THD *thd, bool is_commit);
#ifdef HAVE_GTID_NEXT_LIST
  /// Lock all SIDNOs owned by the given THD.
  void lock_owned_sidnos(const THD *thd);
#endif
  /// Unlock all SIDNOs owned by the given THD.
  void unlock_owned_sidnos(const THD *thd);
  /// Broadcast the condition for all SIDNOs owned by the given THD.
  void broadcast_owned_sidnos(const THD *thd);
  /// Read-write lock that protects updates to the number of TSIDs.
  mutable Checkable_rwlock *tsid_lock;
  /// The Tsid_map used by this Gtid_state.
  mutable Tsid_map *tsid_map;
  /// Contains one mutex/cond pair for every SIDNO.
  Mutex_cond_array tsid_locks;
  /**
    The set of GTIDs that existed in some previously purged binary log.
    This is always a subset of executed_gtids.
  */
  Gtid_set lost_gtids;
  /*
    The set of GTIDs that has been executed and
    stored into gtid_executed table.
  */
  Gtid_set executed_gtids;
  /*
    The set of GTIDs that exists only in gtid_executed table, not in
    binlog files.
  */
  Gtid_set gtids_only_in_table;
  /* The previous GTIDs in the last binlog. */
  Gtid_set previous_gtids_logged;
  /// The set of GTIDs that are owned by some thread.
  Owned_gtids owned_gtids;
  /// The SIDNO for this server.
  rpl_sidno server_sidno;

  /// The number of anonymous transactions owned by any client.
  std::atomic<int32> atomic_anonymous_gtid_count{0};
  /// The number of GTID-violating transactions that use GTID_NEXT=AUTOMATIC.
  std::atomic<int32> atomic_automatic_gtid_violation_count{0};
  /// The number of GTID-violating transactions that use GTID_NEXT=AUTOMATIC.
  std::atomic<int32> atomic_anonymous_gtid_violation_count{0};
  /// The number of clients that are executing
  /// WAIT_FOR_EXECUTED_GTID_SET.
  std::atomic<int32> atomic_gtid_wait_count{0};
  /// The number of sessions that have GTID_NEXT set to AUTOMATIC with tag
  /// assigned
  std::atomic<int64_t> atomic_automatic_tagged_gtid_session_count{0};

  /// Used by unit tests that need to access private members.
#ifdef FRIEND_OF_GTID_STATE
  friend FRIEND_OF_GTID_STATE;
#endif

  /**
    This is a sub task of update_on_rollback responsible only to handle
    the case of a thread that needs to skip GTID operations when it has
    "failed to commit".

    Administrative commands [CHECK|REPAIR|OPTIMIZE|ANALYZE] TABLE
    are written to the binary log even when they fail.  When the
    commands fail, they will call update_on_rollback; later they will
    write the binary log.  But we must not do any of the things in
    update_gtids_impl if we are going to write the binary log.  So
    these statements set the skip_gtid_rollback flag, which tells
    update_on_rollback to return early.  When the statements are
    written to the binary log they will call update_on_commit as
    usual.

    @param[in] thd - Thread to be evaluated.

    @retval true The transaction should skip the rollback, false otherwise.
  */
  bool update_gtids_impl_check_skip_gtid_rollback(THD *thd);
  /**
    This is a sub task of update_gtids_impl responsible only to handle
    the case of a thread that owns nothing and does not violate GTID
    consistency.

    If the THD does not own anything, there is nothing to do, so we can do an
    early return of the update process. Except if there is a GTID consistency
    violation; then we need to decrease the counter, so then we can continue
    executing inside update_gtids_impl.

    @param[in] thd - Thread to be evaluated.
    @retval true The transaction can be skipped because it owns nothing and
                 does not violate GTID consistency, false otherwise.
  */
  bool update_gtids_impl_do_nothing(THD *thd);
  /**
    This is a sub task of update_gtids_impl responsible only to evaluate
    if the thread is committing in the middle of a statement by checking
    THD's is_commit_in_middle_of_statement flag.

    This flag is true for anonymous transactions, when the
    'transaction' has been split into multiple transactions in the
    binlog, and the present transaction is not the last one.

    This means two things:

    - We should not release anonymous ownership in case
      gtid_next=anonymous.  If we did, it would be possible for user
      to set GTID_MODE=ON from a concurrent transaction, making it
      impossible to commit the current transaction.

    - We should not decrease the counters for GTID-violating
      statements.  If we did, it would be possible for a concurrent
      client to set ENFORCE_GTID_CONSISTENCY=ON despite there is an
      ongoing transaction that violates GTID consistency.

    The flag is set in two cases:

     1. We are committing the statement cache when there are more
        changes in the transaction cache.

        This happens either because a single statement in the
        beginning of a transaction updates both transactional and
        non-transactional tables, or because we are committing a
        non-transactional update in the middle of a transaction when
        binlog_direct_non_transactional_updates=1.

        In this case, the flag is set further down in this function.

     2. The statement is one of the special statements that may
        generate multiple transactions: CREATE...SELECT, DROP TABLE,
        DROP DATABASE. See comment for THD::owned_gtid in
        sql/sql_class.h.

        In this case, the THD::is_commit_in_middle_of_statement flag
        is set by the caller and the flag becomes true here.

    @param[in] thd - Thread to be evaluated.
    @return The value of thread's is_commit_in_middle_of_statement flag.
  */
  bool update_gtids_impl_begin(THD *thd);
  /**
    Handle the case that the thread own a set of GTIDs.

    This is a sub task of update_gtids_impl responsible only to handle
    the case of a thread with a set of GTIDs being updated.

    - Release ownership of the GTIDs owned by the THD. This removes
      the GTID from Owned_gtids and clears the ownership status in the
      THD object.
    - Add the owned GTIDs to executed_gtids if the is_commit flag is set.
    - Send a broadcast on the condition variable for the sidno which we
      released ownership.

    @param[in] thd - Thread for which owned GTID set should be updated.
    @param[in] is_commit - If the thread is being updated by a commit.
  */
  void update_gtids_impl_own_gtid_set(THD *thd, bool is_commit);
  /**
    Lock a given sidno of a transaction being updated.

    This is a sub task of update_gtids_impl responsible only to lock the
    sidno of the GTID being updated.

    @param[in] sidno - The sidno to be locked.
  */
  void update_gtids_impl_lock_sidno(rpl_sidno sidno);
  /**

    Locks the sidnos of all the GTIDs of the commit group starting on the
    transaction passed as parameter.

    This is a sub task of update_commit_group responsible only to lock the
    sidno(s) of the GTID(s) being updated.

    The function should follow thd->next_to_commit to lock all sidnos of all
    transactions being updated in a group.

    @param[in] thd - Thread that owns the GTID(s) to be updated or leader
                     of the commit group in the case of a commit group
                     update.
  */
  void update_gtids_impl_lock_sidnos(THD *thd);
  /**
    Handle the case that the thread own a single non-anonymous GTID.

    This is a sub task of update_gtids_impl responsible only to handle
    the case of a thread with a single non-anonymous GTID being updated
    either for commit or rollback.

    - Release ownership of the GTID owned by the THD. This removes
      the GTID from Owned_gtids and clears the ownership status in the
      THD object.
    - Add the owned GTID to executed_gtids if the is_commit flag is set.
    - Send a broadcast on the condition variable for the sidno which we
      released ownership.

    @param[in] thd - Thread to be updated that owns single non-anonymous GTID.
    @param[in] is_commit - If the thread is being updated by a commit.
  */
  void update_gtids_impl_own_gtid(THD *thd, bool is_commit);
  /**
    Unlock a given sidno after broadcasting its changes.

    This is a sub task of update_gtids_impl responsible only to
    unlock the sidno of the GTID being updated after broadcasting
    its changes.

    @param[in] sidno - The sidno to be broadcasted and unlocked.
  */
  void update_gtids_impl_broadcast_and_unlock_sidno(rpl_sidno sidno);
  /**
    Unlocks all locked sidnos after broadcasting their changes.

    This is a sub task of update_commit_group responsible only to
    unlock the sidno(s) of the GTID(s) being updated after broadcasting
    their changes.
  */
  void update_gtids_impl_broadcast_and_unlock_sidnos();
  /**
    Handle the case that the thread owns ANONYMOUS GTID.

    This is a sub task of update_gtids_impl responsible only to handle
    the case of a thread with an ANONYMOUS GTID being updated.

    - Release ownership of the anonymous GTID owned by the THD and clears
      the ownership status in the THD object.
    - Decrease counters of GTID-violating transactions.

    @param[in] thd - Thread to be updated that owns anonymous GTID.
    @param[in,out] more_trx - If the 'transaction' has been split into
                              multiple transactions in the binlog.
                              This is firstly assigned with the return of
                              Gtid_state::update_gtids_impl_begin function, and
                              its value can be set to true when
                              Gtid_state::update_gtids_impl_anonymous_gtid
                              detects more content on the transaction cache.
  */
  void update_gtids_impl_own_anonymous(THD *thd, bool *more_trx);
  /**
    Handle the case that the thread owns nothing.

    This is a sub task of update_gtids_impl responsible only to handle
    the case of a thread that owns nothing being updated.

    There are two cases when this happens:
    - Normally, it is a rollback of an automatic transaction, so
      the is_commit is false and gtid_next=automatic.
    - There is also a corner case. This case may happen for a transaction
      that uses GTID_NEXT=AUTOMATIC, and violates GTID_CONSISTENCY, and
      commits changes to the database, but does not write to the binary log,
      so that no GTID is generated. An example is CREATE TEMPORARY TABLE
      inside a transaction when binlog_format=row. Despite the thread does
      not own anything, the GTID consistency violation makes it necessary to
      call end_gtid_violating_transaction. Therefore
      MYSQL_BIN_LOG::gtid_end_transaction will call
      gtid_state->update_on_commit in this case, and subsequently we will
      reach this case.

    @param[in] thd - Thread to be updated that owns anonymous GTID.
  */
  void update_gtids_impl_own_nothing(THD *thd);
  /**
    Handle the final part of update_gtids_impl.

    This is a sub task of update_gtids_impl responsible only to handle
    the call to end_gtid_violating_transaction function when there is no
    more transactions split after the current transaction.

    @param[in] thd - Thread for which owned GTID is updated.
    @param[in] more_trx - This is the value returned from
                          Gtid_state::update_gtids_impl_begin and can be
                          changed for transactions owning anonymous GTID at
                          Gtid_state::update_gtids_impl_own_anonymous.
  */
  void update_gtids_impl_end(THD *thd, bool more_trx);
  /**
    This array is used by Gtid_state_update_gtids_impl* functions.

    The array items (one per sidno of the tsid_map) will be set as true for
    each sidno that requires to be locked when updating a set of GTIDs
    (at Gtid_set::update_gtids_impl_lock_sidnos).

    The array items will be set false at
    Gtid_set::update_gtids_impl_broadcast_and_unlock_sidnos.

    It is used to so that lock, unlock, and broadcast operations are only
    called once per sidno per commit group, instead of once per transaction.

    Its access is protected by:
    - global_tsid_lock->wrlock when growing and cleaning up;
    - MYSQL_BIN_LOG::LOCK_commit when setting true/false on array items.
  */
  Prealloced_array<bool, 8> commit_group_sidnos;
  /**
    Ensure that commit_group_sidnos have room for the SIDNO passed as
    parameter.

    This function must only be called in one place:
    Gtid_state::ensure_sidno().

    @param sidno The SIDNO.
    @return RETURN_STATUS_OK or RETURN_STATUS_REPORTED_ERROR.
  */
  enum_return_status ensure_commit_group_sidnos(rpl_sidno sidno);
};

/*
  BUG# #18089914 - REFACTORING: RENAME GROUP TO GTID
  changed AUTOMATIC_GROUP to AUTOMATIC_GTID
  changed ANONYMOUS_GROUP to ANONYMOUS_GTID
  changed INVALID_GROUP   to INVALID_GTID
  changed UNDEFINED_GROUP to UNDEFINED_GTID
  changed GTID_GROUPto ASSIGNED_GTID
  changed NOT_YET_DETERMINED_GROUP to NOT_YET_DETERMINED_GTID
*/

/**
  Enumeration of different types of values for Gtid_specification,
  i.e, the different internal states that @@session.gtid_next can be in.
*/
enum enum_gtid_type {
  /**
    Specifies that the GTID has not been generated yet; it will be
    generated on commit.  It will depend on the GTID_MODE: if
    GTID_MODE<=OFF_PERMISSIVE, then the transaction will be anonymous;
    if GTID_MODE>=ON_PERMISSIVE, then the transaction will be assigned
    a new GTID.

    In the latter case, the Gtid_specification may hold a tag. Then,
    the new GTID will be generated with that tag.

    AUTOMATIC_GTID with an empty tag is the default value:
    thd->variables.gtid_next has this state when GTID_NEXT="AUTOMATIC".

    It is important that AUTOMATIC_GTID==0 so that the default value
    for thd->variables->gtid_next.type is AUTOMATIC_GTID.
  */
  AUTOMATIC_GTID = 0,
  /**
    Specifies that the transaction has been assigned a GTID (UUID:NUMBER).

    thd->variables.gtid_next has this state when GTID_NEXT="UUID:NUMBER".

    This is the state of GTID-transactions replicated to the slave.
  */
  ASSIGNED_GTID,
  /**
    Specifies that the transaction is anonymous, i.e., it does not
    have a GTID and will never be assigned one.

    thd->variables.gtid_next has this state when GTID_NEXT="ANONYMOUS".

    This is the state of any transaction generated on a pre-GTID
    server, or on a server with GTID_MODE==OFF.
  */
  ANONYMOUS_GTID,
  /**
    GTID_NEXT is set to this state after a transaction with
    GTID_NEXT=='UUID:NUMBER' is committed.

    This is used to protect against a special case of unsafe
    non-transactional updates.

    Background: Non-transactional updates are allowed as long as they
    are sane.  Non-transactional updates must be single-statement
    transactions; they must not be mixed with transactional updates in
    the same statement or in the same transaction.  Since
    non-transactional updates must be logged separately from
    transactional updates, a single mixed statement would generate two
    different transactions.

    Problematic case: Consider a transaction, Tx1, that updates two
    transactional tables on the master, t1 and t2. Then slave (s1) later
    replays Tx1. However, t2 is a non-transactional table at s1. As such, s1
    will report an error because it cannot split Tx1 into two different
    transactions. Had no error been reported, then Tx1 would be split into Tx1
    and Tx2, potentially causing severe harm in case some form of fail-over
    procedure is later engaged by s1.

    To detect this case on the slave and generate an appropriate error
    message rather than causing an inconsistency in the GTID state, we
    do as follows.  When committing a transaction that has
    GTID_NEXT==UUID:NUMBER, we set GTID_NEXT to UNDEFINED_GTID.  When
    the next part of the transaction is being processed, an error is
    generated, because it is not allowed to execute a transaction when
    GTID_NEXT==UNDEFINED.  In the normal case, the error is not
    generated, because there will always be a Gtid_log_event after the
    next transaction.
  */
  UNDEFINED_GTID,
  /**
    GTID_NEXT is set to this state by the slave applier thread when it
    reads a Format_description_log_event that does not originate from
    this server.

    Background: when the slave applier thread reads a relay log that
    comes from a pre-GTID master, it must preserve the transactions as
    anonymous transactions, even if GTID_MODE>=ON_PERMISSIVE.  This
    may happen, e.g., if the relay log was received when master and
    slave had GTID_MODE=OFF or when master and slave were old, and the
    relay log is applied when slave has GTID_MODE>=ON_PERMISSIVE.

    So the slave thread should set GTID_NEXT=ANONYMOUS for the next
    transaction when it starts to process an old binary log.  However,
    there is no way for the slave to tell if the binary log is old,
    until it sees the first transaction.  If the first transaction
    begins with a Gtid_log_event, we have the GTID there; if it begins
    with query_log_event, row events, etc, then this is an old binary
log.  So at the time the binary log begins, we just set
    GTID_NEXT=NOT_YET_DETERMINED_GTID.  If it remains
    NOT_YET_DETERMINED when the next transaction begins,
    gtid_pre_statement_checks will automatically turn it into an
    anonymous transaction.  If a Gtid_log_event comes across before
    the next transaction starts, then the Gtid_log_event will just set
    GTID_NEXT='UUID:NUMBER' accordingly.
  */
  NOT_YET_DETERMINED_GTID,
  /**
    The applier sets GTID_NEXT this state internally, when it
    processes an Anonymous_gtid_log_event on a channel having
    ASSIGN_GTIDS_TO_ANONYMOUS_TRANSACTIONS, before it calls
    set_gtid_next.  This tells set_gtid_next to generate a new,
    sequential GTID, and acquire ownership for it.  Thus, this state
    is only used for a very brief period of time.  It is not
    user-visible.
  */
  PRE_GENERATE_GTID,
};
/// Global state of GTIDs.
extern Gtid_state *gtid_state;

/**
  This struct represents a specification of a GTID for a statement to
  be executed: either "AUTOMATIC", "AUTOMATIC:<tag>", "ANONYMOUS" or "TSID:GNO".

  This is a POD. It has to be a POD because it is used in THD::variables.
*/
struct Gtid_specification {
  // Constants used in gtid specification
  static constexpr auto str_automatic = "AUTOMATIC";
  static constexpr auto str_automatic_tagged = "AUTOMATIC:";
  static constexpr auto str_automatic_sep = ":";
  static constexpr auto str_pre_generated = "PRE_GENERATE_GTID";
  static constexpr auto str_not_yet_determined = "NOT_YET_DETERMINED";
  static constexpr auto str_anonymous = "ANONYMOUS";

  using Tsid = mysql::gtid::Tsid;
  using Tag = mysql::gtid::Tag;
  using Tag_plain = mysql::gtid::Tag_plain;
  /// The type of this GTID
  enum_gtid_type type;
  /**
    The GTID:
    { SIDNO, GNO } if type == GTID;
    { 0, 0 } if type == AUTOMATIC or ANONYMOUS.
  */
  Gtid gtid;

  /// @brief Tag defined by the user while specifying GTID_NEXT="AUTOMATIC:TAG".
  /// We must store here the information about tag, because automatic
  /// tagged GTID does not have sidno assigned
  Tag_plain automatic_tag;

  /// @brief Prints automatic tag specification to the given buffer
  /// @param[in,out] buf Buffer to write to, must be allocated
  /// @return The number of bytes written to the buffer
  std::size_t automatic_to_string(char *buf) const;

  /// Set the type to ASSIGNED_GTID and SIDNO, GNO to the given values.
  void set(rpl_sidno sidno, rpl_gno gno) {
    gtid.set(sidno, gno);
    type = ASSIGNED_GTID;
    automatic_tag.clear();
  }

  /// @brief Helper function indicating whether this is to-be-generated GTID
  /// @param[in] type Type of the GTID
  /// @retval true This GTID will be generated
  /// @retval false Other type of the GTID
  static bool is_automatic(const enum_gtid_type &type) {
    return type == AUTOMATIC_GTID;
  }
  /// @brief Helper function indicating whether this is to-be-generated GTID
  /// @retval true This GTID will be generated
  /// @retval false Other type of the GTID
  bool is_automatic() const { return is_automatic(type); }

  /// @brief Helper function indicating whether this is an undefined GTID
  /// @return Returns true for undefined GTIDs
  bool is_undefined() const { return type == UNDEFINED_GTID; }

  /// @brief Helper function indicating whether this is an assigned GTID
  /// @return Returns true for assigned GTIDs
  bool is_assigned() const { return type == ASSIGNED_GTID; }

  /// @brief Returns tag object generated from internal tag data
  /// @return Tag object
  Tag generate_tag() const;

  /// @brief Helper function indicating whether this is to-be-generated GTID
  ///   with a tag assigned
  /// @retval true This GTID will be generated with assigned tag
  /// @retval false Other type of the GTID
  bool is_automatic_tagged() const;

  /// Set the type to ASSIGNED_GTID and TSID, GNO to the given Gtid.
  /// @brief gtid_param GTID to copy from
  void set(const Gtid &gtid_param) { set(gtid_param.sidno, gtid_param.gno); }
  /// @brief Set the type to AUTOMATIC_GTID.
  void set_automatic() {
    type = AUTOMATIC_GTID;
    automatic_tag.clear();
  }
  /// @brief Copy spec from other
  /// @param[in] other Pattern to copy from
  void set(const Gtid_specification &other);

  /// Set the type to ANONYMOUS_GTID.
  void set_anonymous() {
    type = ANONYMOUS_GTID;
    automatic_tag.clear();
  }
  /// Set the type to NOT_YET_DETERMINED_GTID.
  void set_not_yet_determined() {
    type = NOT_YET_DETERMINED_GTID;
    automatic_tag.clear();
  }
  /// Set to undefined. Must only be called if the type is ASSIGNED_GTID.
  void set_undefined() {
    assert(type == ASSIGNED_GTID);
    type = UNDEFINED_GTID;
    automatic_tag.clear();
  }
  /// Return true if this Gtid_specification is equal to 'other'.
  bool equals(const Gtid_specification &other) const {
    return (type == other.type &&
            (type != ASSIGNED_GTID || gtid.equals(other.gtid)));
  }
  /**
    Return true if this Gtid_specification is a ASSIGNED_GTID with the
    same TSID, GNO as 'other_gtid'.
  */
  bool equals(const Gtid &other_gtid) const {
    return type == ASSIGNED_GTID && gtid.equals(other_gtid);
  }
#ifdef MYSQL_SERVER
  /**
    Parses the given string and stores in this Gtid_specification.

    @param tsid_map tsid_map to use when converting TSID to a sidno.
    @param text The text to parse
    @return operation status
  */
  [[NODISCARD]] mysql::utils::Return_status parse(Tsid_map *tsid_map,
                                                  const char *text);

  /// @brief Returns true if the given string is a valid Gtid_specification.
  /// @param[in] text Textual representation of the GTID specification
  static bool is_valid(const char *text);

  /// @brief Returns true if the given string is a tagged Gtid_specification.
  /// @param[in] text Textual representation of the GTID specification
  static bool is_tagged(const char *text);
#endif
  static const int MAX_TEXT_LENGTH = Gtid::MAX_TEXT_LENGTH;
  /**
    Writes this Gtid_specification to the given string buffer.

    @param tsid_map Tsid_map to use if the type of this
    Gtid_specification is ASSIGNED_GTID.
    @param [out] buf The buffer
    @param need_lock If true, this function acquires global_tsid_lock
    before looking up the sidno in tsid_map, and then releases it. If
    false, this function asserts that the lock is held by the caller.
    @retval The number of characters written.
  */
  int to_string(const Tsid_map *tsid_map, char *buf,
                bool need_lock = false) const;
  /**
    Writes this Gtid_specification to the given string buffer.

    @param tsid TSID to use if the type of this Gtid_specification is
    ASSIGNED_GTID.  Can be NULL if this Gtid_specification is
    ANONYMOUS_GTID or AUTOMATIC_GTID.
    @param[out] buf The buffer
    @retval The number of characters written.
  */
  int to_string(const Tsid &tsid, char *buf) const;

#ifndef NDEBUG
  /// Debug only: print this Gtid_specification to stdout.
  void print() const {
    char buf[MAX_TEXT_LENGTH + 1];
    to_string(global_tsid_map, buf);
    printf("%s\n", buf);
  }
#endif
  /**
    Print this Gtid_specification to the trace file if debug is
    enabled; no-op otherwise.
  */
  void dbug_print(const char *text [[maybe_unused]] = "",
                  bool need_lock [[maybe_unused]] = false) const {
#ifndef NDEBUG
    char buf[MAX_TEXT_LENGTH + 1];
    to_string(global_tsid_map, buf, need_lock);
    DBUG_PRINT("info", ("%s%s%s", text, *text ? ": " : "", buf));
#endif
  }
};

static_assert(std::is_trivial_v<Gtid_specification>);
static_assert(std::is_standard_layout_v<Gtid_specification>);

/**
  Indicates if a statement should be skipped or not. Used as return
  value from gtid_before_statement.
*/
enum enum_gtid_statement_status {
  /// Statement can execute.
  GTID_STATEMENT_EXECUTE,
  /// Statement should be cancelled.
  GTID_STATEMENT_CANCEL,
  /**
    Statement should be skipped, but there may be an implicit commit
    after the statement if gtid_commit is set.
  */
  GTID_STATEMENT_SKIP
};

#ifdef MYSQL_SERVER

/**
  Check if current transaction should be skipped, that is, if GTID_NEXT
  was already logged.

  @param  thd    The calling thread.

  @retval true   Transaction was already logged.
  @retval false  Transaction must be executed.
*/
bool is_already_logged_transaction(const THD *thd);

/**
  Perform GTID-related checks before executing a statement:

  - Check that the current statement does not contradict
    enforce_gtid_consistency.

  - Check that there is no implicit commit in a transaction when
    GTID_NEXT==UUID:NUMBER.

  - Change thd->variables.gtid_next.type to ANONYMOUS_GTID if it is
    currently NOT_YET_DETERMINED_GTID.

  - Check whether the statement should be cancelled.

  @param thd THD object for the session.

  @retval GTID_STATEMENT_EXECUTE The normal case: the checks
  succeeded, and statement can execute.

  @retval GTID_STATEMENT_CANCEL The checks failed; an
  error has be generated and the statement must stop.

  @retval GTID_STATEMENT_SKIP The checks succeeded, but the GTID has
  already been executed (exists in GTID_EXECUTED). So the statement
  must not execute; however, if there are implicit commits, then the
  implicit commits must execute.
*/
enum_gtid_statement_status gtid_pre_statement_checks(THD *thd);

/**
  Perform GTID-related checks before executing a statement, but after
  executing an implicit commit before the statement, if any:

  If gtid_next=anonymous, but the thread does not hold anonymous
  ownership, then acquire anonymous ownership. (Do this only if this
  is not an 'innocent' statement, i.e., SET/SHOW/DO/SELECT that does
  not invoke a stored function.)

  It is important that this is done after the implicit commit, because
  the implicit commit may release anonymous ownership.

  @param thd THD object for the session

  @retval false Success.

  @retval true Error. Error can happen if GTID_MODE=ON.  The error has
  been reported by (a function called by) this function.
*/
bool gtid_pre_statement_post_implicit_commit_checks(THD *thd);

/**
  Acquire ownership of the given Gtid_specification.

  The Gtid_specification must be of type ASSIGNED_GTID or ANONYMOUS_GTID.

  The caller must hold global_tsid_lock (normally the rdlock).  The
  lock may be temporarily released and acquired again. In the end,
  the lock will be released, so the caller should *not* release the
  lock.

  The function will try to acquire ownership of the GTID and update
  both THD::gtid_next, Gtid_state::owned_gtids, and
  THD::owned_gtid / THD::owned_sid.

  @param thd The thread that acquires ownership.

  @param spec The Gtid_specification.

  @retval false Success: either we have acquired ownership of the
  GTID, or it is already included in GTID_EXECUTED and will be
  skipped.

  @retval true Failure; the thread was killed or an error occurred.
  The error has been reported using my_error.
*/
bool set_gtid_next(THD *thd, const Gtid_specification &spec);
#ifdef HAVE_GTID_NEXT_LIST
int gtid_acquire_ownership_multiple(THD *thd);
#endif

/**
  Return sidno for a given tsid, see Tsid_map::add_sid() for details.
*/
rpl_sidno get_sidno_from_global_tsid_map(const mysql::gtid::Tsid &tsid);

/**
  Return Tsid for a given sidno on the global_tsid_map.
  See Tsid_map::sidno_to_tsid() for details.
*/
const mysql::gtid::Tsid &get_tsid_from_global_tsid_map(rpl_sidno sidno);

/**
  Return last gno for a given sidno, see
  Gtid_state::get_last_executed_gno() for details.
*/
rpl_gno get_last_executed_gno(rpl_sidno sidno);

void gtid_set_performance_schema_values(const THD *thd);

/**
  If gtid_next=ANONYMOUS or NOT_YET_DETERMINED, but the thread does
  not hold anonymous ownership, acquire anonymous ownership.

  @param thd Thread.

  @retval true Error (can happen if gtid_mode=ON and
  gtid_next=anonymous). The error has already been reported using
  my_error.

  @retval false Success.
*/
bool gtid_reacquire_ownership_if_anonymous(THD *thd);

/**
  The function commits or rolls back the gtid state if it needs to.
  It's supposed to be invoked at the end of transaction commit or
  rollback, as well as as at the end of XA prepare.

  @param thd       Thread context
  @param needs_to  The actual work will be done when the parameter is true
  @param do_commit When true the gtid state changes are committed, otherwise
                   they are rolled back.
*/

inline void gtid_state_commit_or_rollback(THD *thd, bool needs_to,
                                          bool do_commit) {
  if (needs_to) {
    if (do_commit)
      gtid_state->update_on_commit(thd);
    else
      gtid_state->update_on_rollback(thd);
  }
}

#endif  // ifdef MYSQL_SERVER

#endif /* RPL_GTID_H_INCLUDED */
